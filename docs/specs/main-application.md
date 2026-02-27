# Main Application Controller

## Overview
The main application controller (`main.cpp`) orchestrates the entire CPAP data uploader system, managing initialization, the upload state machine, web interface, and system lifecycle events.

## Core Responsibilities

### System Initialization
- **Early power reduction**: CPU throttled to 80 MHz and Bluetooth memory released as the very first instructions in `setup()`
- Hardware pin configuration and peripheral setup
- Component initialization (WiFi, SD card, uploaders, web server)
- Configuration loading and validation
- Fast-boot detection using `esp_reset_reason()`
- **WiFi power optimization**: 802.11b disabled, TX power set before association, power saving enabled after connect
- **DFS configuration**: `esp_pm_configure()` enables automatic CPU scaling (80-160 MHz)

### Upload State Machine (FSM)
- Implements the core upload logic with states: IDLE, LISTENING, ACQUIRING, UPLOADING, RELEASING, COOLDOWN
- Supports both "smart" and "scheduled" upload modes
- Manages SD card exclusive access with timeout handling
- Coordinates between SMB and Cloud upload passes

### Heap Management & Recovery
- **Conditional-reboot strategy**: `handleReleasing()` calls `esp_restart()` only when real upload work was done
- When `FileUploader` returns `NOTHING_TO_DO` (all backends fully synced), FSM skips the reboot and enters `COOLDOWN` directly via `g_nothingToUpload` flag — prevents endless reboot cycles when data is already synced
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

### Conditional Reboot in handleReleasing
```cpp
void handleReleasing() {
    sdManager.releaseControl();
    if (g_nothingToUpload) {          // pre-flight found no work
        g_nothingToUpload = false;
        cooldownStartedAt = millis();
        transitionTo(UploadState::COOLDOWN);  // no reboot
        return;
    }
    // Real upload completed — reboot to restore heap
    delay(200);
    esp_restart();
}
```

### UploadResult::NOTHING_TO_DO
- Returned by `FileUploader::uploadWithExclusiveAccess()` when pre-flight scan finds no pending work for any configured backend
- FSM sets `g_nothingToUpload = true` and transitions to `RELEASING`
- `RELEASING` state then skips the reboot and enters `COOLDOWN` instead

### Config Edit Lock (`g_configEditLock`)
```cpp
bool g_configEditLock = false;          // set via POST /api/config-lock
unsigned long g_configEditLockAt = 0;  // millis() when lock acquired
const unsigned long CONFIG_EDIT_LOCK_TIMEOUT_MS = 30 * 60 * 1000;  // 30 min
```
- When `true`, `handleListening()` returns early and does NOT transition to `ACQUIRING` — FSM stays in `LISTENING` until lock is released or expires
- Auto-expires after 30 minutes to prevent accidentally blocking uploads forever
- Cannot be acquired while an upload is in progress (HTTP 409 from `/api/config-lock`)
- Released automatically after a successful Save or Save & Reboot from the web UI

### Upload Modes
- **Smart Mode**: Continuous loop, uploads recent data anytime, old data only in upload window
- **Scheduled Mode**: Only uploads within configured time window, enters IDLE between windows

## Global Objects
- `Config` - Configuration management
- `SDCardManager` - SD card access control
- `WiFiManager` - Network connectivity
- `FileUploader` - Upload orchestration
- `TrafficMonitor` - SD bus activity detection
- `CpapWebServer` - Web interface (optional)
- `OTAManager` - OTA updates (optional)

## Lifecycle
1. **Boot**: Immediate CPU throttle (80 MHz) + BT memory release, detect reset reason, initialize components
2. **Setup**: Load config, apply TX power early, connect WiFi (802.11b disabled), enable DFS, start web server
3. **Loop**: Run FSM with state-appropriate `vTaskDelay()` yields (enables DFS), handle web requests, monitor heap
4. **Upload**: Pre-flight scan → if work found, upload active backend, reboot; if no work, go to cooldown
5. **Recovery**: Soft reboot after every real upload session restores contiguous heap

## Power Management
- **Boot**: CPU at 80 MHz from first instruction, Bluetooth memory released
- **WiFi**: 802.11b disabled (OFDM only), TX power default 8.5 dBm, MIN_MODEM sleep default
- **DFS**: CPU scales 80-160 MHz automatically via `esp_pm_configure()`
- **Loop yields**: State-appropriate `vTaskDelay()` calls (10-100ms) allow DFS to engage
- **Compile-time**: `CONFIG_BT_ENABLED=n`, `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11`, `CONFIG_PM_ENABLE=y`

## Configuration Dependencies
- All timing parameters (inactivity, exclusive access, cooldown)
- Upload mode and window settings
- Backend endpoints (SMB, Cloud, WebDAV)
- Power management settings (CPU speed, TX power, power saving mode)

## Integration Points
- **UploadFSM**: Core state machine logic
- **FileUploader**: Backend upload orchestration
- **TrafficMonitor**: SD activity detection for smart mode
- **WiFiManager**: Network connectivity for cloud operations
- **WebServer**: User interface and manual controls
