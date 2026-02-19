# Web Interface (Progressive Web App)

## Overview
The Web Interface (`TestWebServer.cpp/.h` + `web_ui.h`) provides a Progressive Web App (PWA) for monitoring and controlling the CPAP uploader. Features pre-allocated buffers to prevent heap fragmentation and auto-refreshing real-time updates.

## Architecture

### PWA Design Principles
- **Pre-allocated buffers**: All HTML/JS content generated at startup
- **No dynamic allocation**: Prevents heap fragmentation during uploads
- **Auto-refreshing**: Real-time status updates without user interaction
- **Responsive design**: Works on mobile and desktop browsers

### Server Implementation
- **AsyncWebServer**: Non-blocking request handling
- **Rate limiting**: Prevents server overload during uploads
- **CORS support**: Enables cross-origin API access
- **mDNS integration**: Accessible via `http://cpap.local`

## Core Features

### Real-time Dashboard
```cpp
void updateStatusSnapshot() {
    // Pre-allocated JSON buffer
    char buf[1024];
    int n = snprintf(buf, sizeof(buf),
        "{\"state\":\"%s\",\"uptime\":%lu,\"time\":\"%s\","
        "\"free_heap\":%u,\"max_alloc\":%u,"
        "\"smb_comp\":%d,\"smb_inc\":%d,"
        "\"cloud_comp\":%d,\"cloud_inc\":%d}",
        stateString, uptime, timeStr,
        ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
        smbCompleted, smbIncomplete,
        cloudCompleted, cloudIncomplete);
}
```

### Manual Controls
- **Trigger Upload**: Force immediate upload bypassing schedule
- **Reset State**: Clear upload history for fresh start
- **Soft Reboot**: Restart with fast-boot (skip delays)
- **View Logs**: Real-time log streaming

### SD Activity Monitor
```cpp
void handleMonitor() {
    // Real-time graph of SD bus activity
    String json = trafficMonitor.getActivityStatus();
    server->send(200, "application/json", json);
}
```

## Interface Components

### Main Dashboard
- **System status**: Current FSM state, uptime, time sync
- **Heap monitoring**: Free heap and max contiguous heap
- **Upload progress**: Separate SMB and Cloud progress bars
- **Network status**: WiFi connectivity and signal strength
- **Next upload**: Time until next scheduled upload

### Upload Controls
- **Manual trigger**: Bypass schedule for immediate upload
- **State reset**: Clear all upload tracking
- **Soft reboot**: Restart with delay skipping
- **Configuration view**: Current settings display

### Monitoring Tools
- **SD activity graph**: Real-time bus activity visualization
- **Log viewer**: Client-side persistent log buffer with reboot detection, deduplication, and copy-to-clipboard
- **Status API**: JSON endpoint for external monitoring
- **OTA updates**: Firmware update interface

## Advanced Features

### Rate Limiting
```cpp
bool isUploadUiRateLimited(UploadUiSlot slot, bool uploadInProgress, unsigned long minIntervalMs) {
    // Prevent server overload during uploads
    static unsigned long lastServedMs[kUploadUiSlotCount] = {0};
    
    if (uploadInProgress && (now - lastServedMs[slot]) < minIntervalMs) {
        return true; // Rate limited
    }
    return false;
}
```

### Pre-allocated Content
```cpp
// All HTML/JS generated at startup to prevent runtime allocation
const char* getMainPageHtml() {
    return R"(
<!DOCTYPE html>
<html>
<head>
    <title>CPAP Data Uploader</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <script>
        // Auto-refresh every 3 seconds
        setInterval(() => location.reload(), 3000);
    </script>
</head>
<body>
    <!-- Pre-built UI content -->
</body>
</html>
)";
}
```

### Real-time Features
- **Auto-refresh**: 3-second page reload for dashboard
- **Live status**: JSON API for custom monitoring
- **Progress tracking**: Real-time upload progress bars
- **Manual controls**: Upload triggers, state reset, soft reboot indication

### Client-side Log Buffering
The Logs tab maintains a persistent in-browser ring buffer (up to 2000 lines) that survives page soft-reloads:
- **Deduplication**: Each poll response is diffed against `lastSeenLine` (last non-empty line already in buffer). Only lines after `lastSeenLine` are appended — no duplicates across polls.
- **Reboot detection**: The boot banner (`=== CPAP Data Auto-Uploader ===`) is always present in server responses (ring buffer starts from boot). A genuinely new reboot is detected only when `lastSeenLine` is absent from the response **or** appears before the boot banner. In that case a `─── DEVICE REBOOTED ───` separator is inserted. Same-boot polls are treated as normal tails.
- **Copy to clipboard**: A "Copy to clipboard" button exports the entire buffer as plain text.
- **Clear buffer**: Resets the buffer and `lastSeenLine` state.

