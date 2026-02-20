# Traffic Monitoring

## Overview
The Traffic Monitor (`TrafficMonitor.cpp/.h`) detects SD card bus activity using the ESP32's PCNT (Pulse Counter) peripheral. This enables "smart mode" uploads by sensing when the CPAP machine is actively accessing the SD card.

## Hardware Interface

### GPIO Configuration
- **Pin**: GPIO 33 (CS_SENSE signal from SD card slot)
- **Mode**: INPUT (floating, relies on external pull-ups)
- **Peripheral**: PCNT Unit 0, Channel 0

### Signal Characteristics
- **Both edges counted**: Rising and falling edges
- **Glitch filter**: ~100ns minimum pulse width
- **No external pull-up**: Uses SD bus pull-ups

## Core Features

### Pulse Counting
```cpp
pcnt_config_t pcntConfig = {};
pcntConfig.pulse_gpio_num = _pin;
pcntConfig.pos_mode = PCNT_COUNT_INC;   // Count rising edges
pcntConfig.neg_mode = PCNT_COUNT_INC;   // Count falling edges
```

### Activity Detection
- **Sample window**: 100ms intervals
- **Activity threshold**: Any pulses counted = active
- **Idle tracking**: Cumulative consecutive idle time
- **Reset on activity**: Idle counter resets on any detected activity

### Sampling System
```cpp
struct Sample {
    uint32_t timestamp;     // When sample was taken
    uint16_t pulseCount;     // Pulses detected in window
    bool active;            // True if pulses > 0
};
```

## Key Operations

### Real-time Monitoring
```cpp
void update() {
    // Called every main loop iteration
    if (millis() - _lastSampleTime >= 100) {
        takeSample();
        _lastSampleTime = millis();
    }
}
```

### Activity Queries
```cpp
bool isBusy();                    // Was last sample active?
bool isIdleFor(uint32_t ms);      // Idle for at least ms?
uint32_t getConsecutiveIdleMs();  // Current idle streak
void resetIdleTracking();         // Reset idle counter
```

### Statistical Data
- **Sample buffer**: Rolling 300 samples (5 minutes at 1Hz)
- **Longest idle streak**: Maximum consecutive idle time
- **Active/Idle counts**: Total samples in each category
- **Pulse accumulation**: Per-second pulse counting

## Smart Mode Integration

### Upload Triggering
The monitor enables smart mode uploads:
1. **LISTENING**: Wait for `INACTIVITY_SECONDS` of idle time
2. **ACQUIRING**: Take SD control when safe
3. **UPLOADING**: Perform upload operations
4. **RELEASING**: Return SD control to CPAP

### Activity Detection Logic
```cpp
bool canStartUpload() {
    return trafficMonitor.isIdleFor(config->getInactivitySeconds());
}
```

### Safety Mechanisms
- **Immediate abort**: If activity detected during upload
- **Short sessions**: Respect `EXCLUSIVE_ACCESS_MINUTES` limit
- **Frequent checks**: Activity monitoring continues during upload

## Performance Characteristics

### CPU Usage
- **Minimal overhead**: PCNT hardware does counting
- **Interrupt-free**: No ISR overhead, uses polling
- **Low memory**: ~3KB for sample buffer

### Timing Accuracy
- **100ms resolution**: Sufficient for CPAP activity patterns
- **Hardware timing**: PCNT provides precise edge detection
- **No drift**: Hardware-based timing, not software-dependent

### Sensitivity
- **Single edge detection**: Can detect minimal activity
- **No false positives**: Requires actual signal changes
- **Noise immunity**: Hardware glitch filter rejects spurious signals

## Web Interface Integration

### SD Activity Monitor
- **Real-time graph**: Shows last 5 minutes of activity
- **Live updates**: Auto-refreshes every second
- **Statistics**: Idle/active percentages, longest idle streak
- **Debug view**: Raw pulse counts and timing data

### API Endpoints
```cpp
// JSON status for web interface
String getActivityStatus() {
    return "{\"active\":" + String(isBusy()) + 
           ",\"idleMs\":" + String(getConsecutiveIdleMs()) + "}";
}
```

## Configuration Dependencies

### Timing Parameters
- `INACTIVITY_SECONDS`: Bus silence required before upload (default: 125)
- Sample window: Fixed at 100ms (hardware optimal)

### Integration Points
- **UploadFSM**: Uses activity detection for state transitions
- **WebServer**: Provides activity monitoring interface
- **Config**: Supplies inactivity threshold

## Error Handling

### Hardware Failures
- **PCNT init failure**: Logged, system continues without monitoring
- **GPIO issues**: Fallback to scheduled mode if monitoring unavailable
- **Signal loss**: Treated as idle (safe default)

### Edge Cases
- **Continuous activity**: Never triggers upload (prevents interference)
- **No activity**: Triggers upload immediately when in threshold met
- **Intermittent activity**: Resets idle timer appropriately

## Debugging Features

### Detailed Logging
- **Sample timing**: When samples are taken
- **Pulse counts**: Raw edge detection results
- **State changes**: Idle/active transitions
- **Statistics**: Cumulative tracking data

### Diagnostic Tools
- **Activity graph**: Visual representation in web interface
- **Raw data access**: Sample buffer for analysis
- **Performance metrics**: CPU and memory usage tracking

## Usage Patterns

### Smart Mode Operation
1. System enters LISTENING state
2. Monitor continuously samples bus activity
3. When `INACTIVITY_SECONDS` reached without activity
4. FSM transitions to ACQUIRING state
5. Upload proceeds with continued monitoring
6. Any activity during upload triggers immediate release

### Scheduled Mode
- Monitor still runs for statistics/logging
- Upload timing determined by schedule, not activity
- Activity detection still provides safety abort capability
