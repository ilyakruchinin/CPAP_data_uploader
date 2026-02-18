# SD Card Management

## Overview
The SD Card Manager (`SDCardManager.cpp/.h`) provides exclusive access control to the SD card, coordinating between the CPAP machine and the uploader. Implements safe handoff mechanisms and ensures reliable operation.

## Core Responsibilities

### Exclusive Access Control
- **Bus arbitration**: Coordinate access between CPAP and uploader
- **Activity detection**: Monitor CPAP usage via TrafficMonitor
- **Safe handoff**: Ensure CPAP always gets priority access
- **Timeout protection**: Prevent uploader from hogging SD card

### Mount/Unmount Management
```cpp
bool takeControl() {
    // Mount SD card with exclusive access
    if (!SD_MMC.begin("/sd", true)) {
        LOG_ERROR("[SDCard] Failed to mount SD card");
        return false;
    }
    
    // Set card to 1-bit mode for compatibility
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
    
    _hasControl = true;
    return true;
}
```

## Key Features

### Safe Mounting
- **1-bit mode**: Ensures compatibility with CPAP hardware
- **Exclusive access**: Prevents concurrent access conflicts
- **Error recovery**: Retry mechanisms for mount failures
- **Status tracking**: Monitor mount state and health

### Handoff Coordination
```cpp
void releaseControl() {
    if (_hasControl) {
        // Unmount SD card cleanly
        SD_MMC.end();
        _hasControl = false;
        
        LOG("[SDCard] Control released - CPAP can now access card");
    }
}
```

### Activity Monitoring Integration
- **TrafficMonitor integration**: Detect CPAP activity
- **Immediate release**: Drop SD access on CPAP activity
- **Timeout enforcement**: Respect `EXCLUSIVE_ACCESS_MINUTES`
- **Cooldown periods**: Allow CPAP time to reclaim access

## Configuration Dependencies

### Timing Parameters
- `EXCLUSIVE_ACCESS_MINUTES`: Maximum uploader hold time (default: 5)
- `COOLDOWN_MINUTES`: Minimum pause between sessions (default: 10)
- `INACTIVITY_SECONDS`: Required silence before access (default: 125)

### Hardware Configuration
- **Pin mapping**: SD_CLK, SD_CMD, SD_D0 pins
- **Card type**: SDHC support up to 32GB
- **Speed mode**: Default SDMMC frequency
- **Power management**: Card power control

## Operations

### 1. Take Control
```cpp
bool takeControl() {
    // Verify bus is idle before taking control
    if (!trafficMonitor.isIdleFor(config->getInactivitySeconds())) {
        LOG_WARN("[SDCard] Bus active - cannot take control");
        return false;
    }
    
    // Mount and configure SD card
    if (!SD_MMC.begin("/sd", true)) {
        return false;
    }
    
    _hasControl = true;
    _controlTakenAt = millis();
    return true;
}
```

### 2. Maintain Control
```cpp
bool maintainControl() {
    if (!_hasControl) return false;
    
    // Check timeout
    if (millis() - _controlTakenAt > config->getExclusiveAccessMinutes() * 60000) {
        LOG_WARN("[SDCard] Exclusive access timeout - releasing control");
        releaseControl();
        return false;
    }
    
    // Check for CPAP activity
    if (trafficMonitor.isBusy()) {
        LOG_WARN("[SDCard] CPAP activity detected - releasing control");
        releaseControl();
        return false;
    }
    
    return true;
}
```

### 3. Release Control
```cpp
void releaseControl() {
    if (_hasControl) {
        // Ensure all files are closed
        SD_MMC.end();
        _hasControl = false;
        
        LOG("[SDCard] Control released for CPAP access");
    }
}
```

## Error Handling

### Mount Failures
- **Retry logic**: Multiple mount attempts with delays
- **Card detection**: Verify card is physically present
- **Format issues**: Handle corrupted filesystems gracefully
- **Hardware problems**: Detect and report card failures

