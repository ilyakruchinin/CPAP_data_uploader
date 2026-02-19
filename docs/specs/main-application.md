# Main Application Controller

## Overview
The main application controller (`main.cpp`) orchestrates the entire CPAP data uploader system, managing initialization, the upload state machine, web interface, and system lifecycle events.

## Core Responsibilities

### System Initialization
- Hardware pin configuration and peripheral setup
- Component initialization (WiFi, SD card, uploaders, web server)
- Configuration loading and validation
- Fast-boot detection using `esp_reset_reason()`

### Upload State Machine (FSM)
- Implements the core upload logic with states: IDLE, LISTENING, ACQUIRING, UPLOADING, RELEASING, COOLDOWN
- Supports both "smart" and "scheduled" upload modes
- Manages SD card exclusive access with timeout handling
- Coordinates between SMB and Cloud upload passes

### Heap Management & Recovery
- **Always-reboot strategy**: `handleReleasing()` unconditionally calls `esp_restart()` after every upload session
- Fast-boot path (`ESP_RST_SW`) skips cold-boot stabilization delays and Smart Wait
- Each session runs exactly one backend — single-backend cycling prevents concurrent SMB+TLS memory pressure

### Web Interface Integration
- Progressive Web App (PWA) with pre-allocated buffers
- Real-time status monitoring and manual controls
- OTA firmware updates
- SD activity monitoring and log viewing

## Key Features

### Fast-Boot Detection
```cpp
bool fastBoot = (esp_reset_reason() == ESP_RST_SW);
if (fastBoot) {
    LOG("[FastBoot] Software reset — skipping stabilization + Smart Wait");
}
```

### Always-Reboot After Upload
```cpp
void handleReleasing() {
    sdManager.releaseControl();
    // Always reboot to restore contiguous heap and advance backend cycling
    LOGF("[FSM] Upload session complete — soft-reboot");
    delay(200);
    esp_restart();
}
```

### Upload Modes
- **Smart Mode**: Continuous loop, uploads recent data anytime, old data only in upload window
- **Scheduled Mode**: Only uploads within configured time window, enters IDLE between windows

## Global Objects
- `Config` - Configuration management
- `SDCardManager` - SD card access control
- `WiFiManager` - Network connectivity
- `FileUploader` - Upload orchestration
- `TrafficMonitor` - SD bus activity detection
- `TestWebServer` - Web interface (optional)
- `OTAManager` - OTA updates (optional)

## Lifecycle
1. **Boot**: Detect reset reason, initialize components
2. **Setup**: Load config, connect WiFi, start web server
3. **Loop**: Run FSM, handle web requests, monitor heap
4. **Upload**: Scan folders, upload to backends, save state
5. **Recovery**: Auto-reboot on heap fragmentation if needed

## Configuration Dependencies
- All timing parameters (inactivity, exclusive access, cooldown)
- Upload mode and window settings
- Backend endpoints (SMB, Cloud, WebDAV)
- Power management settings

## Integration Points
- **UploadFSM**: Core state machine logic
- **FileUploader**: Backend upload orchestration
- **TrafficMonitor**: SD activity detection for smart mode
- **WiFiManager**: Network connectivity for cloud operations
- **TestWebServer**: User interface and manual controls
