# CPAP AutoSync v0.11.1-i Release Notes

## ⚡ Power Optimization & Stability — Deep Sleep, Cipher Hardening, Critical Bug Fixes

This release builds on v0.11.0's power foundation with aggressive idle-state power reduction, TLS cipher optimization, and three critical stability fixes identified from post-release field reports.

### Summary of Changes

| Metric | v0.11.0 | v0.11.1-i |
|:-------|:--------|:----------|
| **Idle current (WiFi connected)** | ~20–25 mA | **~2–3 mA** (auto light-sleep) |
| **Peak TX current** | ~120–150 mA @ 8.5 dBm | **~100–120 mA @ 5 dBm** |
| **DFS transitions during idle** | Frequent (80↔160 MHz) | **Zero** (CPU locked at 80 MHz) |
| **FreeRTOS tick overhead** | 1000 ISRs/sec/core | **100 ISRs/sec/core** |
| **TLS cipher suite** | AES-128/256 + ChaCha20 | **AES-128-GCM only** (HW accelerated) |

---

### 🔴 Critical: WiFi Race Condition Fixed

The main loop (Core 1) no longer calls `connectStation()` while the upload task (Core 0) is running. Previously, both cores could fight over WiFi state, causing connection corruption and multi-hour offline events. The upload task manages its own WiFi recovery via `tryCoordinatedWifiCycle()`.

### 🔴 Critical: DFS Hardcoding Fixed

`esp_pm_configure()` now uses the configured `CPU_SPEED_MHZ` for the DFS ceiling instead of a hardcoded 160 MHz. With the default `CPU_SPEED_MHZ = 80`, DFS is effectively disabled — zero frequency transitions, zero PLL relock transients. Users on non-constrained hardware can set `CPU_SPEED_MHZ = 160` to re-enable DFS.

### 🔴 Critical: GMT_OFFSET Config Key Accepted

`GMT_OFFSET` is now accepted as a backward-compatible alias for `GMT_OFFSET_HOURS`. Previously, users migrating from v0.5.x JSON configs had their timezone silently ignored, causing the upload window to operate in UTC and potentially grabbing the SD card during therapy.

### 🟠 Default TX Power Reduced to 5 dBm

Default WiFi transmit power lowered from 8.5 dBm (`MID`) to 5.0 dBm (`LOW`). This further reduces peak current during WiFi transmission. Sufficient for typical bedroom placement (~5 m). Increase to `MID` or `HIGH` in `config.txt` if your router is further away.

### 🟠 Auto Light-Sleep Enabled

The ESP32 CPU now enters light-sleep between WiFi DTIM intervals when the FSM is in IDLE or COOLDOWN states. GPIO 33 (CS_SENSE) is configured as a wakeup source to detect CPAP SD bus activity. A PM lock prevents sleep during active states (LISTENING, UPLOADING, etc.). This reduces idle current from ~20 mA to ~2–3 mA.

### 🟠 FreeRTOS Tick Rate Reduced to 100 Hz

`CONFIG_FREERTOS_HZ` reduced from 1000 to 100. Combined with tickless idle (`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`), this eliminates unnecessary timer ISRs during sleep and reduces DFS evaluation points by 90%. All firmware operations (TrafficMonitor, upload retries, web server) work correctly at 100 Hz.

### 🟡 ChaCha20 Ciphers Disabled

`CONFIG_MBEDTLS_CHACHA20_C` and `CONFIG_MBEDTLS_CHACHAPOLY_C` set to `n` in `sdkconfig.defaults`. This forces TLS to use hardware-accelerated AES ciphers only, eliminating software-only ChaCha20 computation that pins the CPU during uploads.

### 🟡 AES-256 Disabled (AES-128 Only)

`CONFIG_MBEDTLS_AES_256_C` set to `n`. All TLS connections now use AES-128-GCM exclusively, which is fully hardware-accelerated on ESP32. AES-128 is universally supported by SleepHQ and all major servers. To re-enable AES-256, change `CONFIG_MBEDTLS_AES_256_C=n` to `=y` in `sdkconfig.defaults` and rebuild.

### 🟡 Bluetooth Memory Release Diagnostics

Heap snapshots are now captured before and after `esp_bt_controller_mem_release()` and logged after Serial initialization. This confirms whether the runtime BT memory release succeeds (saving ~30 KB) even though BT is disabled at compile time.

---

## ⚠️ Breaking Changes

### New Power Defaults

| Setting | v0.11.0 Default | v0.11.1-i Default |
|:--------|:---------------|:------------------|
| `WIFI_TX_PWR` | MID (8.5 dBm) | **LOW (5.0 dBm)** |

### If WiFi Connectivity Degrades After Upgrade

Add the following to `config.txt`:
```ini
WIFI_TX_PWR = MID
```
Or for maximum range:
```ini
WIFI_TX_PWR = HIGH
```

---

## Files Changed

| File | Changes |
|:-----|:--------|
| `src/main.cpp` | WiFi race guard, DFS fix, auto light-sleep, GPIO wakeup, PM lock, BT diagnostics |
| `src/Config.cpp` | `GMT_OFFSET` alias, default TX power to `LOW` |
| `sdkconfig.defaults` | FreeRTOS 100 Hz, tickless idle, ChaCha20 off, AES-256 off |
