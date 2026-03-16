# Replatform Plan: Migration to pioarduino

## Document Purpose

This is a concrete implementation plan for migrating the CPAP Data Uploader firmware from the stock PlatformIO Arduino-ESP32 framework (Arduino 2.0.17 / ESP-IDF 4.4.x, precompiled) to **pioarduino** (Arduino 3.3.x / ESP-IDF 5.5.x, source-compiled).

It synthesises the findings from:

- `32-ANALYSIS-CO46.md` — full codebase audit, framework ceiling identification
- `33-ANALYSIS-G31.md` — "total current budget" theory, migration motivation
- `34-ANALYSIS-C54.md` — grounded correction: PM benefit is mainly light-sleep, not DFS; migration is an enabler, not a guaranteed fix

### What this plan is

- a step-by-step guide to every file and API change required
- an honest assessment of risk per change area
- a validation checklist

### What this plan is NOT

- a guarantee that migration will solve brownout issues
- a recommendation to rush — this should be done on a dedicated branch with careful regression testing

---

## Target Platform

| Property | Current | Target |
|---|---|---|
| PlatformIO platform | `espressif32` (official) | `pioarduino/platform-espressif32` (stable) |
| Arduino core | 2.0.17 (precompiled) | 3.3.7 (source-compiled) |
| ESP-IDF | 4.4.x | 5.5.2 |
| sdkconfig handling | mostly ignored (precompiled `.a`) | **respected** (source compilation) |
| mbedTLS | precompiled; requires `rebuild_mbedtls.py` hack | source-compiled; native config |

### pioarduino platform reference

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
```

---

## What the Migration Unlocks

Directly from C54 analysis, grounded in codebase evidence:

1. **`CONFIG_PM_ENABLE=y` becomes functional** — auto light-sleep in IDLE/COOLDOWN states (PM lock already implemented in `transitionTo()`)
2. **`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` becomes functional** — suppresses tick ISRs during sleep
3. **`CONFIG_BT_ENABLED=n` becomes functional** — compile-time BT removal (~30 KB DRAM savings)
4. **`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10` becomes functional** — caps PHY TX during framework-controlled phases
5. **`CONFIG_FREERTOS_HZ=100` becomes functional** — reduces ISR overhead
6. **Native mbedTLS configuration** — eliminates `scripts/rebuild_mbedtls.py`
7. **All `sdkconfig.defaults` entries take effect** — lwIP tuning, TLS buffer sizes, cipher disablement

### What it does NOT automatically unlock (per C54 correction)

- Large CPU DFS savings — current `min_freq_mhz = 80` means no frequency scaling below 80 MHz
- Guaranteed brownout elimination — hardware margin remains the dominant variable
- Zero-effort compatibility — API changes require code modifications

---

## Risk Assessment Summary

| Area | Risk | Reason |
|---|---|---|
| `platformio.ini` | **Low** | Config change only |
| `sdkconfig.defaults` | **Low** | Already written; just becomes effective |
| `rebuild_mbedtls.py` | **Low** | Removal only |
| PCNT driver migration | **Medium** | Full API rewrite from legacy to new driver |
| PM struct rename | **Low** | Single type rename |
| Brownout register access | **Low-Medium** | Header still exists for ESP32; verify |
| WiFi API | **Low** | Minor deprecation (`available()` → `accept()`) |
| SD_MMC API | **Low** | Unchanged in Arduino 3.x for ESP32 |
| WebServer API | **Low** | No breaking changes for basic usage |
| WiFiClientSecure / HTTPClient | **Low** | `flush()` semantics changed but not used destructively |
| `esp_bt_controller_mem_release` | **Low-Medium** | May not be needed if BT compiled out; verify |
| RTC GPIO APIs | **Low** | `rtc_gpio_hold_dis` / `rtc_gpio_deinit` still available |
| libsmb2 (external component) | **Low** | Pure C library, no framework dependency |
| LittleFS | **Low** | Supported in Arduino 3.x |
| FreeRTOS APIs | **Low** | `xTaskCreate`, `vTaskDelay`, `pdMS_TO_TICKS` unchanged |
| `esp_freertos_hooks` | **Low-Medium** | Idle hook registration may have moved |
| OTA (Update library) | **Low** | Arduino OTA API unchanged |
| Full regression (upload FSM, dual backend, web UI) | **Medium-High** | Functional, not API — requires real-device testing |

---

## Phase 0: Branch Setup

**Create a dedicated migration branch.** Do not do this on `main`.

```
git checkout -b feature/pioarduino-migration
```

All changes below happen on this branch. The existing `main` branch remains untouched and releasable.

---

## Phase 1: Build System Changes

### 1.1 `platformio.ini`

Replace the platform line and remove the mbedTLS rebuild script.

**Current:**

```ini
[env:pico32-ota]
platform = espressif32
board = pico32
framework = arduino
...
extra_scripts = 
    pre:scripts/generate_version_prebuild.py
    pre:scripts/rebuild_mbedtls.py
