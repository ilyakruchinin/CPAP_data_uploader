# Release Notes v0.8.1

## Stability & Reliability

This is a stability-focused release that eliminates watchdog-triggered reboots during SleepHQ cloud uploads and makes the "Reset State" button responsive even mid-upload.

---

## 🐛 Bug Fixes

### Watchdog Timeout During Cloud Uploads (Critical)
- **Root cause:** The upload task runs on Core 0 for TLS/OAuth operations. During CPU-intensive TLS handshakes (5-15 seconds of RSA/EC crypto), Core 0's IDLE task was starved and couldn't feed the hardware watchdog — triggering a system reboot.
- **Fix:** IDLE0 is temporarily unsubscribed from the task watchdog while the upload task owns Core 0, and re-subscribed when it completes. A software watchdog (2-minute heartbeat timeout) in the main loop provides a safety net — if the upload task hangs, it is force-killed and the system recovers gracefully instead of freezing.

### "Reset State" Button Unresponsive During Upload
- **Before:** The reset handler was gated by `!uploadTaskRunning`, so pressing "Reset State" during an upload did nothing until the upload finished (potentially minutes).
- **After:** Reset takes effect **immediately**. The upload task is killed, an NVS flag is set, and the device reboots. State files are deleted on next clean boot with a fresh SD card mount — avoiding SD bus corruption from killing a task mid-I/O.

### TrafficMonitor Polling After Boot
- **Before:** `trafficMonitor.update()` (PCNT hardware counter sampling) was called every loop iteration regardless of FSM state — unnecessary overhead during uploads, cooldown, idle, etc.
- **After:** Only called in `LISTENING` and `MONITORING` states where activity detection is actually needed.

---

## 🔧 Technical Details

### Files Changed
- **`src/main.cpp`** — IDLE0 WDT management, software watchdog, NVS-based deferred state reset, conditional TrafficMonitor polling
- **`src/SleepHQUploader.cpp`** — Heartbeat feeds in `httpRequest()` and streaming upload WiFi reconnection loops
- **`src/TestWebServer.cpp`** — Fixed chunked response initiation in `handleApiConfig()`

### Reset State Flow (New)
1. User presses "Reset State" on web UI
2. NVS `reset_state` flag set to `true`
3. Upload task killed immediately (if running)
4. `esp_restart()` — full hardware reset
5. On boot: NVS flag detected → state files deleted with clean SD mount → flag cleared
6. Device starts fresh with empty upload state

### Upload Task Watchdog Architecture
- **Hardware WDT:** IDLE0 unsubscribed during upload (prevents false trigger during TLS)
- **Software WDT:** `g_uploadHeartbeat` updated after each HTTP operation; main loop force-kills task if stale for 2 minutes
- **Recovery:** Force-killed task triggers IDLE0 re-subscription + FSM transition to RELEASING

---

## 📊 Build Information

- Flash: ~41.7% (1.31 MB)
- RAM: ~16.3%
- Build environments: `pico32` (standard), `pico32-ota` (OTA-enabled)

---

## 🔗 Links

- **Previous release:** [v0.8.0](RELEASE_NOTES_v0.8.0.md)
- **User Guide:** [release/README.md](README.md)