## API Endpoints

### Status Endpoints
- `GET /` - Main dashboard (auto-refreshing)
- `GET /status` - JSON status for external monitoring
- `GET /config` - Current configuration display
- `GET /logs` - Recent system logs

### Control Endpoints
- `GET /trigger-upload` - Force immediate upload
- `GET /reset-state` - Clear upload history
- `GET /soft-reboot` - Restart with fast-boot
- `POST /ota` - Firmware update (OTA builds only)
- `GET /api/config-raw` - Read raw `config.txt` from SD card
- `POST /api/config-raw` - Write new `config.txt` content (max 4096 bytes, atomic rename)
- `POST /api/config-lock` - Acquire or release the config edit lock (body: `{"lock":true/false}`)

### Config Edit Lock (`/api/config-lock`)
Prevents the FSM from starting new upload sessions while the user edits `config.txt` in the web UI. Automatically expires after **30 minutes** if not released. Rejected (HTTP 409) if an upload is currently in progress.

Flow:
1. User clicks **Edit** → `POST /api/config-lock {"lock":true}` → FSM paused, textarea unlocked
2. User edits, then clicks **Save** or **Save & Reboot** → config written atomically → `POST /api/config-lock {"lock":false}` → FSM resumed
3. **Cancel** → `POST /api/config-lock {"lock":false}` → FSM resumed, no write

### Monitoring Endpoints
- `GET /monitor` - SD activity status JSON
- `GET /api/status` - Detailed system status
- `GET /ota` - OTA update interface

## Performance Optimizations

### Memory Management
- **Static buffers**: All response content pre-allocated
- **No string concatenation**: Avoids heap fragmentation
- **Stack allocation**: Temporary buffers on stack
- **Rate limiting**: Prevents memory pressure during uploads

### Network Efficiency
- **Minimal responses**: Compact JSON formats
- **Caching headers**: Browser caching for static content
- **Compression**: Disabled for memory conservation
- **Connection reuse**: Keep-alive where possible

## Configuration Integration

### System Status
- **FSM state**: Current upload state and timing
- **Heap metrics**: Free memory and fragmentation
- **Network status**: WiFi connectivity and signal
- **Upload progress**: Per-backend completion status

### Display Settings
- **Hostname**: mDNS name for access
- **Time display**: Local time with timezone
- **Progress bars**: Visual upload indication
- **Error messages**: User-friendly error display

## Security Considerations

### Access Control
- **No authentication**: Intended for trusted networks only
- **Local network**: mDNS limits exposure to local network
- **CORS restrictions**: Limited to necessary origins
- **No sensitive data**: Passwords censored in displays

### Safe Operations
- **State validation**: Checks FSM state before operations
- **Confirmation dialogs**: Prevent accidental destructive actions
- **Rate limiting**: Prevents denial of service
- **Input validation**: Sanitizes all user inputs

## Integration Points

### Main Application
- **Global flags**: Trigger upload, reset state, soft reboot
- **Status monitoring**: Real-time system status
- **Control interface**: Manual override capabilities

### Upload System
- **Progress tracking**: Real-time upload status
- **State management**: FSM state visibility
- **Error reporting**: Upload error display

### Traffic Monitor
- **Activity graph**: Real-time SD bus activity
- **Statistics**: Idle/active time percentages
- **Debug data**: Raw pulse counts and timing

## Browser Compatibility

### Supported Features
- **Auto-refresh**: Meta refresh tags
- **Responsive design**: Mobile-friendly layout
- **JSON parsing**: Modern JavaScript
- **AJAX requests**: Fetch API or XMLHttpRequest

### Progressive Enhancement
- **Basic functionality**: Works without JavaScript
- **Enhanced features**: Auto-refresh, real-time updates
- **Graceful degradation**: Fallback for older browsers
- **Mobile optimization**: Touch-friendly interface

## Debugging Features

### Development Tools
- **Verbose logging**: Detailed request logging
- **Error pages**: User-friendly error messages
- **Debug endpoints**: Additional diagnostic information
- **Performance metrics**: Request timing data

### Troubleshooting
- **Status indicators**: Clear system state display
- **Error logs**: Detailed error information
- **Network diagnostics**: WiFi and connectivity status
- **Memory monitoring**: Heap usage display
