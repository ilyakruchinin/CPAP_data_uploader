# Post-Replatform Power Optimisation Plan

## Document Purpose

This document assesses the power optimisation landscape **after** the pioarduino migration (doc 35) has been completed. It catalogues:

1. **What was removed or disabled** during migration stabilisation and why
2. **What is currently active** and delivering power savings
3. **Recovery plan** — how to safely reintroduce each removed optimisation
4. **New opportunities** — power savings newly available on IDF 5.5.x / Arduino ESP32 3.3.x that were not in the original plan

This is a research and planning document only. **No code changes.**

---

## Current Platform

| Property | Value |
|---|---|
| PlatformIO platform | pioarduino `stable` (source-compiled hybrid) |
| Arduino core | 3.3.7 |
| ESP-IDF | 5.5.2 |
| Board | ESP32-PICO-D4 (`pico32`) |
| Flash | 4 MB |
| Free DRAM | ~121 KB |
| CPU default | 80 MHz |

---

## 1. What Was Removed or Disabled During Migration

### 1.1 Light-Sleep (DISABLED)

**Original plan (doc 35 §1.2, §4.1):** `light_sleep_enable = true` + `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`

**Current state:**
- `light_sleep_enable = false` in `esp_pm_configure()` call
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE=n` in both `sdkconfig.defaults` and `custom_sdkconfig`
- GPIO wakeup code commented out (`gpio_wakeup_enable` / `esp_sleep_enable_gpio_wakeup`)

**Why removed:** GPIO wakeup on CS_SENSE (GPIO 33) conflicts with the new IDF 5.x PCNT driver that owns the same pin. The PCNT driver registers its own GPIO interrupt handler; calling `gpio_wakeup_enable()` with `GPIO_INTR_LOW_LEVEL` on the same pin causes continuous interrupt firing on the shared GPIO 32-39 interrupt handler, leading to a software panic ~2 minutes after boot.

**Impact:** Light-sleep was the single largest expected power saving from the migration. Without it, the CPU stays awake between DTIM intervals in IDLE/COOLDOWN states, consuming ~15-20 mA more than it would with light-sleep enabled. The PM lock acquire/release infrastructure is still in place and functional — it just has no light-sleep to gate.

### 1.2 mbedTLS Customisations (REMOVED — cannot recover)

**Original plan (doc 35 §1.2):** All mbedTLS `sdkconfig.defaults` entries become effective:
- `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y` (16 KB in / 4 KB out)
- `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y`
- `CONFIG_MBEDTLS_CHACHA20_C=n`
- `CONFIG_MBEDTLS_AES_256_C=n`
- `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n`
- `CONFIG_MBEDTLS_SSL_SESSION_TICKETS=n`
- `CONFIG_MBEDTLS_SSL_RENEGOTIATION=n`

**Current state:** All removed from both `platformio.ini` `custom_sdkconfig` and `sdkconfig.defaults`.

**Why removed:** The hybrid compile recompiles IDF libs (including mbedTLS) but does NOT recompile the Arduino framework layer (`WiFiClientSecure`). Since `WiFiClientSecure` embeds `mbedtls_ssl_context` and `mbedtls_ssl_config` structs directly, any config that changes struct sizes causes an ABI mismatch → memory corruption → deterministic `InstrFetchProhibited` crash during TLS handshake.

**Impact:** TLS connections use default (larger) mbedTLS buffers. This costs ~12-16 KB more heap during TLS sessions compared to the asymmetric buffer plan, and leaves unnecessary cipher suites compiled in. However, TLS works reliably.

**Recovery:** **Not possible** with the current hybrid compile model. Recovery requires one of:
- pioarduino adding support for recompiling the Arduino framework layer (not available)
- Switching to pure ESP-IDF (no Arduino framework) — major rewrite
- Upstream Arduino ESP32 adopting configurable mbedTLS struct sizes

This is a permanent constraint of the hybrid compile approach.

### 1.3 `CONFIG_FREERTOS_HZ=100` (NOT ACHIEVABLE)

**Original plan (doc 35 §1.2):** Reduce tick rate from 1000 Hz to 100 Hz to cut ISR overhead.

**Current state:** `CONFIG_FREERTOS_HZ=1000` is enforced by Arduino ESP32's `CMakeLists.txt` (~line 405). The hybrid compile cannot override it.

**Why:** The Arduino framework's `delay()`, `millis()`, and `micros()` implementations depend on 1000 Hz tick rate. Changing it breaks timing across the entire Arduino ecosystem.

**Impact:** ~0.5-1 mA of unnecessary ISR overhead. Minor compared to other items.

**Recovery:** Not possible without modifying the Arduino framework source. Permanent constraint.

### 1.4 Tickless Idle (DISABLED — tied to light-sleep)

**Original plan (doc 35 §1.2):** `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` + `CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3`

**Current state:** Explicitly set to `n`.

**Why:** Tickless idle is only needed for auto light-sleep. Since light-sleep is disabled (§1.1), tickless idle would serve no purpose and was disabled to avoid any interaction issues.

**Impact:** None while light-sleep is disabled. Will be re-enabled alongside light-sleep.

---

## 2. What Is Currently Active and Working

### 2.1 Dynamic Frequency Scaling (DFS) ✓

- `CONFIG_PM_ENABLE=y` — functional in hybrid compile
- `min_freq_mhz = 40` (XTAL frequency)
- `max_freq_mhz = 80` (from config, default)
- PM lock infrastructure: acquired in active FSM states, released in IDLE/COOLDOWN
- WiFi driver automatically holds `ESP_PM_APB_FREQ_MAX` lock during active RF operations
- New PCNT driver (IDF 5.x) automatically holds `ESP_PM_APB_FREQ_MAX` lock while enabled

**Savings:** In IDLE/COOLDOWN with PM lock released and WiFi in modem-sleep between DTIM intervals, CPU scales to 40 MHz. This saves ~5-10 mA vs locked 80 MHz. The 40 MHz floor (XTAL) was lowered from the original 80 MHz plan during migration.

### 2.2 Compile-Time Bluetooth Removal ✓

- `CONFIG_BT_ENABLED=n` — effective in hybrid compile
- `esp_bt.h` guarded with `#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED`
- BT controller memory never allocated

