# CPAP AutoSync v2.0i-alpha1

## Critical Upgrade Notice: USB Flash Required

**This is a major platform upgrade. You MUST perform a full flash via USB.**

OTA updates from v1.x are **not supported** for this release. The underlying framework, partition table, and SDK configuration have all changed. A full USB flash is required.

**Instructions:**
1. Connect the ESP32 to your computer via USB.
2. Use the Web Flasher or PlatformIO to upload the firmware.
3. **Important:** If you previously had `STORE_CREDENTIALS_PLAIN_TEXT = false` (the old default), your `config.txt` likely contains `***STORED_IN_FLASH***` as your WiFi password. After flashing, you **must** re-enter your actual passwords in `config.txt` — NVS is erased during a full flash. The firmware will log a clear error if it detects this situation.

---

## Platform Upgrade: pioarduino (Arduino 3.3.x / ESP-IDF 5.5.2)

The entire build toolchain has been migrated from stock PlatformIO Arduino-ESP32 (Arduino 2.0.17 / ESP-IDF 4.4.x, precompiled) to **pioarduino** (Arduino 3.3.x / ESP-IDF 5.5.2, source-compiled hybrid).

### Why

The previous platform used **precompiled** framework libraries. SDK configuration options in `sdkconfig.defaults` (power management, Bluetooth removal, tick rate, lwIP tuning, etc.) were silently ignored — the precompiled `.a` files could not be customised. This meant:

- `CONFIG_PM_ENABLE=y` did nothing → no Dynamic Frequency Scaling, no auto light-sleep
- `CONFIG_BT_ENABLED=n` did nothing → ~30 KB DRAM wasted on Bluetooth that was never used
- `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` did nothing → PHY TX cap couldn't be enforced at framework level
- mbedTLS cipher removal did nothing → ChaCha20 (software) remained available, missing the hardware AES accelerator path
- All lwIP buffer tuning was ignored

### What Changed

| Property | v1.x | v2.0i |
|---|---|---|
| PlatformIO platform | `espressif32` (official) | `pioarduino` (source-compiled hybrid) |
| Arduino core | 2.0.17 (precompiled) | 3.3.7 (source-compiled) |
| ESP-IDF | 4.4.x | 5.5.2 |
| sdkconfig | Ignored (precompiled) | **Respected** (source compilation) |
| mbedTLS | Precompiled; required `rebuild_mbedtls.py` hack | Source-compiled; native config |
| PCNT driver | Legacy (`driver/pcnt.h`) | New IDF 5.x (`driver/pulse_cnt.h`) |
| PM struct | `esp_pm_config_esp32_t` | `esp_pm_config_t` |

### User-Facing Benefits

- **Lower idle power consumption** — Dynamic Frequency Scaling (DFS) now works: CPU scales from 80 MHz down to 40 MHz (XTAL) in idle/cooldown states, saving ~5–10 mA
- **~30 KB more free DRAM** — Bluetooth is removed at compile time, not just at runtime
- **Faster TLS handshakes** — Forces hardware-accelerated AES-128-GCM by removing software ChaCha20/Poly1305 ciphers
- **More reliable operation** — lwIP buffer tuning, brownout detector configuration, and WiFi TX power caps are now actually enforced by the framework

---

## Breaking Changes

### `STORE_CREDENTIALS_PLAIN_TEXT` → `MASK_CREDENTIALS` (inverted semantics)

The credential storage config key has been **renamed and inverted**:

| Old (v1.x) | New (v2.0i) | Meaning |
|---|---|---|
| `STORE_CREDENTIALS_PLAIN_TEXT = true` | `MASK_CREDENTIALS = false` (or omit) | Passwords stay in `config.txt` as plaintext |
| `STORE_CREDENTIALS_PLAIN_TEXT = false` (old default) | `MASK_CREDENTIALS = true` | Passwords migrated to NVS, censored in `config.txt` |

**The new default is plaintext** (`MASK_CREDENTIALS = false`). This is safer for development — passwords survive full firmware flashes without loss. The old key `STORE_CREDENTIALS_PLAIN_TEXT` is no longer recognised and will generate an "unknown config key" warning.