```

**Target:**

```ini
[env:pico32-ota]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
board = pico32
framework = arduino
...
extra_scripts = 
    pre:scripts/generate_version_prebuild.py
```

**Notes:**

- Remove `pre:scripts/rebuild_mbedtls.py` from `extra_scripts`
- The `board_build.sdkconfig = sdkconfig.defaults` line stays — it now actually works
- Consider pinning to a specific pioarduino release tag (e.g., `55.03.37`) instead of `stable` for reproducible builds

### 1.2 `sdkconfig.defaults`

**No changes required.** The file is already well-written and commented. Every entry was designed with the expectation that a source-compiled framework would eventually use it.

Key entries that become effective:

- `CONFIG_PM_ENABLE=y`
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`
- `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3`
- `CONFIG_BT_ENABLED=n`
- `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10`
- `CONFIG_FREERTOS_HZ=100`
- `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` (and all TLS buffer settings)
- `CONFIG_MBEDTLS_CHACHA20_C=n` / `CONFIG_MBEDTLS_AES_256_C=n`

**One addition to consider:**

```ini
# Suppress legacy PCNT driver deprecation warning during migration
# Remove this line once TrafficMonitor is migrated to new PCNT API
CONFIG_PCNT_SUPPRESS_DEPRECATE_WARN=y
```

This allows a **two-phase approach**: get the build working with the legacy PCNT driver first, then migrate PCNT in a separate step.

### 1.3 `scripts/rebuild_mbedtls.py`

**Action:** Remove from `extra_scripts` in Phase 1.1. Do NOT delete the file yet — keep it in the repo until the migration is validated, then remove in a cleanup commit.

**Why it becomes unnecessary:** pioarduino compiles mbedTLS from source using the project's `sdkconfig.defaults`. All the custom defines the script was injecting (`MBEDTLS_SSL_IN_CONTENT_LEN`, `MBEDTLS_SSL_OUT_CONTENT_LEN`, `MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH`, etc.) are already in `sdkconfig.defaults`.

---

## Phase 2: Mandatory Code Changes

These are changes that **must** be made for the build to succeed or to avoid runtime errors.

### 2.1 PCNT Driver Migration (`TrafficMonitor.cpp` + `TrafficMonitor.h`)

**This is the largest single code change in the migration.**

The legacy PCNT driver (`driver/pcnt.h`) is deprecated in IDF 5.x. It still compiles with a warning (suppressible via `CONFIG_PCNT_SUPPRESS_DEPRECATE_WARN=y`), but the recommended path is migrating to the new driver (`driver/pulse_cnt.h`).

**Strategy:** Migrate in Phase 2 if comfortable, or defer to Phase 4 by using the suppression Kconfig. The legacy driver still works in IDF 5.5.

#### API mapping (legacy → new)

| Legacy | New |
|---|---|
| `#include "driver/pcnt.h"` | `#include "driver/pulse_cnt.h"` |
| `pcnt_config_t` + `pcnt_unit_config()` | `pcnt_new_unit()` + `pcnt_new_channel()` |
| `PCNT_UNIT_0` / `PCNT_CHANNEL_0` | `pcnt_unit_handle_t` / `pcnt_channel_handle_t` (opaque) |
| `pcnt_set_filter_value()` + `pcnt_filter_enable()` | `pcnt_unit_set_glitch_filter()` with `pcnt_glitch_filter_config_t` |
| `pcnt_counter_pause()` | `pcnt_unit_stop()` |
| `pcnt_counter_resume()` | `pcnt_unit_start()` |
| `pcnt_counter_clear()` | `pcnt_unit_clear_count()` |
| `pcnt_get_counter_value(unit, &count)` | `pcnt_unit_get_count(unit_handle, &count)` |
| `PCNT_COUNT_INC` (pos_mode/neg_mode) | `pcnt_channel_set_edge_action(PCNT_CHANNEL_EDGE_ACTION_INCREASE, ...)` |
| `PCNT_MODE_KEEP` (ctrl modes) | `pcnt_channel_set_level_action(PCNT_CHANNEL_LEVEL_ACTION_KEEP, ...)` |
| `counter_h_lim` / `counter_l_lim` | Set via `pcnt_unit_config_t` `.low_limit` / `.high_limit` |