### Access Conflicts
- **Activity detection**: Immediate release on CPAP access
- **Timeout protection**: Never hold card longer than configured
- **Error recovery**: Clean unmount even on errors
- **Status reporting**: Clear error messages for debugging

### Data Integrity
- **Flush operations**: Ensure data written before release
- **File handle management**: Prevent open file leaks
- **Corruption detection**: Basic filesystem checks
- **Safe unmount**: Proper unmount sequence

## Performance Characteristics

### Mount Timing
- **Cold mount**: 500ms - 2 seconds (depends on card)
- **Warm mount**: 100ms - 500ms (card already powered)
- **Unmount**: <100ms (clean shutdown)
- **File operations**: Varies by file size and card speed

### Compatibility
- **Card types**: SDHC up to 32GB, SDXC limited support
- **Speed classes**: Class 10 recommended for performance
- **Formats**: FAT32 (standard), exFAT (limited)
- **Hardware**: ESP32 SDMMC peripheral

## Integration Points

### Upload System
- **FileUploader**: Primary consumer of SD access
- **UploadFSM**: Coordinates access timing
- **TrafficMonitor**: Provides activity detection
- **Configuration**: Supplies timing parameters

### CPAP Machine
- **Bus sharing**: Hardware-level bus arbitration
- **Priority access**: CPAP always has priority
- **Activity detection**: Via CS_SENSE signal
- **Compatibility**: Works with ResMed Series 9/10/11

### Web Interface
- **Status reporting**: SD card access state
- **Error messages**: Mount and access errors
- **Diagnostics**: Card health and performance
- **Manual control**: Override for testing (development)

## Safety Mechanisms

### Timeout Protection
```cpp
bool isTimeoutExceeded() {
    return _hasControl && 
           (millis() - _controlTakenAt > config->getExclusiveAccessMinutes() * 60000);
}
```

### Activity Detection
```cpp
bool shouldReleaseForActivity() {
    return _hasControl && trafficMonitor.isBusy();
}
```

### Error Recovery
- **Graceful degradation**: Continue operation with reduced functionality
- **Automatic retry**: Recover from transient failures
- **Safe defaults**: Conservative behavior on uncertainty
- **User notification**: Clear error reporting

## Debugging Features

### Detailed Logging
- **Mount operations**: All mount/unmount events
- **Access timing**: When control is taken/released
- **Error conditions**: Detailed failure information
- **Performance metrics**: Operation timing data

### Status Monitoring
- **Card presence**: Physical card detection
- **Mount state**: Current filesystem status
- **Access duration**: Time control held
- **Activity detection**: CPAP access attempts

### Diagnostic Tools
- **Card information**: Size, format, speed class
- **Performance tests**: Read/write speed testing
- **Error history**: Recent error patterns
- **Compatibility checks**: Hardware validation

## Configuration Examples

### Default Settings
```ini
EXCLUSIVE_ACCESS_MINUTES = 5
COOLDOWN_MINUTES = 10
INACTIVITY_SECONDS = 125
```

### Conservative Settings
```ini
EXCLUSIVE_ACCESS_MINUTES = 3
COOLDOWN_MINUTES = 15
INACTIVITY_SECONDS = 180
```

### Aggressive Settings
```ini
EXCLUSIVE_ACCESS_MINUTES = 10
COOLDOWN_MINUTES = 5
INACTIVITY_SECONDS = 60
```

## Troubleshooting

### Common Issues
- **Mount failures**: Check card format, seating, compatibility
- **Access conflicts**: Verify inactivity detection, timing
- **Performance issues**: Check card speed class, fragmentation
- **Data corruption**: Ensure clean unmount, power stability

### Recovery Procedures
- **Card reformat**: Last resort for filesystem corruption
- **Hardware reset**: Power cycle to clear hardware issues
- **Configuration reset**: Return to default timing values
- **Firmware update**: Address compatibility issues