**Savings:** ~30 KB DRAM freed at compile time. Eliminates BT-related leakage current (~0.5-1 mA).

### 2.3 WiFi TX Power Cap ✓

- `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10` — effective (caps PHY at 10 dBm)
- Runtime `WiFi.setTxPower()` applied after `WiFi.begin()` (deferred correctly)
- Brownout-recovery mode forces `POWER_LOWEST` (-1 dBm)

**Savings:** Reduces peak TX current from ~300+ mA (at 20 dBm) to ~120-150 mA (at 10 dBm). This is one of the most impactful single settings for brownout prevention.

### 2.4 WiFi Modem Sleep ✓

- `WIFI_PS_MIN_MODEM` (default) or `WIFI_PS_MAX_MODEM` (brownout recovery)
- WiFi radio powers down between DTIM beacon intervals

**Savings:** ~15-70 mA depending on DTIM interval and AP configuration. `MAX_MODEM` saves more but increases latency.

### 2.5 Reduced WiFi RX Buffers ✓

- `CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=6` (down from default 8)

**Savings:** ~3 KB DRAM. Minor but contributes to overall heap headroom.

### 2.6 CPU Throttle at Boot ✓

- `setCpuFrequencyMhz(80)` as the first line of `setup()`
- Reduces boot sequence from 240 MHz to 80 MHz immediately

**Savings:** ~30-40 mA during the 20+ second boot sequence.

### 2.7 Timed mDNS ✓

- mDNS runs for 60 seconds after boot/reconnect, then stops
- Eliminates continuous multicast group membership and radio wakes

**Savings:** Eliminates periodic multicast wakes (~1-3 mA average).

### 2.8 Brownout Detector Management ✓

- `CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_0=y` (lowest threshold ~2.43V)
- Configurable OFF/RELAXED modes for problem hardware
- RELAXED mode disables during WiFi init, re-enables after