#### Files affected

- `src/TrafficMonitor.cpp` — all PCNT calls
- `include/TrafficMonitor.h` — add private members for unit/channel handles

#### Scope

The TrafficMonitor usage is straightforward:

- configure one unit, one channel
- count rising + falling edges on one GPIO
- set glitch filter
- periodically read and clear the counter

This maps cleanly to the new API. No interrupts, no watch points, no multi-channel complexity.

### 2.2 PM Config Struct Rename (`main.cpp`)

In IDF 5.x, `esp_pm_config_esp32_t` is deprecated in favour of the generic `esp_pm_config_t`.

**Current:**

```cpp
esp_pm_config_esp32_t pm_config = {
    .max_freq_mhz = targetCpuMhz,
    .min_freq_mhz = 80,
    .light_sleep_enable = true
};
```

**Target:**

```cpp
esp_pm_config_t pm_config = {
    .max_freq_mhz = targetCpuMhz,
    .min_freq_mhz = 80,
    .light_sleep_enable = true
};
```

**Risk:** Very low. Same fields, different type name.

**Note:** Check whether `esp_pm_config_esp32_t` still compiles as a deprecated alias. If so, this can be deferred. But it is a one-line change, so just do it.

### 2.3 Brownout Register Access (`main.cpp`)

The firmware uses direct register manipulation for brownout detector control:

```cpp
#include <soc/rtc_cntl_reg.h>

CLEAR_PERI_REG_MASK(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
SET_PERI_REG_MASK(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
```

**Status in IDF 5.x:** For the original ESP32 (not S2/S3/C3), `soc/rtc_cntl_reg.h` still exists and the register definitions are unchanged. The macros `CLEAR_PERI_REG_MASK` and `SET_PERI_REG_MASK` are defined in `soc/soc.h` which is still available (though it may not be auto-included — check).

**Action:**

- Verify compilation. If `soc/rtc_cntl_reg.h` is no longer auto-included, add an explicit `#include <soc/soc.h>` if not already present.
- The register addresses are hardware-defined and do not change between IDF versions for the same chip.

**Risk:** Low for ESP32. Would be higher if targeting ESP32-S3/C3 (different register layouts), but this project targets ESP32 only.

### 2.4 Bluetooth Memory Release (`main.cpp`)

**Current:**

```cpp
#include <esp_bt.h>
esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
```

**With `CONFIG_BT_ENABLED=n` actually taking effect**, the BT controller is never initialised and its memory regions may be automatically reclaimed. The `esp_bt.h` header may not even be available.

**Action:**

- Wrap the call in a `#if CONFIG_BT_ENABLED` guard, or
- Simply test whether the build succeeds. If `esp_bt.h` is absent when BT is disabled, remove the include and the call. The memory savings happen at compile time instead.

**Risk:** Low. The worst case is a missing header error that is trivially fixed.

### 2.5 FreeRTOS Idle Hooks (`main.cpp`)

**Current:**

```cpp
#include <esp_freertos_hooks.h>
```

Used for CPU load measurement via idle hook registration.

**Status in IDF 5.x:** The `esp_freertos_hooks.h` API is still available but may have moved. Check if the include path changed.

**Action:** Verify compilation. If the header moved, update the include path.

**Risk:** Low.

---

## Phase 3: Likely-Safe Code (Verify, Don't Rewrite)

These areas are expected to work without changes but should be verified during the build and initial testing.

### 3.1 WiFi APIs

**`WiFiManager.cpp`:**

- `WiFi.mode()`, `WiFi.begin()`, `WiFi.disconnect()`, `WiFi.reconnect()` — unchanged
- `WiFi.setTxPower()` — unchanged
- `esp_wifi_set_protocol()` — unchanged (IDF API, not Arduino wrapper)
- `esp_wifi_set_config()` — unchanged
- `esp_wifi_set_ps()` — unchanged (implicitly via Arduino WiFi power save)

**One deprecation to watch:** `WiFiServer::available()` is deprecated in Arduino 3.x. If used, replace with `WiFiServer::accept()`. However, the project uses the `WebServer` library which handles this internally.

### 3.2 WebServer

`CpapWebServer.cpp` uses:

- `WebServer` class — unchanged in Arduino 3.x
- `server->on()`, `server->send()`, `server->sendContent()` — unchanged
- `server->client()` returning `WiFiClient` — unchanged
- SSE via `WiFiClient` direct writes — unchanged