**Action required:** If your `config.txt` contains `***STORED_IN_FLASH***`, replace those entries with your actual passwords before flashing v2.0i.

---

## SMB Upload Reliability

### Async SMB Event Loop

All SMB operations (connect, open, write, close, mkdir, stat, opendir) have been rewritten from synchronous `libsmb2` calls to their **async equivalents** with a shared event loop.

**Why:** The previous synchronous `smb2_write()` could block the upload task for the entire duration of a network timeout (15–30s), starving the watchdog and freezing the web server (shared lwIP stack).

**Benefits:**
- **No more watchdog resets during SMB uploads** — the event loop feeds the watchdog every 1-second poll iteration
- **Web UI stays responsive** during uploads — `poll()` yields CPU to lwIP between iterations
- **Cancel from Web UI** actually works — abort flag checked every iteration
- **Heap-neutral** — async API uses identical data structures to sync; no additional memory

### TLS Socket Timeout Reduced

TLS socket timeout reduced from **60s → 20s**, safely below the 30s task watchdog. Previously, a single blocking `tlsClient->connect()` or `tlsClient->write()` could exceed the 30s watchdog window.

### Watchdog Feeding in Reconnect Backoff

All `delay()` calls in SMB reconnect/retry paths now feed the watchdog first. Previously, cumulative backoff delays (150ms + 200ms × attempt) could push the total elapsed time past the watchdog threshold.

---

## Heap Fragmentation Management

### TLS Pre-Warm Before SD Mount

TLS handshake (~36 KB contiguous) now happens **before** `SD_MMC.begin()`, while heap is cleanest (~98 KB max_alloc). Previously, TLS connected on-demand after SD mount and pre-flight scanning had already fragmented heap to ~38–55 KB, causing intermittent handshake failures.

After pre-warm, a **PCNT silence re-check** confirms the CPAP hasn't started using the SD card during the 2–4s handshake. If silence is broken, the cycle aborts cleanly without ever touching the SD bus.

**Benefits:**
- Eliminates TLS handshake failures at low `max_alloc` (~38 KB)
- Reduces SD card hold time (TLS delay moved before card grab)
- Adds a safety net against CPAP interference during the handshake window

### Heap Safety Valve

In `MINIMIZE_REBOOTS` mode, if `max_alloc` drops below **32 KB**, the device now forces a reboot to restore heap. Previously it only logged a warning at 35 KB, allowing the device to drift into an unrecoverable fragmented state.

### Operational Floor Documented

The `max_alloc` steady-state floor of **~38,900 bytes** during active SMB transfers is now documented as expected behaviour. All allocation guards use thresholds below this (36 KB for TLS). The floor recovers after transfers complete.

---

## Log Display Fix

Fixed out-of-order timestamps in the Web UI Logs tab where lines from a previous boot appeared interleaved with current-boot lines. The client now clears its buffer and re-fetches from the server on reboot detection, rather than attempting client-side cross-boot log stitching.

---

## Known Limitations

- **USB flash required** — OTA upgrade from v1.x is not supported
- **mbedTLS asymmetric buffers not available** — the pioarduino hybrid compile cannot safely change mbedTLS struct sizes without ABI mismatch against the precompiled WiFiClientSecure layer
- **`CONFIG_FREERTOS_HZ=100` not achievable** — Arduino's `delay()`/`millis()` depend on 1000 Hz tick rate, enforced by the framework's CMakeLists.txt

---

## Technical Notes

- PCNT driver fully migrated to IDF 5.x new driver API (`driver/pulse_cnt.h`)
- `esp_pm_config_esp32_t` → `esp_pm_config_t`
- Brownout register access verified compatible with IDF 5.x
- `esp_bt_controller_mem_release()` guarded with `#if CONFIG_BT_ENABLED`
- WiFi sleep IRAM optimisations reverted to recover ~2–4 KB DRAM (caused heap fragmentation under TLS reconnect)
- `LWIP_PBUF_POOL_SIZE` reduced to free more contiguous heap
- `rebuild_mbedtls.py` build hack removed — no longer needed with source compilation