### 2.9 PM Lock FSM Integration ✓

- `ESP_PM_CPU_FREQ_MAX` lock created and managed per FSM state
- Acquired: LISTENING, ACQUIRING, UPLOADING, RELEASING, MONITORING, COMPLETE
- Released: IDLE, COOLDOWN
- Allows DFS to scale CPU down in low-activity states

---

## 3. Recovery Plan — Reintroducing Removed Optimisations

### 3.1 Light-Sleep Recovery (HIGH PRIORITY)

This is the single most impactful recovery item. Estimated savings: **15-25 mA** in IDLE/COOLDOWN states.

#### The Problem

GPIO 33 (CS_SENSE) is used by PCNT for SD bus activity counting. The IDF 5.x PCNT driver owns this GPIO via the GPIO matrix. Calling `gpio_wakeup_enable()` on the same pin causes an interrupt conflict.

#### Viable Wakeup Alternatives

**Option A: Timer-based wakeup (RECOMMENDED — lowest risk)**

Instead of GPIO wakeup, use the RTC timer to wake from light-sleep periodically:

```c
// Wake every 100ms to check PCNT counter (same polling interval as TrafficMonitor)
esp_sleep_enable_timer_wakeup(100000);  // 100ms in microseconds
```

- **Pro:** No GPIO conflict. PCNT counter accumulates during light-sleep (hardware counter runs on APB clock, but glitch filter needs APB). The PCNT `ESP_PM_APB_FREQ_MAX` lock prevents light-sleep while PCNT is enabled.
- **Con:** The PCNT driver holds `ESP_PM_APB_FREQ_MAX` while enabled, which **blocks light-sleep**. To actually enter light-sleep, the PCNT unit must be disabled first.
- **Implementation:** Disable PCNT (`pcnt_unit_disable()`) when entering IDLE/COOLDOWN, re-enable when leaving. The PM lock is automatically released on disable. Timer wakeup provides periodic polling to check for activity via a simpler mechanism (direct GPIO read or short PCNT burst).

**Option B: EXT1 wakeup on a different GPIO**

If a separate GPIO is available that also reflects SD bus activity (e.g., another SD_MMC data line), `esp_sleep_enable_ext1_wakeup()` can be used. EXT1 works with RTC GPIOs and does not conflict with PCNT.

- **Pro:** Instant wakeup on bus activity, no polling.
- **Con:** Requires a free RTC GPIO connected to the SD bus. GPIO 33 is an RTC GPIO, but it's the same pin PCNT uses. Need a second monitoring point.
- **Feasibility:** Low — the hardware only has one CS_SENSE connection.

**Option C: PCNT disable/enable cycling**

Disable the PCNT unit in IDLE/COOLDOWN states (releasing its PM lock), use direct GPIO reads or EXT0/EXT1 wakeup for the CS_SENSE pin during sleep, then re-enable PCNT when transitioning to active states.

- **Pro:** Gets both light-sleep AND PCNT counting (in active states).
- **Con:** More complex state management. PCNT counter resets on disable.
- **Implementation:** Similar to Option A but with explicit PCNT lifecycle management.

#### Recommended Approach: Option A+C Hybrid

1. In `transitionTo()`, when entering IDLE or COOLDOWN:
   - Call `pcnt_unit_disable()` → releases `ESP_PM_APB_FREQ_MAX` lock
   - Configure `esp_sleep_enable_timer_wakeup(100000)` for 100ms periodic wake
   - The existing PM lock release allows light-sleep to engage

2. In `transitionTo()`, when leaving IDLE or COOLDOWN:
   - Call `pcnt_unit_enable()` → reacquires APB lock, resumes counting
   - Disable timer wakeup (or leave it — harmless when PM lock is held)

3. During IDLE/COOLDOWN periodic wakes (every 100ms):
   - Quick GPIO read on CS_SENSE to detect bus activity
   - If activity detected, transition out of IDLE

