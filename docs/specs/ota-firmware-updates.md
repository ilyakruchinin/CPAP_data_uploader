# OTA Firmware Updates

## Overview
The OTA Manager (`OTAManager.cpp/.h`) provides Over-The-Air firmware update capabilities through the web interface. Supports secure downloads, verification, and rollback-safe installation.

## Architecture

### Update Sources
- **Local upload**: Direct file upload via web interface
- **Remote download**: Download from URL (GitHub releases)
- **Verification**: MD5 checksum validation
- **Safety**: Partition-based updates with rollback capability

### Partition Layout
```
OTA Partition Layout (1.5MB app space):
┌─────────────────┐
│   ota_0         │ ← Currently running (or ota_1)
├─────────────────┤
│   ota_1         │ ← Target for update
├─────────────────┤
│   bootloader    │
├─────────────────┤
│   partition table│
└─────────────────┘
```

## Core Features

### Web Interface Integration
```cpp
void handleUpdate() {
    // Method 1: Local file upload
    if (server->hasArg("upload")) {
        handleLocalUpload();
    }
    
    // Method 2: Remote URL download
    else if (server->hasArg("url")) {
        handleRemoteDownload();
    }
}
```

### Secure Download Process
```cpp
bool downloadFromUrl(const String& url) {
    HTTPClient http;
    http.begin(url);
    
    // Get file info
    int fileSize = http.getSize();
    if (fileSize < 0) {
        LOG_ERROR("[OTA] Failed to get file size");
        return false;
    }
    
    // Verify size limits
    if (fileSize > MAX_OTA_SIZE) {
        LOG_ERROR("[OTA] File too large for OTA");
        return false;
    }
    
    // Stream to update partition
    return streamToPartition(http, fileSize);
}
```

## Update Process Flow

### 1. Pre-flight Checks
```cpp
bool canStartUpdate() {
    // Verify sufficient space
    size_t appSize = ESP.getFreeSketchSpace();
    if (appSize < MIN_UPDATE_SPACE) {
        LOG_ERROR("[OTA] Insufficient space for update");
        return false;
    }
    
    // Verify battery/power (if applicable)
    // Verify network stability
    // Verify no upload in progress
    
    return true;
}
```

### 2. Download/Upload
```cpp
bool performUpdate(Stream& stream, size_t size) {
    // Begin update to next partition
    Update.begin(size);
    
    // Write data in chunks
    size_t written = 0;
    while (written < size) {
        uint8_t buffer[1024];
        size_t bytesRead = stream.readBytes(buffer, sizeof(buffer));
        
        if (Update.write(buffer, bytesRead) != bytesRead) {
            LOG_ERROR("[OTA] Write failed");
            Update.abort();
            return false;
        }
        
        written += bytesRead;
        feedUploadHeartbeat(); // Prevent watchdog timeout
    }
    
    return true;
}
```

### 3. Verification
```cpp
bool verifyUpdate(const String& expectedMd5) {
    if (expectedMd5.length() > 0) {
        String actualMd5 = Update.md5String();
        if (actualMd5 != expectedMd5) {
            LOG_ERROR("[OTA] MD5 mismatch");
            Update.abort();
            return false;
        }
    }
    return true;
}
```

### 4. Installation
```cpp
bool installUpdate() {
    if (!Update.end(true)) { // true = verify after write
        LOG_ERROR("[OTA] Update verification failed");
        return false;
    }
    
    LOG("[OTA] Update successful, rebooting...");
    delay(1000);
    ESP.restart();
    return true; // Never reached
}
```

## Safety Features

### Rollback Protection
- **Partition switching**: Updates alternate partition
- **Boot verification**: Bootloader verifies partition validity
- **Automatic rollback**: Returns to previous partition if boot fails
- **Manual rollback**: Web interface for partition selection

