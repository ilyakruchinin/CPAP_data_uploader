# CPAP Data Uploader v0.10.1 Release Notes

## Stability Optimizations

### 📉 SD Card Access Minimization
**Feature:** The system now minimizes the time it spends holding the SD card hostage, both during Web UI operations and normal uploads.
- **Mount Duration Logging:** Every time the ESP32 releases the SD card, the total mount duration (in milliseconds) is now recorded in the system logs, allowing for precise tracking of SD card hold times.
- **Config Editor:** The Web UI no longer aggressively pre-fetches the `config.txt` file when loading the dashboard or pressing F5. The SD card is now only accessed when you explicitly click the **Edit** button.
- **Save & Reboot:** The obsolete `config-lock` network endpoints and backend locking states have been removed. Because the ESP32 always mounts the SD card natively as Read/Write (only enforcing Read-Only behavior at the application level), clicking **Save & Reboot** now instantly writes the file and reboots the ESP32 without any unmounting, remounting, or wait delays.
- **Network Pre-Connect:** During a normal therapy upload, connecting to SMB shares and performing the TLS handshake with SleepHQ can take 2-5 seconds. Previously, the ESP32 held the SD card captive during this entire network setup phase. Now, the system pre-connects to all active backends *before* acquiring the SD card, shaving several seconds off the total SD bus hold time per session.

### 🔄 SD Protocol Reset Disabled by Default
**Feature:** The `ENABLE_SD_CMD0_RESET` configuration option now defaults to `false`. 
**Reason:** While manually bit-banging the `CMD0` (GO_IDLE_STATE) frame helps some CPAP models cleanly re-initialize the SD card when the ESP32 releases control, it has been found to cause compatibility issues or unnecessary delays on others. If you experience intermittent CPAP timeouts when the ESP32 finishes an upload, you can still manually enable this behavior by adding `ENABLE_SD_CMD0_RESET = true` to your `config.txt`.