**Prerequisites:**
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` must be re-enabled
- `light_sleep_enable = true` in `esp_pm_configure()`
- `esp_sleep_enable_timer_wakeup()` configured

**Risk:** Medium. Requires careful testing of PCNT disable/enable cycling and verification that no pulses are missed during the transition window.

**Effort:** 2-4 hours implementation + testing.

### 3.2 DFS Floor at 10 MHz (LOW PRIORITY)

**Original plan (doc 35 §4.1):** Lower `min_freq_mhz` from 80 to 10 MHz.

**Current state:** Already lowered to 40 MHz (XTAL). Going to 10 MHz is possible but:

- WiFi requires 80 MHz when active (WiFi driver holds its own lock)
- PCNT glitch filter requires APB at 80 MHz (PCNT driver holds its own lock)
- 10 MHz only applies when ALL PM locks are released AND CPU is awake between light-sleep intervals
- The window where 10 MHz applies (awake, no locks, between sleeps) is very narrow

**Savings:** Marginal — perhaps 1-2 mA in the narrow window. The CPU at 40 MHz already draws very little (~5 mA).

**Recommendation:** Not worth the added complexity and risk. Keep 40 MHz floor.

---

## 4. New Opportunities on IDF 5.5.x

### 4.1 Flash Leakage Workaround During Light-Sleep (NEW)

IDF 5.5 introduces `CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y` which reduces flash power consumption during light-sleep without fully powering down flash (which is risky with filter capacitors).

**How it works:** The flash chip is put into a low-power standby mode during light-sleep, reducing its leakage current from ~1-3 mA to ~50-100 uA, without the risks of full power-down.

**Applicability:** Directly applicable once light-sleep is re-enabled (§3.1). No code changes needed — Kconfig only.

**Savings:** ~1-2 mA during light-sleep periods.

**Config:**
```ini
CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y
```

**Risk:** Low. This is a well-tested IDF feature. Should be enabled by default in IDF 5.5.

### 4.2 PCNT Driver DFS Awareness (NEW — already benefiting)

The new IDF 5.x PCNT driver (`driver/pulse_cnt.h`) is DFS-aware. It automatically holds `ESP_PM_APB_FREQ_MAX` while the unit is enabled, ensuring the glitch filter operates correctly. The legacy driver was NOT DFS-aware.

**Current benefit:** Already active. The PCNT driver correctly manages its PM lock.

**Future benefit:** When light-sleep recovery (§3.1) disables PCNT in IDLE/COOLDOWN, the PM lock is automatically released, enabling DFS to reach the 40 MHz floor AND allowing light-sleep.

### 4.3 WiFi `failure_retry_cnt` (NEW)

IDF 5.5 adds `failure_retry_cnt` to `wifi_sta_config_t`, allowing the WiFi driver to automatically retry connection failures without application intervention. This reduces the CPU time spent in reconnect loops.

**Applicability:** Could simplify `WiFiManager::connectStation()` reconnect logic. Reduces active CPU time during WiFi instability.

**Savings:** Indirect — less CPU active time during reconnect attempts.

**Risk:** Low.

### 4.4 `esp_sleep_pd_config()` for RTC Domain Power-Down (NEW for light-sleep)

IDF 5.5 provides fine-grained control over which RTC power domains remain active during light-sleep:

```c
// Power down RTC peripherals during light-sleep (if not needed for wakeup)
esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_AUTO);
// Power down RTC fast memory during light-sleep
esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_AUTO);
```

**Applicability:** Once light-sleep is enabled, this can reduce light-sleep current by powering down unused RTC domains.

**Savings:** ~50-200 uA per domain. Small but cumulative.

**Risk:** Low, provided no RTC peripherals are needed during sleep.

### 4.5 SDMMC Driver DFS Improvements (NEW)

The IDF 5.x SDMMC driver is DFS-aware and automatically holds `ESP_PM_APB_FREQ_MAX` during transactions. This means SD card operations correctly prevent frequency scaling that could corrupt bus timing.

**Current benefit:** Already active implicitly through the Arduino SD_MMC wrapper.

### 4.6 `esp_pm_dump_locks()` Diagnostic (NEW)

IDF 5.x exposes `esp_pm_dump_locks()` which prints all currently held PM locks to the console. This is invaluable for diagnosing why light-sleep isn't being entered.

**Applicability:** Add as a diagnostic web endpoint or periodic log dump. Useful during light-sleep recovery testing.

```c
// Add to web server or periodic diagnostic
esp_pm_dump_locks(stdout);
```

### 4.7 WiFi Listen Interval Tuning (EXISTING — not yet exploited)

The WiFi driver supports configuring the listen interval, which controls how many DTIM intervals the station can sleep through in modem-sleep mode:

```c
wifi_config_t wifi_config;
esp_wifi_get_config(WIFI_IF_STA, &wifi_config);
wifi_config.sta.listen_interval = 10;  // Sleep through 10 DTIM intervals
esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
```

Default is typically 3. Increasing this extends the modem-sleep duration between wakes.

**Savings:** With DTIM=1 at 100ms beacon interval:
- `listen_interval=3` (default): wake every 300ms
- `listen_interval=10`: wake every 1000ms

Each avoided wake saves ~15-20 mA for ~5ms = ~0.08 mJ. Over time, this is meaningful.

**Trade-off:** Higher listen interval increases latency for incoming packets (web server requests, mDNS queries). Acceptable in IDLE/COOLDOWN but not during active operations.

**Implementation:** Set high listen interval in IDLE/COOLDOWN, reset to default in active states. Could be integrated with the PM lock FSM.

**Risk:** Low-Medium. Need to verify web server responsiveness at higher intervals.

### 4.8 `CONFIG_ESP_WIFI_SLP_IRAM_OPT` and `CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME` (NEW)

IDF 5.5 introduces WiFi sleep optimisation Kconfig options:

- `CONFIG_ESP_WIFI_SLP_IRAM_OPT=y` — Places WiFi sleep-related code in IRAM for faster sleep/wake transitions, reducing the time the radio is active during wakes.
- `CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME` — Minimum time the WiFi stays active after a wake. Lower values allow faster return to sleep.

**Savings:** Reduces the active time window during each modem-sleep wake cycle.

**Risk:** Low. IRAM opt costs ~2 KB of IRAM but reduces wake active time.

### 4.9 Disable PCNT During IDLE/COOLDOWN (NEW approach)

This is not a platform feature per se, but a new approach enabled by the IDF 5.x PCNT driver's clean enable/disable lifecycle:

The new PCNT driver has a clear state machine: **init → enabled → running → stopped → disabled → deleted**. Calling `pcnt_unit_disable()` cleanly releases the PM lock and detaches from the GPIO without destroying the unit or channel configuration. `pcnt_unit_enable()` reattaches and reacquires the lock.

This was not practical with the legacy PCNT driver which had no clean disable/enable cycle.

**Implementation:** In IDLE/COOLDOWN, disable the PCNT unit (releases `ESP_PM_APB_FREQ_MAX` lock). Use simple GPIO reads (or timer-wakeup + GPIO read) to detect SD activity. On detection, re-enable PCNT and transition to LISTENING.

**Savings:** Enables both DFS to 40 MHz AND light-sleep during IDLE/COOLDOWN. Without disabling PCNT, the `ESP_PM_APB_FREQ_MAX` lock keeps APB at 80 MHz and blocks light-sleep.

**This is the key enabler for light-sleep recovery (§3.1).**

---

## 5. Implementation Priority Matrix

| # | Item | Savings Est. | Effort | Risk | Dependencies |
|---|---|---|---|---|---|
| 1 | Light-sleep recovery via PCNT disable/enable cycling (§3.1 + §4.9) | **15-25 mA** | 2-4 hours | Medium | §4.9 |
| 2 | Re-enable tickless idle (`CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`) | Required for #1 | 5 min | Low | #1 |
| 3 | Flash leakage workaround (§4.1) | 1-2 mA | 5 min | Low | #1 |
| 4 | WiFi listen interval tuning in IDLE/COOLDOWN (§4.7) | 2-5 mA avg | 1 hour | Low-Med | None |
| 5 | WiFi sleep IRAM opt (§4.8) | 0.5-1 mA avg | 5 min | Low | None |
| 6 | `esp_pm_dump_locks()` diagnostic (§4.6) | 0 (diagnostic) | 30 min | None | None |
| 7 | RTC domain power-down during light-sleep (§4.4) | 0.1-0.2 mA | 15 min | Low | #1 |

### Recommended Implementation Order

```
Phase A: Diagnostics (no risk, immediate value)
  └── A.1  Add esp_pm_dump_locks() to web UI / periodic log

