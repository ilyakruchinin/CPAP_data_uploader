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
- Monitors contiguous heap (`max_alloc`) for fragmentation
- Automatic soft-reboot when heap is insufficient for next operation
- Fast-boot path skips stabilization delays on software resets
- Staged backend processing (SMB then Cloud) to optimize memory usage

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

### Heap Recovery
```cpp
const uint32_t SD_MOUNT_MIN_ALLOC = 45000;
if (ESP.getMaxAllocHeap() < SD_MOUNT_MIN_ALLOC) {
    LOG("[FSM] Heap fragmented — fast-reboot to restore heap");
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