**Note:** `WiFiClient::flush()` no longer clears the receive buffer in Arduino 3.x; it is now a no-op (transmit flush). A new `clear()` method replaces the old receive-clear behaviour. Check if any code relies on `flush()` to clear incoming data.

### 3.3 WiFiClientSecure / HTTPClient

`SleepHQUploader.cpp` and `OTAManager.cpp` use:

- `WiFiClientSecure` — unchanged
- `HTTPClient` — unchanged
- TLS certificate methods (`setCACert`, `setInsecure`) — unchanged

### 3.4 SD_MMC

`SDCardManager.cpp` uses:

- `SD_MMC.begin()`, `SD_MMC.end()`, `SD_MMC.cardType()` — unchanged
- `gpio_set_drive_capability()` — unchanged (IDF API)
- `SDMMC_FREQ_DEFAULT` — unchanged

**One change in IDF 5.x:** `sdmmc_host_pullup_en()` is removed; replaced by `SDMMC_SLOT_FLAG_INTERNAL_PULLUP` flag. Verify this project does not call it (it does not, based on the audit).

### 3.5 LittleFS

`LittleFS` is supported in Arduino 3.x. No changes expected.

### 3.6 GPIO / RTC GPIO

- `gpio_wakeup_enable()` — unchanged
- `esp_sleep_enable_gpio_wakeup()` — unchanged
- `rtc_gpio_hold_dis()` — unchanged
- `rtc_gpio_deinit()` — unchanged
- `driver/gpio.h` — unchanged
- `driver/rtc_io.h` — unchanged

### 3.7 libsmb2 (External Component)

Pure C library in `components/libsmb2`. No Arduino or IDF API dependencies. Expected to compile unchanged.

### 3.8 ArduinoJson

External library (`bblanchon/ArduinoJson@^6.21.3`). No framework dependency. Works with any Arduino core version.

---

## Phase 4: Post-Migration Improvements (Optional)

These are not required for the migration to succeed, but become possible once on pioarduino.

### 4.1 Lower `min_freq_mhz` for DFS

**Current** (in `main.cpp`):

```cpp
.min_freq_mhz = 80,
```

With PM actually working, you could experiment with lowering this:

```cpp
.min_freq_mhz = 10,   // XTAL frequency — ~2-3 mA CPU idle draw
```

**Caution (from C54):** WiFi requires 80 MHz when active. The WiFi driver holds its own PM lock to prevent sleeping below 80 MHz during WiFi operations. However, between DTIM intervals in modem-sleep, the CPU could drop to 10 MHz. This needs careful testing — verify that PCNT counting, web server responsiveness, and SSE push are not degraded.

### 4.2 Delete `scripts/rebuild_mbedtls.py`

Once the migration is validated and the custom mbedTLS rebuild is confirmed unnecessary, delete the script and its cache directory.

### 4.3 Migrate PCNT to New Driver (if deferred from Phase 2)

If Phase 2 used `CONFIG_PCNT_SUPPRESS_DEPRECATE_WARN=y` to defer, complete the PCNT migration here.

### 4.4 Audit `esp_bt_controller_mem_release` Cleanup

With BT compiled out, confirm the ~30 KB DRAM savings are achieved at boot without the runtime release call. Remove the call and the `#include <esp_bt.h>` if confirmed.

### 4.5 Consider `custom_sdkconfig` for Board-Specific Overrides

pioarduino supports a `custom_sdkconfig` option in `platformio.ini` for per-environment SDK configuration overrides. This could be useful if different build targets (e.g., a future ESP32-S3 variant) need different power settings.

---

## Validation Checklist

Every item must pass before the migration branch is merged.

### Build Validation

- [ ] `pio run -e pico32-ota` compiles cleanly (zero errors)
- [ ] Warning count is equal to or lower than the current build
- [ ] Binary size fits within OTA partition constraints
- [ ] `sdkconfig.defaults` entries are reflected in the generated sdkconfig (spot-check key values)

### Boot Validation

- [ ] Device boots without crash or panic
- [ ] Serial log shows PM configuration success (not the current "PM configuration failed" error)
- [ ] Serial log shows correct CPU frequency
- [ ] Free heap and max alloc are at least as good as current firmware
- [ ] BT memory is not reserved (if `CONFIG_BT_ENABLED=n` is effective)

### WiFi Validation

- [ ] WiFi connects successfully
- [ ] TX power is applied correctly (check log messages)
- [ ] Modem sleep modes work (MIN_MODEM and MAX_MODEM)
- [ ] mDNS discovery works within the 60-second window
- [ ] WiFi reconnect after deliberate disconnect works
- [ ] Brownout detector relaxation around WiFi connect still works