### Error Recovery
```cpp
void handleUpdateError() {
    Update.abort();
    LOG_ERROR("[OTA] Update aborted due to error");
    
    // Clean up temporary files
    // Reset update state
    // Notify user of failure
}
```

### Validation Checks
- **Size limits**: Prevent oversized updates
- **Format verification**: Ensure valid firmware image
- **Checksum validation**: MD5 verification when provided
- **Version checking**: Prevent downgrade (optional)

## Configuration

### Update Settings
```cpp
#define MAX_OTA_SIZE 1310720        // Maximum firmware size
#define MIN_UPDATE_SPACE 1500000    // Minimum free space required
#define UPDATE_TIMEOUT_MS 300000    // 5 minutes max update time
```

### Security Options
- **TLS verification**: HTTPS downloads with certificate validation
- **Checksum required**: MD5 verification mandatory for remote downloads
- **Size limits**: Prevent oversized malicious uploads
- **Access control**: Web interface authentication (future)

## Performance Characteristics

### Timing
- **Local upload**: 2-5 minutes (depends on network)
- **Remote download**: 5-15 minutes (depends on connection)
- **Verification**: <30 seconds
- **Installation**: <10 seconds

### Memory Usage
- **Base**: ~2KB for OTA manager
- **Buffer**: 1KB for download streaming
- **Peak**: During update verification
- **Temporary**: Update partition space

## Integration Points

### Web Interface
- **Update page**: `/ota` endpoint
- **Progress tracking**: Real-time update progress
- **Error reporting**: Detailed failure information
- **Version display**: Current and available versions

### Main Application
- **Update trigger**: Manual initiation via web interface
- **State management**: Pause uploads during update
- **Reboot handling**: Clean shutdown before restart
- **Recovery**: Handle update failures gracefully

### Build System
- **OTA builds**: Special builds with OTA partition layout
- **Version management**: Automatic version reporting
- **Compatibility**: Ensure update compatibility
- **Signing**: Future support for signed updates

## Security Considerations

### Download Security
- **HTTPS only**: Remote downloads use TLS
- **Certificate validation**: Verify server identity
- **Checksum verification**: MD5 validation of firmware
- **Size limits**: Prevent malicious oversized files

### Installation Security
- **Partition verification**: Bootloader validates firmware
- **Rollback capability**: Return to known good version
- **Access control**: Web interface restrictions
- **Update signing**: Future support for cryptographic signatures

## Troubleshooting

### Common Issues
- **Insufficient space**: Clear unused data, use standard build
- **Network failures**: Check internet connectivity, retry
- **Corrupted download**: Verify checksum, re-download
- **Boot failures**: Automatic rollback to previous version

### Recovery Procedures
- **Manual rollback**: Select previous partition via web interface
- **Serial recovery**: Flash firmware via USB if needed
- **Factory reset**: Clear configuration and start fresh
- **Support contact**: Report persistent issues

### Debug Information
- **Update logs**: Detailed update process logging
- **Partition info**: Current and available partitions
- **Error codes**: Specific failure reasons
- **Performance metrics**: Download speed and timing

## Usage Examples

### Local Update
1. Access `http://cpap.local/ota`
2. Choose "Method 1: Local Upload"
3. Select firmware file from computer
4. Click "Upload and Install"
5. Wait for completion and automatic reboot

### Remote Update
1. Access `http://cpap.local/ota`
2. Choose "Method 2: Remote Download"
3. Enter firmware URL (GitHub release)
4. Optionally enter MD5 checksum
5. Click "Download and Install"
6. Wait for download, verification, and installation

## Future Enhancements

### Security Improvements
- **Signed updates**: Cryptographic signature verification
- **Key management**: Secure key distribution
- **Authentication**: User authentication for updates
- **Audit logging**: Update history and verification

### Usability Improvements
- **Auto-update**: Automatic checking for new versions
- **Delta updates**: Smaller differential updates
- **Scheduled updates**: Update during maintenance windows
- **Update notifications**: Web interface notifications
