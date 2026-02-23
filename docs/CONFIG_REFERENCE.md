# Config Reference
...

## SD Card & System Options

*   **`ENABLE_SD_CMD0_RESET`** (default: `true`)
    *   If `true`, bit-bangs the `CMD0` (GO_IDLE_STATE) frame on the SD bus immediately before returning control to the CPAP machine. This forces the CPAP to cleanly remount the SD card instead of throwing a timeout exception. 

*   **`SAVE_LOGS`** (default: `false`)
    *   If `true`, writes persistent logs to the ESP32's internal `LittleFS` partition (as rotating `syslog.A.txt` / `syslog.B.txt` files) instead of holding them in RAM. **Note:** Logs are NO LONGER written to the physical SD card to prevent filesystem corruption.