### Web Server Validation

- [ ] Web UI loads correctly (status page, config page, logs page)
- [ ] SSE live log stream works
- [ ] Config editor works (read and write)
- [ ] Upload trigger from web UI works
- [ ] SD Activity Monitor works (PCNT data visible)
- [ ] OTA update page loads

### Upload FSM Validation

- [ ] IDLE → LISTENING → ACQUIRING → UPLOADING → RELEASING → COOLDOWN cycle completes
- [ ] Cloud (SleepHQ) upload succeeds — TLS handshake, file upload, hash verification
- [ ] SMB upload succeeds — connect, directory creation, file write, disconnect
- [ ] Dual-backend (CLOUD + SMB) session completes without heap exhaustion
- [ ] Upload abort from web UI works
- [ ] Cooldown timer works correctly

### Power Management Validation

- [ ] PM lock acquire/release log messages appear during FSM transitions
- [ ] In IDLE state, current draw is measurably lower than current firmware (if measurement hardware available)
- [ ] Light-sleep is entered during IDLE/COOLDOWN (verify via `esp_pm_dump_locks()` or current measurement)
- [ ] GPIO wakeup from light-sleep works (CS_SENSE triggers LISTENING)
- [ ] No watchdog timeouts during sleep/wake transitions

### SD Card Validation

- [ ] SD card mount succeeds (both 1-bit and 4-bit modes)
- [ ] Drive strength reduction still applied
- [ ] Compatibility remount on release works
- [ ] TrafficMonitor detects bus activity correctly

### Stability Validation

- [ ] 24-hour soak test without crash or watchdog reboot
- [ ] Multiple upload cycles complete successfully
- [ ] Heap does not leak across sessions
- [ ] No brownout regressions on test hardware

---

## Migration Sequence (Recommended Order)

```
Phase 0:  Create branch
            │
Phase 1:  ├── 1.1  Update platformio.ini
          ├── 1.2  (sdkconfig.defaults — no change needed)
          └── 1.3  Disable rebuild_mbedtls.py in extra_scripts
            │
          First build attempt — expect errors
            │
Phase 2:  ├── 2.1  PCNT migration (or suppress warning to defer)
          ├── 2.2  esp_pm_config_esp32_t → esp_pm_config_t
          ├── 2.3  Verify brownout register includes
          ├── 2.4  Guard or remove esp_bt_controller_mem_release
          └── 2.5  Verify FreeRTOS idle hook includes
            │
          Second build attempt — should compile
            │
Phase 3:  Flash and test
          ├── Boot validation
          ├── WiFi validation
          ├── Web server validation
          ├── Upload FSM validation
          ├── Power management validation
          ├── SD card validation
          └── Stability soak test
            │
Phase 4:  Optional improvements
          ├── 4.1  Experiment with min_freq_mhz = 10
          ├── 4.2  Delete rebuild_mbedtls.py
          ├── 4.3  Complete PCNT migration (if deferred)
          ├── 4.4  Clean up BT release code
          └── 4.5  Consider custom_sdkconfig
            │
          Merge to main when all validation passes
```

---

## Estimated Effort

| Phase | Effort | Notes |
|---|---|---|
| Phase 0 | 5 min | Branch creation |
| Phase 1 | 15 min | Config changes only |
| Phase 2 | 1-3 hours | PCNT is the bulk; rest is trivial |
| Phase 3 | 1-2 days | Real-device testing, multiple upload cycles |
| Phase 4 | 1-2 hours | Optional cleanup and experiments |

**Total: ~2-3 days of focused work**, assuming access to test hardware and at least one CPAP machine for SD card handoff testing.

---

## Rollback Plan

If the migration introduces regressions that cannot be resolved in a reasonable timeframe:

1. Do not merge the branch
2. `main` remains on the current framework — fully releasable
3. Document any findings (build issues, API incompatibilities) for a future attempt
4. Continue shipping on the current framework with existing runtime power mitigations

The branch-based approach means **zero risk to the current release pipeline**.

---

## Summary

This migration is:

- **Moderate effort** — the main code change is PCNT, everything else is config or trivial renames
- **Moderate risk** — the risk is in regression, not in API complexity
- **Strategically valuable** — removes framework ceilings, enables real PM/light-sleep, eliminates build hacks

Per the C54 analysis: frame this as a **platform upgrade that enables future power work**, not as a standalone brownout fix. The strongest immediate brownout mitigations remain hardware-side (bulk capacitors) and config-side (conservative TX power, modem sleep, 1-bit SD mode).
