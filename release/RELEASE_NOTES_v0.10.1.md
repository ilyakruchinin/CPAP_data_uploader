# CPAP Data Uploader v0.10.1 Release Notes

## Stability Optimizations

### 🔄 SD Protocol Reset Disabled by Default
**Feature:** The `ENABLE_SD_CMD0_RESET` configuration option now defaults to `false`. 
**Reason:** While manually bit-banging the `CMD0` (GO_IDLE_STATE) frame helps some CPAP models cleanly re-initialize the SD card when the ESP32 releases control, it has been found to cause compatibility issues or unnecessary delays on others. If you experience intermittent CPAP timeouts when the ESP32 finishes an upload, you can still manually enable this behavior by adding `ENABLE_SD_CMD0_RESET = true` to your `config.txt`.