Phase B: Light-Sleep Recovery (highest impact)
  ├── B.1  PCNT disable/enable in transitionTo() for IDLE/COOLDOWN
  ├── B.2  Re-enable CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
  ├── B.3  Set light_sleep_enable = true in esp_pm_configure()
  ├── B.4  Configure esp_sleep_enable_timer_wakeup(100000) for periodic wake
  ├── B.5  GPIO read on CS_SENSE during periodic wake to detect activity
  └── B.6  Test: verify light-sleep entry/exit, PCNT resumes, upload cycle works

Phase C: WiFi Optimisations (low-hanging fruit)
  ├── C.1  CONFIG_ESP_WIFI_SLP_IRAM_OPT=y
  ├── C.2  WiFi listen_interval tuning in IDLE/COOLDOWN (high) vs active (default)
  └── C.3  Test: verify web server responsiveness at higher listen interval

Phase D: Flash / RTC Optimisations (polish)
  ├── D.1  CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y
  └── D.2  esp_sleep_pd_config() for RTC domain power-down
```

---

## 6. What Cannot Be Recovered

| Item | Reason | Permanent? |
|---|---|---|
| mbedTLS customisations (asymmetric buffers, cipher removal) | ABI mismatch with precompiled WiFiClientSecure | Yes (hybrid compile constraint) |
| `CONFIG_FREERTOS_HZ=100` | Enforced by Arduino framework CMakeLists.txt | Yes (Arduino framework constraint) |
| DFS floor below 40 MHz | Marginal benefit, WiFi/PCNT locks dominate | Not worth pursuing |

---

## 7. Estimated Total Recovery Potential

| State | Current Draw (est.) | After Recovery (est.) | Savings |
|---|---|---|---|
| IDLE (WiFi modem-sleep, no light-sleep) | ~35-45 mA | — | — |
| IDLE (WiFi modem-sleep + light-sleep + PCNT disabled) | — | ~15-25 mA | **15-25 mA** |
| COOLDOWN (same as IDLE) | ~35-45 mA | ~15-25 mA | **15-25 mA** |
| UPLOADING (TLS active) | ~120-180 mA | ~120-180 mA | 0 (CPU locked) |
| LISTENING (PCNT active, WiFi idle) | ~40-50 mA | ~40-50 mA | 0 (APB lock held) |

The dominant recovery is light-sleep in IDLE/COOLDOWN states. Since the device spends the vast majority of its time in these states (typically 23+ hours/day), even a 15 mA reduction is highly significant for brownout margin.

---

## 8. Summary

The migration successfully delivered:
- **DFS** (40-80 MHz scaling) — **active**
- **Compile-time BT removal** — **active**
- **PHY TX power cap** — **active**
- **Modem-sleep** — **active**
- **PCNT DFS awareness** — **active** (new driver benefit)

The primary casualty was **light-sleep**, which was the largest expected power saving. Recovery is feasible via PCNT disable/enable cycling (§3.1 + §4.9) and should be the top implementation priority.

The mbedTLS and FREERTOS_HZ limitations are permanent constraints of the hybrid compile model and are accepted trade-offs for the stability gained.

New IDF 5.5 opportunities (flash leakage workaround, WiFi sleep IRAM opt, listen interval tuning) provide additional incremental savings that were not in the original plan.

**Total estimated recovery: 15-25 mA in IDLE/COOLDOWN** — achievable with medium effort and medium risk.
