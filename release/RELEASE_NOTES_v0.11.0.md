# CPAP Data Uploader v0.11.0 Release Notes

## ⚡ Power Optimization — AirSense 11 Compatibility

This release dramatically reduces ESP32 power consumption to improve compatibility with Singapore-made AirSense 11 machines that have constrained SD card slot power delivery.

### Summary of Changes

| Metric | Before (v0.10.x) | After (v0.11.0) |
|:-------|:------------------|:-----------------|
| **Peak TX current** | 370 mA (802.11b possible) | ~120-150 mA (802.11g/n @ 8.5 dBm) |
| **Idle WiFi current** | ~120-130 mA | ~20-25 mA |
| **Boot current (pre-WiFi)** | ~50-68 mA (240 MHz) | ~20-25 mA (80 MHz) |

---

### 🔴 802.11b Protocol Disabled
802.11b (DSSS modulation) is now disabled at connection time. Only 802.11g/n (OFDM) is permitted. This eliminates the 370 mA peak TX current scenario entirely. Virtually all modern routers support 802.11g/n.

### 🔴 Default TX Power Reduced to 8.5 dBm
Default WiFi transmit power changed from 19.5 dBm (maximum) to 8.5 dBm. Sufficient for typical bedroom placement (5-15 m from router). A new `MAX` option (19.5 dBm) is available for users who need it. Compile-time cap at 11 dBm via `sdkconfig.defaults`.

### 🔴 CPU Throttled to 80 MHz at Boot
CPU frequency is now reduced to 80 MHz as the very first instruction in `setup()`, before any delays or initialization. This saves ~30-40 mA during the entire 20+ second boot sequence.

### 🔴 Bluetooth Fully Disabled
Bluetooth is disabled at compile time (`CONFIG_BT_ENABLED=n`) and controller memory is released at runtime. Saves ~30 KB DRAM and eliminates BT-related leakage current.

### 🟠 Default WiFi Power Saving Changed to MIN_MODEM
Default WiFi power saving mode changed from `NONE` (radio always on) to `MID` (WIFI_PS_MIN_MODEM). Wakes every DTIM to preserve mDNS and broadcast reception. Reduces idle WiFi current from ~120 mA to ~22-31 mA.

### 🟠 Cloud Upload Power Leak Fixed
Removed `WiFi.setSleep(false)` in `SleepHQUploader.cpp` that permanently disabled WiFi power saving after any Cloud upload without restoring it. The ESP-IDF WiFi driver already holds PM locks during active TX/RX operations.

### 🟠 TX Power Applied Before WiFi Association
New `applyTxPowerEarly()` method pre-initializes WiFi STA mode and sets TX power before `WiFi.begin()`, preventing full-power spikes during the initial scan and association phase.

### 🟡 Dynamic Frequency Scaling (DFS)
Enabled ESP-IDF power management framework (`CONFIG_PM_ENABLE=y`). CPU automatically scales between 80 MHz (idle) and 160 MHz (WiFi/TLS activity). WiFi driver holds PM locks during active operations.

### 🟡 Main Loop Yields for DFS
Added state-appropriate `vTaskDelay()` calls at the end of `loop()` so the FreeRTOS IDLE task can run and DFS can engage:
- IDLE / COOLDOWN: 100 ms yield
- LISTENING: 50 ms yield
- MONITORING / UPLOADING: 10 ms yield

---

## ⚠️ Breaking Changes

### New Power Defaults
Users relying on the previous high-power defaults will now run at lower power by default. **Existing `config.txt` files with explicit power settings are unaffected.** Only users relying on defaults will see different behavior — which is the intended improvement.

| Setting | Old Default | New Default |
|:--------|:-----------|:------------|
| `CPU_SPEED_MHZ` | 240 | **80** |
| `WIFI_TX_PWR` | HIGH (19.5 dBm) | **MID (8.5 dBm)** |
| `WIFI_PWR_SAVING` | NONE | **MID (MIN_MODEM)** |

### New TX Power Level: MAX
A new `MAX` TX power level (19.5 dBm) has been added for users who need the previous maximum range. Use `WIFI_TX_PWR = MAX` in `config.txt`.

### If WiFi Connectivity Degrades After Upgrade
Add the following to `config.txt`:
```ini
WIFI_TX_PWR = HIGH
```
Or for maximum range:
```ini
WIFI_TX_PWR = MAX
```
