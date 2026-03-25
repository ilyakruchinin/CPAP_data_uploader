# Power Optimisation Analysis CO46

## Document Purpose

This document is a comprehensive, grounded analysis of power optimisation strategies for the CPAP Data Uploader firmware. It builds on the earlier `31-ANALYSIS-C54.md` with a **complete codebase audit**, **hard evidence on framework migration paths**, and **concrete recommendations** ranked by impact, risk, and feasibility.

**Strict constraint**: This is a research document only. No code changes are made or implied as immediate actions.

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [Current Power Implementation Audit](#current-power-implementation-audit)
3. [Precompiled SDK Constraints — Hard Evidence](#precompiled-sdk-constraints--hard-evidence)
4. [Framework Migration Analysis](#framework-migration-analysis)
5. [Concrete Optimisation Opportunities](#concrete-optimisation-opportunities)
6. [Advice Triage: What Was Wrong in Previous Documents](#advice-triage-what-was-wrong-in-previous-documents)
7. [Measurement-First Strategy](#measurement-first-strategy)
8. [Recommendations](#recommendations)

---

## Executive Summary

### What the firmware already does well

The CPAP Data Uploader has an **unusually mature power management implementation** for an Arduino-framework project. A full audit confirms **15+ distinct power mitigations** are already in place at the firmware level. The brownout issues experienced by some users are **not caused by missing software optimisations** — they stem from hardware-level current transients on the 3.3V rail that software alone cannot fully eliminate.

### The single biggest remaining software lever

The precompiled Arduino-ESP32 2.0.17 framework (ESP-IDF 4.4.x) ships with `CONFIG_PM_ENABLE` **disabled** in its precompiled libraries. This means:

- `esp_pm_configure()` always returns `ESP_ERR_NOT_SUPPORTED`
- Dynamic Frequency Scaling (DFS) cannot engage — the CPU stays at whatever `setCpuFrequencyMhz()` set
- Automatic light-sleep between DTIM intervals is impossible
- PM locks (`esp_pm_lock_create/acquire/release`) are no-ops

The firmware already has PM lock infrastructure (`g_pmLock` in `main.cpp`, `transitionTo()` FSM helper) but it **does nothing** because the underlying PM subsystem is compiled out.

### The framework migration question

**pioarduino** (v55.03.37, Arduino 3.3.7 + ESP-IDF 5.5.2) is the most viable migration path. It offers:

- `custom_sdkconfig` support — Arduino projects can set `CONFIG_PM_ENABLE=y` without abandoning Arduino APIs
- Source-compiled ESP-IDF libraries that respect `sdkconfig.defaults`
- Elimination of the `rebuild_mbedtls.py` hack (mbedTLS config respected natively)
- Active community (137K+ downloads on latest release, maintained by Jason2866)

**Migration risk is moderate**, not trivial. The PCNT legacy driver used by `TrafficMonitor` is deprecated in ESP-IDF 5.x (still functional but should be migrated). WiFi, SD_MMC, and TLS APIs are largely compatible. The main work is testing, not rewriting.

### Bottom line

| Category | Status |
|----------|--------|
| Runtime mitigations | Comprehensive — little room for improvement without framework change |
| Compile-time optimisations | Blocked by precompiled framework |
| Framework migration | pioarduino is viable, well-supported, moderate effort |
| Hardware mitigation | Bulk capacitor on 3.3V rail is the single most effective brownout fix |
| Measurement | No current profiling data exists — any further work should be measurement-driven |

---

## Current Power Implementation Audit

### Complete inventory of power-relevant code

Every power-related code path was audited across the entire firmware. Below is the definitive list.

#### 1. CPU Frequency Management (`main.cpp`)

| What | Where | Details |
|------|-------|---------|
| Immediate 80 MHz throttle on boot | `setup()` line ~460 | `setCpuFrequencyMhz(80)` — first line of setup, before any WiFi or peripheral init |
| DFS `max_freq_mhz` uses config value | PM configure block ~540 | `config.getCpuSpeedMhz()` used (not hardcoded 160) — prevents di/dt spikes from unexpected frequency jumps |
| DFS `min_freq_mhz` set to 10 | PM configure block ~540 | Lowest possible value for XTAL-derived REF_TICK |

**Assessment**: Correct and optimal for current framework. The 80 MHz lock is the right choice — it prevents the DFS governor from boosting to 160 MHz during TLS crypto, which was identified as the root cause of di/dt brownout on Singapore AS11 units.

#### 2. Bluetooth Memory Release (`main.cpp`)

| What | Where | Details |
|------|-------|---------|
| BT controller memory freed | `setup()` early | `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` |

**Assessment**: This is a runtime-only release. The precompiled SDK has `CONFIG_BT_ENABLED=y`, so BT code is still in flash and some RAM is still consumed by BT stacks until this call. A framework with `CONFIG_BT_ENABLED=n` at compile time would save ~60KB flash and ~30KB RAM permanently.

#### 3. WiFi Power Management (`WiFiManager.cpp`, `main.cpp`)

| What | Where | Details |
|------|-------|---------|
| TX power pre-set before association | `applyTxPowerEarly()` | Sets WiFi STA mode + TX power before `begin()` to prevent full-power scan spikes |
| 802.11b protocol disabled | `connectStation()` | `esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G \| WIFI_PROTOCOL_11N)` — eliminates legacy 11b high-power frames |
| Configurable TX power levels | `applyPowerSettings()` | 5 levels: LOWEST (-1 dBm), LOW (2 dBm), MID (5 dBm), HIGH (8.5 dBm), MAX (11 dBm, PHY-capped) |
| Configurable power save modes | `applyPowerSettings()` | SAVE_NONE, SAVE_MID (MIN_MODEM), SAVE_MAX (MAX_MODEM + listen_interval=10) |
| Brownout-recovery WiFi degradation | `setup()` ~511 | Forces POWER_LOWEST + SAVE_MAX when previous boot was brownout reset |
| Brownout detector relaxation during reconnect | `loop()` ~1190 | Disables brownout detector during WiFi reconnect, re-enables after |

**Assessment**: This is thorough. The early TX power set is a notable detail — most projects set TX power after connection, allowing a full-power scan burst. The `listen_interval=10` for MAX_MODEM is correct and meaningful (radio sleeps for 10× DTIM intervals instead of waking every beacon).

#### 4. mDNS Timed Shutdown (`main.cpp`)

| What | Where | Details |
|------|-------|---------|
| mDNS starts on boot | `setup()` ~519 | `wifiManager.startMDNS(config.getHostname())` |
| mDNS stops after 60s | `loop()` ~1080 | `MDNS.end()` — eliminates multicast group membership and radio wakes |
| mDNS skipped in brownout recovery | `setup()` ~511 | No mDNS at all when `g_brownoutRecoveryBoot` is true |

**Assessment**: Good optimisation. mDNS multicast keeps the radio awake more frequently than necessary. The 60-second window is a reasonable UX compromise.

#### 5. SD Card Power Management (`SDCardManager.cpp`, `pins_config.h`)

| What | Where | Details |
|------|-------|---------|
| GPIO drive strength reduced to CAP_0 | `takeControl()` | All 6 SDIO pins set to ~5mA drive (from default ~20mA) before mount |
| Drive strength restored on release | `releaseControl()` | Restored to CAP_2 (~20mA) for CPAP compatibility |
| 1-bit SDIO mode option | `takeControl()` | `ENABLE_1BIT_SD_MODE` config key — uses 1-bit bus to reduce simultaneous pin switching |
| 4-bit compatibility remount on release | `releaseControl()` | When 1-bit mode used, brief 4-bit remount restores bus width for CPAP |
| SD bus switch control | `setControlPin()` | GPIO 26 controls MUX; GPIO 27 for power control |
| 500ms stabilisation delay after switch | `takeControl()` | Allows SD card internal init after voltage/bus transition |

**Assessment**: The GPIO drive strength reduction is a sophisticated optimisation rarely seen in hobby projects. It directly reduces di/dt on the 3.3V rail. The 1-bit mode option trades throughput for lower peak current — appropriate for power-sensitive deployments.

#### 6. PM Lock Infrastructure (`main.cpp`)

| What | Where | Details |
|------|-------|---------|
| PM lock created in setup | `setup()` ~570 | `esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "upload", &g_pmLock)` |
| Lock released in IDLE/COOLDOWN | `transitionTo()` | Low-power states release lock to allow DFS/light-sleep |
| Lock acquired in active states | `transitionTo()` | UPLOADING, LISTENING, etc. hold lock to prevent frequency drops |

**Assessment**: The infrastructure is **correct and complete** — but **non-functional** because `CONFIG_PM_ENABLE` is not set in the precompiled SDK. The `esp_pm_lock_create()` call succeeds (returns a handle) but `acquire`/`release` are no-ops. If PM were enabled, this code would work as intended with zero changes.

#### 7. Loop Yields for DFS (`main.cpp`)

| What | Where | Details |
|------|-------|---------|
| State-appropriate vTaskDelay | `loop()` end ~1259 | IDLE/COOLDOWN: 100ms, LISTENING: 50ms, UPLOADING/MONITORING: 10ms, default: 10ms |
| taskYIELD in SMB write loop | `SMBUploader.cpp` ~677 | `taskYIELD()` between write chunks allows FreeRTOS idle task to run |

**Assessment**: These yields are necessary for DFS to work (the idle task must run to trigger frequency scaling). Currently they only prevent busy-looping — with PM enabled, they would enable actual frequency reduction.

#### 8. Upload Task Architecture (`main.cpp`, `FileUploader.cpp`)

| What | Where | Details |
|------|-------|---------|
| Upload runs on Core 0 | `handleUploading()` ~851 | Keeps Core 1 free for web server + main loop |
| 12KB task stack | `handleUploading()` ~854 | Minimised from 16KB — TLS buffers on heap |
| WDT unsubscribe for IDLE0 | `handleUploading()` ~846 | Prevents WDT timeout during TLS crypto on Core 0 |
| Log flush skipped during uploads | `loop()` ~1048 | Avoids SPI flash writes overlapping SD/WiFi/TLS |
| SD mount inside task | `uploadTaskFunction()` | Task created at high heap, SD mounted after |
| TLS on-demand connect | `uploadTaskFunction()` note | Cloud phase connects TLS only when preflight confirms work |

**Assessment**: The architecture carefully avoids concurrent high-current operations (SD + WiFi + flash). The on-demand TLS connect saves ~28KB heap and ~11s when no cloud work exists.

#### 9. Brownout Recovery System (`main.cpp`)

| What | Where | Details |
|------|-------|---------|
| Brownout reset detection | `setup()` ~490 | Checks `esp_reset_reason() == ESP_RST_BROWNOUT` |
| Degraded boot mode | Various | No mDNS, POWER_LOWEST TX, SAVE_MAX power save |
| Auto-clear after successful upload | `handleComplete()` ~998 | Proves device can sustain full upload cycle |
| Configurable brownout detector | `Config.cpp` | OFF, RELAXED, ON modes |

**Assessment**: This is a well-designed graduated response system. The RELAXED mode (disable during WiFi connect, re-enable after) is particularly good — WiFi association is the single highest-current event.

#### 10. CPU Load Measurement (`main.cpp`)

| What | Where | Details |
|------|-------|---------|
| Idle hooks on both cores | `_idleHook0`, `_idleHook1` | Increment counters when idle task runs |
| Diagnostics endpoint | Web server | Reports CPU load % per core |

**Assessment**: Useful for diagnostics but no power impact. Could be used to validate that yields are allowing sufficient idle time.

#### 11. Web Server and SSE (`CpapWebServer.cpp`, `main.cpp`)

| What | Where | Details |
|------|-------|---------|
| SSE single-client limit | `pushSseLogs()` | Only one SSE client at a time — bounds radio usage |
| Status snapshot on-demand | `handleApiStatus()` | No periodic polling/rebuild — only when client requests |
| Web server disabled during task | `handleUploading()` ~833 | `uploader->setWebServer(nullptr)` prevents concurrent handleClient |

**Assessment**: The on-demand status approach avoids unnecessary CPU/radio work. SSE is the lightest transport for live logs (no polling). The web server itself has near-zero power cost when no client is connected.

#### 12. Network Recovery (`NetworkRecovery.cpp`)

| What | Where | Details |
|------|-------|---------|
| Coordinated WiFi cycle | `tryCoordinatedWifiCycle()` | Guarded concurrency, 45s cooldown, prevents ASSOC_LEAVE storms |
| Brownout-aware reconnect | `loop()` ~1190 | Detector disabled during reconnect in RELAXED mode |
| No WiFi cycle when link up | `SMBUploader.cpp` | Avoids tearing down active connections |

**Assessment**: Careful design prevents the double-reconnect race condition that caused 6-hour offline hangs. The brownout detector relaxation during reconnect is critical — WiFi association draws ~300-380mA peak.

---

## Precompiled SDK Constraints — Hard Evidence

The following was verified directly from the installed SDK at:
`/root/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32/sdkconfig`

### Confirmed blocked settings

| Setting | Precompiled Value | Desired Value | Impact |
|---------|-------------------|---------------|--------|
| `CONFIG_PM_ENABLE` | **not set** | `y` | **Critical** — blocks all DFS and light-sleep |
| `CONFIG_BT_ENABLED` | `y` | `n` | ~60KB flash, ~30KB RAM wasted |
| `CONFIG_FREERTOS_HZ` | `1000` | `100` or `250` | 1000 Hz tick = 1ms interrupt overhead; 100 Hz sufficient for this project |
| `CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ` | `160` | `80` | Boot starts at 160 MHz until `setCpuFrequencyMhz(80)` runs |
| `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` | **not set** | `y` | Blocks asymmetric TLS buffers; requires rebuild_mbedtls.py hack |
| `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` | **not set** | `y` | Could save ~12KB per TLS connection |
| `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` | `20` (20 dBm) | `10` or lower | Compile-time cap; runtime `setTxPower()` still works but PHY calibration uses full power |
| `CONFIG_ESP32_BROWNOUT_DET_LVL` | `0` (2.43V) | Configurable | Strictest level; more permissive level could prevent false triggers |

### Settings that ARE correctly set

| Setting | Value | Notes |
|---------|-------|-------|
| `CONFIG_ESP32_BROWNOUT_DET` | `y` | Brownout detector enabled (firmware manages at runtime) |
| `CONFIG_SPIRAM` | `y` (but `BOOT_INIT` off) | PSRAM configured but not auto-initialized — correct for PICO-D4 which has no PSRAM |
| `CONFIG_ESP_SLEEP_*` workarounds | Various `y` | Sleep-related hardware workarounds enabled |

### The `sdkconfig.defaults` gap

The project has a well-crafted `sdkconfig.defaults` file with 102 lines of configuration including PM enable, BT disable, WiFi TX cap, FreeRTOS tick rate, and TLS optimisations. **None of these are applied** because the precompiled Arduino framework ignores `sdkconfig.defaults` entirely — it uses its own prebuilt `.a` libraries with baked-in configuration.

The `rebuild_mbedtls.py` script is a creative workaround that replaces the precompiled `libmbedtls.a` with a custom-compiled version. This approach cannot be extended to the WiFi stack, FreeRTOS, or PM subsystem because those components have deep cross-dependencies.

---

## Framework Migration Analysis

### Option A: pioarduino (Recommended)

**Version**: v55.03.37 (stable) — Arduino 3.3.7 + ESP-IDF 5.5.2
**Maintainer**: Jason2866 (active, responsive)
**Downloads**: 137,000+ on latest release
**License**: Apache 2.0

#### What pioarduino provides

1. **`custom_sdkconfig` support** — The killer feature. Arduino projects can define `sdkconfig.defaults` and have them **actually applied** during source compilation. This means:
   - `CONFIG_PM_ENABLE=y` → real DFS and light-sleep
   - `CONFIG_BT_ENABLED=n` → compile-time BT removal (~60KB flash, ~30KB RAM)
   - `CONFIG_FREERTOS_HZ=100` → reduced tick interrupt overhead
   - `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` → native asymmetric TLS buffers
   - `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y` → dynamic TLS buffer sizing
   - All the settings in the project's existing `sdkconfig.defaults` would **actually work**

2. **Source-compiled ESP-IDF libraries** — No more precompiled `.a` files with baked-in config. The entire ESP-IDF is compiled from source with the project's `sdkconfig.defaults` applied.

3. **Eliminates `rebuild_mbedtls.py`** — The custom mbedTLS rebuild script becomes unnecessary because `sdkconfig.defaults` is respected natively.

4. **ESP-IDF 5.5.2** — Latest stable ESP-IDF with improved WiFi power management, better light-sleep support, and numerous bugfixes.

#### Migration breaking changes (Arduino 2.x → 3.x)

Based on the official Espressif migration guide (`2.x_to_3.0`):

| API Area | Breaking Change | Impact on This Project |
|----------|----------------|----------------------|
| **LEDC** | `ledcSetup`/`ledcAttachPin` → `ledcAttach` | **None** — project doesn't use LEDC |
| **Timer** | Complete API redesign | **None** — project doesn't use hardware timers |
| **I2S** | Complete API redesign | **None** — project doesn't use I2S |
| **RMT** | API redesign | **None** — project doesn't use RMT |
| **WiFi** | `flush()` no longer clears RX buffer; new `clear()` method | **Low risk** — project doesn't call `WiFiClient::flush()` for buffer clearing |
| **WiFi** | `WiFiServer::available()` deprecated → `accept()` | **Low risk** — check web server usage |
| **UART** | Default pin changes for ESP32 UART1/UART2 | **None** — project uses default UART0 |
| **PCNT** | Legacy driver deprecated (still functional in IDF 5.x) | **Medium risk** — `TrafficMonitor.cpp` uses legacy `pcnt_config_t` API. Works but should be migrated to new handle-based API to avoid removal in IDF 6.0 |
| **SD_MMC** | No breaking changes documented | **Low risk** — `SD_MMC.begin()` API unchanged |
| **WiFiClientSecure** | TLS internals updated | **Low risk** — `setCACert()`, `setInsecure()`, `connect()` API unchanged |
| **esp_wifi** | Internal changes, API compatible | **Low risk** — `esp_wifi_set_protocol()`, `esp_wifi_set_config()` unchanged |

#### Migration effort estimate

| Task | Effort | Risk |
|------|--------|------|
| Change `platformio.ini` platform URL | Trivial | Low |
| Remove `rebuild_mbedtls.py` and related build hooks | Low | Low |
| Test WiFi connection, TX power, power save modes | Medium | Medium |
| Test SD_MMC mount/unmount in both 1-bit and 4-bit modes | Medium | Low |
| Test TLS connections (SleepHQ upload, OTA) | Medium | Medium |
| Test SMB connections (libsmb2 is pure socket) | Low | Low |
| Migrate PCNT from legacy to new driver API | Medium | Low |
| Test web server + SSE + mDNS | Low | Low |
| Verify `sdkconfig.defaults` are applied (check build output) | Low | Low |
| Test PM enable + DFS + light-sleep integration | High | Medium |
| Full regression test of upload FSM | High | Medium |
| **Total** | **~2-3 days focused work** | **Medium overall** |

#### pioarduino `platformio.ini` change

```ini
; Current:
platform = espressif32

; pioarduino stable:
platform = https://github.com/pioarduino/platform-espressif32/releases/download/stable/platform-espressif32.zip
```

### Option B: Official ESP-IDF + Arduino as Component

**What it is**: Use ESP-IDF as the primary framework with Arduino as a managed component via Espressif's component registry.

**Pros**:
- Full ESP-IDF control from day one
- Official Espressif support
- All sdkconfig options available

**Cons**:
- Requires CMake build system (not PlatformIO native)
- All Arduino library dependencies need manual component registration
- `WebServer`, `SD_MMC`, `WiFiClientSecure`, `ESPmDNS` — all need to be sourced as components
- libsmb2 integration needs CMake component wrapper
- Significantly more migration effort than pioarduino
- Build system is completely different

**Assessment**: **Not recommended** for this project. pioarduino provides the same ESP-IDF 5.x benefits while keeping the PlatformIO + Arduino workflow intact.

### Option C: Stay on Current Framework

**Pros**:
- Zero migration risk
- Known-working codebase

**Cons**:
- PM permanently blocked
- `rebuild_mbedtls.py` hack remains necessary
- BT wastes 60KB flash + 30KB RAM
- FreeRTOS 1000 Hz tick overhead
- Cannot leverage any compile-time power optimisation

**Assessment**: Viable if brownout issues are addressed purely via hardware (capacitors). All existing runtime mitigations remain effective.

---

## Concrete Optimisation Opportunities

### Tier 1: Available Without Framework Change (LOW risk)

#### 1.1 Hardware: Bulk Capacitor on 3.3V Rail

**Impact**: HIGH — directly addresses brownout root cause
**Risk**: LOW (hardware modification)
**Effort**: LOW (soldering one component)

A 100-470µF low-ESR capacitor (tantalum or polymer electrolytic) across the ESP32's 3.3V supply pins acts as a local energy reservoir. During WiFi TX bursts (300-380mA peak, ~1-2ms duration), the capacitor supplies the transient current instead of the upstream regulator.

This is the **single most effective brownout mitigation** because it attacks the root cause: the 3.3V rail voltage dip caused by instantaneous current demand exceeding the regulator's response time.

**Calculation**: A 330µF capacitor with 50mΩ ESR can supply 300mA for 2ms with only ~15mV voltage drop. Without the capacitor, the same current through a typical LDO's output impedance causes 100-300mV sag.

#### 1.2 Further TX Power Reduction for Brownout-Prone Users

**Impact**: MEDIUM
**Risk**: LOW
**Effort**: TRIVIAL (config change)

The firmware already supports `WIFI_TX_PWR=LOWEST` (-1 dBm) but the default is `LOW` (5 dBm). Users experiencing brownouts could be advised to try `LOWEST` in their config. The tradeoff is reduced WiFi range.

**Already available** — no code change needed, just documentation/guidance.

#### 1.3 Increase `listen_interval` for MAX_MODEM

**Impact**: LOW
**Risk**: LOW
**Effort**: TRIVIAL

Currently `listen_interval=10` for `WIFI_PS_MAX_MODEM`. Increasing to 20 or higher would reduce radio wake frequency further. However, the benefit is marginal — the radio already sleeps for most of the time. The latency increase (missed beacons = delayed incoming packets) could affect SSE responsiveness.

**Assessment**: Not worth pursuing unless measurement shows significant radio wake overhead.

### Tier 2: Available With Framework Migration (MEDIUM risk)

#### 2.1 Enable CONFIG_PM_ENABLE + DFS

**Impact**: HIGH — the biggest single software improvement possible
**Risk**: MEDIUM (requires framework migration + testing)
**Effort**: LOW once pioarduino is adopted

With PM enabled, the existing PM lock infrastructure in `main.cpp` becomes functional:
- In IDLE/COOLDOWN states: CPU drops to `min_freq_mhz` (10 MHz), saving ~70% CPU power
- In active states: CPU runs at `max_freq_mhz` (80 MHz) — no change from current behavior
- WiFi driver automatically holds PM lock during TX/RX — no manual management needed
- SDMMC driver holds APB_FREQ_MAX lock during card transactions — no SD corruption risk

**Estimated power reduction**: 20-40% during idle/listening phases (CPU at 10 MHz vs 80 MHz). During uploads, power draw is unchanged (PM lock held).

#### 2.2 Enable Automatic Light-Sleep

**Impact**: HIGH
**Risk**: MEDIUM-HIGH (needs careful testing with WiFi, SD, web server)
**Effort**: MEDIUM

With `CONFIG_PM_ENABLE=y` and `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, the chip can enter light-sleep when no PM locks are held and no tasks are ready. In light-sleep:
- CPU is clock-gated (essentially zero dynamic power)
- WiFi radio wakes only on DTIM beacons (controlled by `listen_interval`)
- GPIO wakeup is available (already configured: `esp_sleep_enable_gpio_wakeup()` in `main.cpp`)

**Caveats**:
- Peripherals are clock-gated during light-sleep — PCNT counter stops counting. This means `TrafficMonitor` cannot detect bus activity during light-sleep. The GPIO wakeup (CS_SENSE pin) would need to serve as the wake trigger instead.
- Web server responsiveness is reduced (client must wait for next DTIM wake)
- SSE push becomes bursty rather than near-real-time

**Assessment**: Best suited for IDLE/COOLDOWN states where the web UI is not actively in use. The firmware already releases the PM lock in these states — light-sleep would engage automatically.

#### 2.3 Compile-Time Bluetooth Removal

**Impact**: MEDIUM (memory savings, not direct power)
**Risk**: LOW
**Effort**: TRIVIAL (sdkconfig change)

`CONFIG_BT_ENABLED=n` removes BT from the build entirely:
- Saves ~60KB flash
- Saves ~30KB RAM
- Eliminates BT-related interrupt handlers
- `esp_bt_controller_mem_release()` becomes unnecessary

The freed RAM directly improves heap headroom for TLS connections and SMB buffers.

#### 2.4 Reduce FreeRTOS Tick Rate

**Impact**: LOW-MEDIUM
**Risk**: LOW
**Effort**: TRIVIAL (sdkconfig change)

`CONFIG_FREERTOS_HZ=100` (from 1000) reduces tick interrupt frequency by 10×:
- Less CPU wakeup overhead (relevant when light-sleep is enabled)
- `vTaskDelay(pdMS_TO_TICKS(100))` becomes `vTaskDelay(10)` instead of `vTaskDelay(100)` — same actual delay
- Timer resolution decreases from 1ms to 10ms — acceptable for this application

**Caveat**: `TrafficMonitor` samples every 100ms — well above the 10ms tick period. No impact.

#### 2.5 Native Asymmetric mbedTLS Buffers

**Impact**: MEDIUM (memory, not power directly)
**Risk**: LOW
**Effort**: TRIVIAL (sdkconfig change, remove rebuild script)

`CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` with `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384` and `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096` would be respected natively. The `rebuild_mbedtls.py` script and its complex build integration become unnecessary.

Additionally, `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y` enables dynamic TLS buffer sizing, potentially saving ~12KB per connection when actual TLS records are smaller than the maximum.

### Tier 3: Not Recommended / Already Debunked

#### 3.1 WiFi Off During Idle ❌

**Why not**: The web server, SSE live logs, OTA updates, and diagnostics all require WiFi. Turning WiFi off during idle breaks the core UX. Modem-sleep with appropriate `listen_interval` achieves 90%+ of the power savings without losing connectivity.

#### 3.2 Fixed PHY Rate ❌

**Why not**: The `esp_wifi_config_80211_tx_rate()` API forces a specific PHY rate for all frames. Lower rates (e.g., 1 Mbps) increase per-frame TX duration, keeping the radio on longer. Higher rates (e.g., MCS7) require more power per symbol. The WiFi rate adaptation algorithm already optimises this tradeoff. Overriding it is more likely to increase total energy consumption.

#### 3.3 Deep Sleep Between Uploads ❌

**Why not**: Deep sleep loses all RAM state, WiFi connection, and web server. Wake-up requires full boot sequence (8s stabilisation + WiFi connect + NTP sync). The product is designed as an always-on monitoring device with web UI access. Deep sleep is architecturally incompatible.

#### 3.4 Disable WiFi During SD Card Operations ❌

**Why not**: The upload task reads from SD and writes to WiFi simultaneously (streaming). Disabling WiFi during SD reads would require full read-into-RAM-then-transmit approach, dramatically increasing RAM usage and upload time.

#### 3.5 Use `esp_wifi_set_bandwidth()` to Force 20 MHz ❌

**Why not**: ESP32 STA mode already defaults to 20 MHz bandwidth. The 40 MHz HT40 mode is only relevant for AP mode or specific SoftAP configurations. This suggestion demonstrates a misunderstanding of ESP32 STA WiFi behavior.

---

## Advice Triage: What Was Wrong in Previous Documents

The documents in `31-A-BUNCH-OF-ADVICES.md` and the earlier analysis contained several categories of advice quality:

### Correct and Already Implemented

These suggestions were good but the firmware already had them:

1. ✅ Throttle CPU to 80 MHz
2. ✅ Release BT memory at runtime
3. ✅ Disable 802.11b protocol
4. ✅ Set WiFi TX power before association
5. ✅ Use WiFi modem sleep (MIN or MAX)
6. ✅ Reduce SD GPIO drive strength
7. ✅ Yield in main loop for DFS
8. ✅ PM lock acquire/release around active states

### Correct but Blocked by Framework

These suggestions were technically sound but impossible to implement:

1. ⚠️ Enable `CONFIG_PM_ENABLE` → blocked by precompiled SDK
2. ⚠️ Enable tickless idle → blocked (requires PM_ENABLE)
3. ⚠️ Disable BT at compile time → blocked by precompiled SDK
4. ⚠️ Cap PHY TX power at compile time → blocked by precompiled SDK
5. ⚠️ Use asymmetric TLS buffers natively → blocked (workaround exists via rebuild script)

### Incorrect or Impractical

These suggestions showed a lack of understanding of the project:

1. ❌ "Turn WiFi off when idle" → breaks web UI, SSE, OTA, diagnostics
2. ❌ "Use deep sleep between uploads" → loses all state, incompatible with always-on design
3. ❌ "Force 20 MHz WiFi bandwidth" → already the default in STA mode
4. ❌ "Disable WiFi during SD reads" → breaks streaming upload architecture
5. ❌ "Use fixed PHY rate" → likely increases total energy vs rate adaptation
6. ❌ "Set CPU to 240 MHz for faster uploads" → increases peak current dramatically, counterproductive for brownout

### Partially Correct but Oversimplified

1. ⚡ "Add capacitor" → correct hardware advice, but needs specific guidance (100-470µF, low ESR, near ESP32 VDD pins)
2. ⚡ "Upgrade framework" → correct direction, but understates migration complexity and overstates power savings
3. ⚡ "Use light-sleep" → correct in principle, but PCNT stops during light-sleep (TrafficMonitor impact), and web server latency increases

---

## Measurement-First Strategy

### The problem with optimising blind

No current profiling data exists for this device. All power analysis to date has been theoretical — based on datasheet values, code review, and brownout symptoms. Before investing significant effort in framework migration, the following measurements would provide **far more value** than any code change:

### Recommended measurement approach

#### Equipment needed
- USB power meter with logging (e.g., Power-Z KM003C, ~$30) OR
- Current shunt + oscilloscope for transient analysis
- The actual SD-WIFI-PRO hardware running production firmware

#### What to measure

| State | Expected Current | What to Look For |
|-------|-----------------|------------------|
| IDLE (no web client) | ~30-50mA | Baseline. Modem-sleep should reduce to ~15-20mA between DTIM |
| IDLE (web UI open, SSE active) | ~50-80mA | SSE push frequency, radio wake patterns |
| LISTENING (TrafficMonitor active) | ~40-60mA | PCNT + 100ms sample loop overhead |
| UPLOADING (SMB) | ~150-250mA | SD read + WiFi TX concurrent, peak spikes |
| UPLOADING (Cloud/TLS) | ~200-350mA | TLS crypto + WiFi TX, highest sustained draw |
| WiFi association | ~300-380mA | **Peak transient** — this is what causes brownout |
| WiFi scan | ~250-350mA | Only during initial connection |
| Boot (first 8 seconds) | ~80-120mA | Before WiFi, just CPU + flash + SD stabilisation |

#### Key questions measurements would answer

1. **Is modem-sleep actually engaging?** — Compare IDLE current with and without `WIFI_PS_MAX_MODEM`. If no difference, the power save mode may not be effective with the current AP.
2. **What is the actual peak transient during WiFi association?** — This determines whether a capacitor fix is sufficient or if TX power must be further reduced.
3. **How much power does the web server cost when no client is connected?** — If near-zero, the "turn off WiFi when idle" advice is definitively debunked.
4. **What is the current difference between 80 MHz and 10 MHz CPU?** — This quantifies the benefit of DFS/PM (achievable only with framework migration).
5. **Does 1-bit SD mode measurably reduce peak current vs 4-bit?** — Validates whether this config option is worth recommending.

### Measurement protocol

1. Flash production firmware with default config
2. Measure each state for 60+ seconds, log min/avg/max current
3. Repeat with `WIFI_PS_MAX_MODEM` and `WIFI_PS_NONE` to validate modem sleep
4. Capture WiFi association transient with oscilloscope (ms-level resolution)
5. Test with and without 330µF capacitor to quantify brownout margin
6. Document results in a new analysis document

---

## Recommendations

### Priority-ordered action plan

#### Phase 0: Measure (before any changes)

Profile actual current draw in every FSM state. This costs ~$30 in equipment and ~2 hours of testing. It will provide hard data to validate or invalidate every assumption in this document and all previous analyses.

**Without measurements, all further optimisation work is speculative.**

#### Phase 1: Hardware Mitigation (immediate, no firmware change)

Recommend 330µF / 6.3V polymer electrolytic capacitor across ESP32 3.3V power pins for brownout-prone users. This is the highest-impact, lowest-risk intervention. Document the recommendation with photos/guidance for the specific SD-WIFI-PRO board.

#### Phase 2: Configuration Guidance (immediate, no firmware change)

For users experiencing brownouts, provide a recommended "power-safe" config:

```ini
WIFI_TX_PWR=LOWEST
WIFI_PS=MAX
CPU_SPEED=80
BROWNOUT_DETECT=RELAXED
ENABLE_1BIT_SD_MODE=true
```

This combination minimises peak current at the cost of WiFi range and SD throughput. All these options already exist in the firmware.

#### Phase 3: Framework Migration (when measurement data justifies it)

If measurements confirm that:
- Modem sleep is engaging but idle power is still high (>40mA) → DFS/PM would help
- WiFi association transient causes brownout even with capacitor → compile-time TX cap needed
- Heap pressure from BT-enabled SDK causes TLS failures → compile-time BT removal needed

Then proceed with pioarduino migration:

1. Branch the codebase
2. Change `platformio.ini` to pioarduino stable
3. Remove `rebuild_mbedtls.py` and related build hooks
4. Migrate `TrafficMonitor.cpp` PCNT to new driver API
5. Verify `sdkconfig.defaults` are applied (check build log for `CONFIG_PM_ENABLE`)
6. Test every FSM state transition
7. Test every upload path (SMB, Cloud, dual)
8. Test brownout recovery, watchdog, reconnect paths
9. Measure again — compare before/after

#### Phase 4: PM Integration (after successful migration)

If pioarduino migration succeeds:

1. Verify `esp_pm_configure()` returns `ESP_OK` (not `ESP_ERR_NOT_SUPPORTED`)
2. Verify PM locks actually engage (log acquire/release events)
3. Measure current in IDLE with DFS active (expect 15-25mA, down from 30-50mA)
4. Optionally enable light-sleep with `light_sleep_enable=true` — but only in IDLE/COOLDOWN states where PCNT and web server are not critical
5. Measure again

---

## Appendix: SDK Configuration Comparison

### Current Precompiled SDK (Arduino-ESP32 2.0.17, ESP-IDF 4.4.x)

```
CONFIG_PM_ENABLE=             # NOT SET — PM completely disabled
CONFIG_BT_ENABLED=y           # BT enabled (wasted resources)
CONFIG_FREERTOS_HZ=1000       # 1ms tick
CONFIG_ESP32_DEFAULT_CPU_FREQ_MHZ=160
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=  # NOT SET
CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=  # NOT SET
CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=20
CONFIG_ESP32_BROWNOUT_DET_LVL=0  # Strictest (2.43V)
CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=8
```

### Desired Configuration (achievable with pioarduino)

```
CONFIG_PM_ENABLE=y
CONFIG_PM_DFS_INIT_AUTO=n     # Manual DFS control via esp_pm_configure()
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_BT_ENABLED=n           # No Bluetooth needed
CONFIG_FREERTOS_HZ=100        # 10ms tick
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=80
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y
CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10  # Compile-time cap at 10 dBm
CONFIG_ESP32_BROWNOUT_DET_LVL=0
CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=8
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=5744
CONFIG_LWIP_TCP_WND_DEFAULT=5744
```

### Estimated Impact of Full Configuration Migration

| Metric | Current | After Migration | Change |
|--------|---------|-----------------|--------|
| Idle current (no web client) | ~35-50mA (est.) | ~15-25mA (est.) | -40-50% |
| Flash usage | ~1.1MB (est.) | ~1.0MB (est.) | -60-80KB |
| Free heap at boot | ~73KB | ~100KB+ | +30KB+ |
| TLS connection headroom | Tight (~38KB ma) | Comfortable (~65KB ma) | +70% |
| WiFi association peak | ~350mA | ~200mA (with TX cap) | -43% |
| Boot CPU frequency | 160 MHz (then drops) | 80 MHz native | No initial spike |

**Note**: All "estimated" values are theoretical projections based on ESP-IDF documentation and datasheet values. Actual measurements are required to validate.

---

## Document History

- **CO46**: Full codebase audit, framework migration analysis with hard evidence, concrete recommendations with risk/effort ratings, measurement-first strategy
- **C54**: Initial analysis of advice documents against codebase, framework upgrade evaluation
