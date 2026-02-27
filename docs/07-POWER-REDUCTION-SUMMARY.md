# Power Reduction Analysis & Recommendations

## 1. Problem Statement

Some Singapore-made AirSense 11 CPAP machines (platform `R390-447/1`, radio module `AIR11M1`) cannot supply sufficient current to the SD WIFI PRO card. The ESP32-PICO-D4 on the SD WIFI PRO can draw up to **370 mA peak** during WiFi TX (802.11b), while the CPAP's SD card slot is designed for passive NAND flash drawing <100 mA. The goal is to reduce peak and average current consumption to the absolute practical minimum while preserving all existing functionality (web UI, mDNS, uploads, OTA).

---

## 2. Current Implementation Audit

### 2.1 Boot Sequence Power Profile

The current boot sequence in `src/main.cpp` `setup()` has significant power inefficiencies:

| Boot Phase | Duration | CPU Freq | WiFi State | Estimated Current |
|:-----------|:---------|:---------|:-----------|:------------------|
| Pin init + `delay(1000)` | 1s | **240 MHz** | Off | ~50-68 mA |
| Cold boot stabilization | 15s | **240 MHz** | Off | ~50-68 mA |
| Smart Wait (PCNT loop) | 5s+ | **240 MHz** | Off | ~50-68 mA |
| SD card access + config load | ~1s | **240 MHz** | Off | ~50-68 mA |
| `setCpuFrequencyMhz()` from config | instant | configurable | Off | varies |
| `WiFi.begin()` + connect | ~5-15s | config (default 240) | **TX at 19.5dBm** | **205-370 mA peaks** |
| `applyPowerSettings()` | instant | config | Connected | varies |

**Key problems:**
- CPU runs at **240 MHz for 20+ seconds** before config is loaded (cold boot)
- WiFi connects at **maximum TX power (19.5dBm)** before `applyPowerSettings()` is called
- No protocol restriction — ESP32 may negotiate **802.11b** (370 mA peak TX)
- The massive current draw during WiFi init and association happens *before* any power settings are applied

### 2.2 Runtime Power Settings

**Config defaults** (`src/Config.cpp`):
- `cpuSpeedMhz` = **240** (maximum)
- `wifiTxPower` = **POWER_HIGH** (19.5 dBm — maximum)
- `wifiPowerSaving` = **SAVE_NONE** (`WiFi.setSleep(false)` — no power saving at all)

These defaults mean that a user who doesn't explicitly configure power settings runs at **absolute maximum power consumption** at all times.

### 2.3 Power Override Leak (Bug)

`src/SleepHQUploader.cpp` line 713:
```cpp
WiFi.setSleep(false);  // Ensure WiFi is in high performance mode for upload
```

This call disables WiFi power saving during Cloud uploads but **never restores it afterward**. After any Cloud upload session, WiFi power saving remains permanently disabled for the remainder of that boot cycle, regardless of what the user configured.

### 2.4 Missing Optimizations

| Optimization | Status |
|:-------------|:-------|
| Bluetooth disabled/memory released | **Not implemented** — BT stack may still be linked and consuming RAM/power |
| 802.11b protocol disabled | **Not implemented** — ESP32 can fall back to 802.11b (370 mA peak) |
| ESP-IDF Power Management (DFS) | **Not implemented** — no `CONFIG_PM_ENABLE`, no `esp_pm_configure()` |
| Auto Light-sleep | **Not implemented** |
| `listen_interval` for MAX_MODEM | **Not configured** — even if user selects MAX, no interval is set |
| Compile-time TX power cap | **Not set** in `sdkconfig.defaults` |
| Early boot CPU throttling | **Not implemented** — 240 MHz until config load |

### 2.5 What Currently Works Well

- **PCNT-based TrafficMonitor** — reliable, hardware-accelerated bus activity detection. No reason to change this.
- **Smart Wait** — prevents bus contention during boot. Essential and should be preserved.
- **SD card multiplexer management** — GPIO 26 control is correct and immediate.

---

## 3. Analysis of Preliminary Reports

### 3.1 Report 1 (06-POWER-REDUCTION-PRELIMINARY-1.md)

**Useful recommendations adopted:**
- Bluetooth memory release via `esp_bt_controller_mem_release()` — **Yes, adopt**
- Disable 802.11b via `esp_wifi_set_protocol()` — **Yes, adopt**
- TX power clamp — **Yes, adopt** (but not as aggressive as suggested)
- `WIFI_PS_MAX_MODEM` with `listen_interval` — **Partially adopt** (see Section 4)
- CPU at 80 MHz — **Yes, adopt as default**
- Boot staggering — **Partially adopt** (early CPU throttle, but we already have delay)

**Recommendations NOT adopted:**
- **Deep Sleep topology** — The device must maintain WiFi for web UI, mDNS, and OTA. Deep sleep loses WiFi association and all RAM state. Not compatible with the always-accessible web interface requirement.
- **GPIO interrupt for CS_SENSE replacing PCNT** — The PCNT counter approach is proven reliable. GPIO interrupt-based approach described in the report is less robust (ISR race conditions, debouncing issues). No reason to replace working hardware.
- **Auto Light-sleep** — Deferred to Phase 3. PCNT peripheral is clock-gated during light sleep, which would prevent SD bus activity detection in LISTENING state. Requires GPIO wakeup integration and more testing.
- **Floating GPIO management** — Low impact (~1-2 mA) compared to RF optimizations. Worth doing eventually but not a priority.
- **Physical LED removal** — Hardware modification, out of scope for firmware.
- **2 dBm TX power** — Too aggressive. May cause connection failures for users with routers in another room.

### 3.2 Report 2 (06-POWER-REDUCTION-PRELIMINARY-2.md)

More measured approach. Key agreement points:
- Fully disable Bluetooth — **Yes**
- WiFi power save (Modem-sleep) — **Yes**
- DFS with appropriate frequency range — **Yes**
- Reduce TX power — **Yes**
- Batch SD operations — **Already implemented** (upload sessions are batched)
- Deep sleep between daily cycles — **Not adopted** (same reason as above)

### 3.3 Official Espressif Documentation Analysis

The Espressif low-power WiFi guide provides measured current data for ESP32 station mode:

| Mode | DTIM 1 | DTIM 3 | DTIM 10 |
|:-----|:-------|:-------|:--------|
| **Modem-sleep (no DFS)** | 31.12 mA | 28.81 mA | 29.66 mA |
| **Modem-sleep + DFS** | 22.65 mA | 21.89 mA | 20.01 mA |
| **Auto Light-sleep** | 3.34 mA | 2.33 mA | 2.19 mA |

These are average currents during idle WiFi connection. **Peak currents during TX remain 130-250 mA** regardless of sleep mode — the sleep mode only affects what happens *between* transmissions.

This confirms that:
- TX power reduction is the primary lever for **peak** current
- Sleep mode selection is the primary lever for **average** current
- Both matter for power-constrained hosts

---

## 4. Recommendations

### Priority Legend
- 🔴 **Critical** — Highest impact on peak current reduction, should be implemented first
- 🟠 **High** — Significant power savings, straightforward to implement
- 🟡 **Medium** — Good savings, requires more careful integration
- 🟢 **Future** — Requires testing, deferred to later phase

---

### 4.1 🔴 Disable 802.11b Protocol (Force OFDM Only)

**Impact:** Eliminates 370 mA peak TX scenario entirely. Single most important safety measure.

**Rationale:** 802.11b uses DSSS modulation at up to 370 mA. The ESP32 can silently fall back to 802.11b when signal is weak or during initial association. By restricting to 802.11g/n (OFDM), peak TX current drops to 205-270 mA. Virtually all home routers from the last 15 years support 802.11g/n.

**Implementation:**
```cpp
#include "esp_wifi.h"
// After WiFi.mode(WIFI_STA) but before WiFi.begin():
esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
```

**Risk:** Extremely low. Any router that doesn't support 802.11g/n is 20+ years old.

---

### 4.2 🔴 Reduce Default TX Power to 8.5 dBm (Configurable)

**Impact:** Directly reduces RF amplifier bias current during every transmission.

**Rationale:** The device operates inside a CPAP machine in a bedroom, typically within 5-15 meters of a WiFi router. 19.5 dBm (maximum) is designed for outdoor/long-range scenarios. 8.5 dBm provides ~7 mW of output power — sufficient for reliable indoor connectivity at bedroom distances while drastically reducing the current spike during each TX burst.

**Default value: 8.5 dBm** — This provides a safety margin over the minimum while being substantially lower than the current 19.5 dBm default. Users experiencing connectivity issues can increase it via config.

**Config key:** `WIFI_TX_PWR` — Retain existing key but change the default value and the available named levels:

| Level Name | dBm Value | Arduino Constant | Use Case |
|:-----------|:----------|:-----------------|:---------|
| `LOW` | 5.0 dBm | `WIFI_POWER_5dBm` | Router in same room, very close proximity |
| `MID` (new default) | 8.5 dBm | `WIFI_POWER_8_5dBm` | Normal bedroom use (recommended) |
| `HIGH` | 11.0 dBm | `WIFI_POWER_11dBm` | Router in adjacent room |
| `MAX` | 19.5 dBm | `WIFI_POWER_19_5dBm` | Long range / weak signal (old default) |

Additionally, cap the compile-time maximum via `sdkconfig.defaults`:
```
CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11
```
This caps the PHY-level maximum at 11 dBm even if runtime code tries to go higher. Users who truly need 19.5 dBm would need a custom build, but this prevents accidental peak power spikes during RF calibration and association.

**Risk:** Some users with routers far from the bedroom may need to increase TX power. The config key provides this escape hatch.

---

### 4.3 🔴 Set CPU to 80 MHz Immediately at Boot

**Impact:** Saves ~30-40 mA during the entire 20+ second boot sequence.

**Rationale:** Currently the CPU runs at 240 MHz for the entire boot process (15s stabilization + Smart Wait + config load) before the config-specified frequency is applied. 80 MHz is the minimum for WiFi operation and is sufficient for all boot tasks (SD card I/O, config parsing, PCNT initialization). This should be the **very first instruction** in `setup()`, before even the serial init.

**Implementation:**
```cpp
void setup() {
    setCpuFrequencyMhz(80);  // Immediately reduce power draw
    Serial.begin(115200);
    // ... rest of setup
}
```

After config is loaded, the frequency can be updated to the user's configured value if different (though 80 MHz should become the new default).

**Risk:** None. All boot operations are I/O-bound, not CPU-bound.

---

### 4.4 🔴 Disable Bluetooth at Compile Time + Runtime Release

**Impact:** Eliminates BT-related current leakage (~2-5 mA) and frees ~30 KB RAM.

**Rationale:** The firmware uses WiFi exclusively. Bluetooth is never initialized or used, but the Arduino-ESP32 framework may still link BT components and allocate memory for them. Disabling at compile time prevents this entirely.

**Implementation (two layers):**

**Layer 1 — Compile time** (`sdkconfig.defaults`):
```
CONFIG_BT_ENABLED=n
```
This prevents the BT stack from being compiled into the firmware, saving flash and RAM.

**Layer 2 — Runtime belt-and-suspenders** (early in `setup()`):
```cpp
#include "esp_bt.h"
esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
```
If the compile-time flag doesn't fully strip BT (framework dependencies), this releases the controller memory. If BT is already disabled at compile time, this call is a safe no-op.

**Risk:** None. BT is not used.

---

### 4.5 🟠 Change Default WiFi Power Saving to MIN_MODEM

**Impact:** Reduces average idle current from ~120 mA (no sleep) to ~22-31 mA (Modem-sleep).

**Rationale:** The current default `SAVE_NONE` keeps the WiFi radio active at all times, even when no data is being transmitted or received. This wastes ~90-100 mA continuously. `WIFI_PS_MIN_MODEM` powers down the RF between DTIM beacons while waking for every DTIM, preserving broadcast/multicast reception (critical for mDNS).

**Why MIN_MODEM and not MAX_MODEM as default:**
- mDNS relies on multicast packets. `MAX_MODEM` skips DTIM beacons based on `listen_interval`, which means multicast mDNS queries may be missed.
- The user explicitly requires mDNS to continue working.
- `MIN_MODEM` wakes at every DTIM (typically every 100-300ms depending on router), ensuring mDNS queries are received.
- `MAX_MODEM` can be offered as a config option for users who don't need responsive mDNS.

**Default change:** `SAVE_NONE` → `SAVE_MID` (which maps to `WIFI_PS_MIN_MODEM`)

Rename the enum values for clarity in config:

| Config Value | WiFi Mode | mDNS Impact | Current |
|:-------------|:----------|:------------|:--------|
| `NONE` | No power save | Full responsiveness | ~120 mA idle |
| `MODEM` (new default) | `WIFI_PS_MIN_MODEM` | Reliable, slight latency | ~22-31 mA idle |
| `MAX` | `WIFI_PS_MAX_MODEM` | May miss queries | ~20-22 mA idle |

**Risk:** Slight increase in mDNS response latency (up to one DTIM interval, typically 100-300ms). Web UI page loads may feel marginally slower. Both are acceptable trade-offs.

---

### 4.6 🟠 Fix SleepHQ WiFi.setSleep(false) Leak

**Impact:** Prevents permanent loss of WiFi power saving after Cloud uploads.

**Rationale:** `SleepHQUploader.cpp` line 713 calls `WiFi.setSleep(false)` before each file upload but never restores the configured power saving mode afterward. This means that after any Cloud upload, WiFi power saving is permanently disabled until the next reboot.

**Fix:** Save and restore the power saving mode around Cloud uploads. Or better yet, remove this override entirely — the upload throughput impact of MIN_MODEM is negligible since the WiFi stack automatically takes a power management lock during active TX/RX, preventing sleep during actual data transfer.

**Recommendation:** Remove `WiFi.setSleep(false)` from `SleepHQUploader.cpp` entirely. The ESP-IDF WiFi driver already holds a `ESP_PM_CPU_FREQ_MAX` lock during active WiFi operations, ensuring full performance when actually transmitting. Modem-sleep only engages between packet bursts when the radio would be idle anyway.

**Risk:** Very low. ESP-IDF's internal power lock mechanism handles this correctly.

---

### 4.7 🟠 Apply TX Power Before WiFi.begin()

**Impact:** Prevents full-power TX spikes during WiFi association.

**Rationale:** Currently `applyPowerSettings()` is called **after** WiFi connects. This means the initial WiFi scan, association, and DHCP all happen at 19.5 dBm. The compile-time `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` cap (from 4.2) partially addresses this, but we should also call `esp_wifi_set_max_tx_power()` as early as possible after `esp_wifi_init()`.

**Implementation:** Move TX power setting into `connectStation()`, called right after `WiFi.mode(WIFI_STA)` and before `WiFi.begin()`:
```cpp
WiFi.mode(WIFI_STA);
esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
// Note: esp_wifi_set_max_tx_power() must be called after esp_wifi_start(),
// but WiFi.mode() calls esp_wifi_init() + esp_wifi_start() internally.
// So setTxPower() can be called here.
WiFi.setTxPower(configuredPower);
WiFi.begin(ssid, password);
```

**Risk:** None.

---

### 4.8 🟡 Enable DFS (Dynamic Frequency Scaling)

**Impact:** Reduces average CPU current during idle periods by ~8-10 mA.

**Rationale:** With `CONFIG_PM_ENABLE=y` in sdkconfig and `esp_pm_configure()` at runtime, the CPU can automatically scale down to `min_freq_mhz` when no tasks hold performance locks. The WiFi stack automatically acquires a `ESP_PM_CPU_FREQ_MAX` lock during active WiFi operations, ensuring full speed when needed.

**Implementation:**

`sdkconfig.defaults`:
```
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_HZ=1000
```

Runtime (after WiFi connect):
```cpp
#include "esp_pm.h"
esp_pm_config_t pm_config = {
    .max_freq_mhz = 160,  // Boost to 160 for WiFi/TLS bursts
    .min_freq_mhz = 80,   // Floor at 80 MHz (WiFi minimum)
    .light_sleep_enable = false  // Phase 3 — not yet
};
esp_pm_configure(&pm_config);
```

**Why max=160 instead of 80:** Setting both max and min to 80 disables DFS entirely (no scaling range). With max=160, the WiFi stack can temporarily boost to 160 MHz for TLS handshakes and active transfers, then drop back to 80 MHz when idle. This provides a good balance of performance and power savings.

**Why min=80 not 40:** The WiFi PHY requires a minimum APB clock of 80 MHz. Setting min below 80 MHz would force WiFi to disconnect when the CPU scales down, which is not what we want.

**Risk:** Low. DFS is well-tested in ESP-IDF. The main consideration is that `CONFIG_PM_ENABLE` adds a small overhead to FreeRTOS tick processing for lock management.

**Prerequisite:** Add `vTaskDelay()` calls in the main loop during IDLE and LISTENING states so the IDLE task can actually run and DFS can engage. Without yields, the CPU stays at max frequency continuously.

---

### 4.9 🟡 Add Explicit Yields in Main Loop

**Impact:** Enables DFS to actually engage. Reduces CPU utilization from 100% to near-zero during idle states.

**Rationale:** The current `loop()` function runs continuously without delays. This keeps the CPU at 100% utilization even when doing nothing (IDLE/LISTENING states). With DFS enabled (4.8), adding `vTaskDelay()` allows the FreeRTOS IDLE task to run, which triggers frequency scaling down.

**Implementation:** Add small delays at the end of `loop()` based on current FSM state:

| State | Suggested Delay | Rationale |
|:------|:----------------|:----------|
| IDLE | 100 ms | Only checks once per 60s anyway |
| LISTENING | 50 ms | TrafficMonitor.update() runs at sampling rate, 50ms is sufficient |
| COOLDOWN | 100 ms | Just timing a cooldown period |
| MONITORING | 10 ms | Live data for web UI, lower delay |
| UPLOADING | 10 ms | Web server responsiveness during upload |

**Risk:** Very low. The current code doesn't have timing requirements tighter than 10ms in any state.

---

### 4.10 🟢 Auto Light-sleep (Future — Phase 3)

**Impact:** Would reduce idle current from ~22 mA (Modem-sleep+DFS) to ~2-3 mA.

**Rationale:** Auto Light-sleep suspends the CPU entirely between DTIM beacons, achieving dramatic power savings. However, it clock-gates all digital peripherals including PCNT, which would prevent SD bus activity detection during sleep.

**Prerequisites for implementation:**
1. Configure GPIO wakeup on CS_SENSE (pin 33) to wake from light sleep when CPAP starts SD access
2. Verify PCNT counter state is preserved across light sleep cycles
3. Test mDNS reliability with light sleep enabled
4. Test web server response latency

**Not recommended for initial implementation** due to PCNT interaction complexity. The gains from Phase 1+2 (Modem-sleep + DFS + TX reduction + 802.11b disable + BT release) should be sufficient to resolve the power issue.

---

## 5. Estimated Power Profile After Changes

### 5.1 Peak Current (During WiFi TX)

| Scenario | Before | After |
|:---------|:-------|:------|
| 802.11b TX at 19.5 dBm | **370 mA** | Eliminated (802.11b disabled) |
| 802.11n TX at 19.5 dBm | **250 mA** | Eliminated (TX capped) |
| 802.11n TX at 8.5 dBm | N/A | **~120-150 mA** (estimated) |
| 802.11n TX at 5.0 dBm | N/A | **~100-120 mA** (estimated) |

### 5.2 Average Current (WiFi Connected, Idle)

| Configuration | Current |
|:-------------|:--------|
| Before (240 MHz, no sleep, 19.5 dBm) | **~120-130 mA** |
| After Phase 1 (80 MHz, MIN_MODEM, 8.5 dBm, no 802.11b) | **~22-31 mA** |
| After Phase 2 (+ DFS + yields) | **~20-25 mA** |
| Future Phase 3 (+ Auto Light-sleep) | **~2-3 mA** |

### 5.3 Boot Sequence

| Phase | Before | After |
|:------|:-------|:------|
| Pre-WiFi (15s cold boot) | ~50-68 mA (240 MHz) | **~20-25 mA** (80 MHz, BT released) |
| WiFi association | ~250-370 mA peaks | **~120-150 mA peaks** (8.5 dBm, no 802.11b) |

---

## 6. Implementation Plan

### Phase 1 — Critical (Implement First)

1. **`sdkconfig.defaults`** — Add compile-time settings:
   - `CONFIG_BT_ENABLED=n`
   - `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11`

2. **`src/main.cpp` `setup()`** — Early power reduction:
   - `setCpuFrequencyMhz(80)` as first instruction
   - `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` before any delays

3. **`src/WiFiManager.cpp` `connectStation()`** — Before WiFi.begin():
   - `esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)`
   - `WiFi.setTxPower(configured_power)` before `WiFi.begin()`

4. **`src/Config.cpp`** — Change defaults:
   - `cpuSpeedMhz` default: 240 → **80**
   - `wifiTxPower` default: `POWER_HIGH` → **`POWER_MID` (8.5 dBm)**
   - `wifiPowerSaving` default: `SAVE_NONE` → **`SAVE_MID` (MIN_MODEM)**
   - Add `POWER_MAX` level (19.5 dBm) for users who need it

5. **`src/SleepHQUploader.cpp`** — Remove `WiFi.setSleep(false)` on line 713

### Phase 2 — DFS Integration

6. **`sdkconfig.defaults`** — Add:
   - `CONFIG_PM_ENABLE=y`
   - `CONFIG_FREERTOS_HZ=1000`

7. **`src/main.cpp`** — After WiFi connect, configure DFS:
   - `esp_pm_configure()` with max=160, min=80

8. **`src/main.cpp` `loop()`** — Add state-appropriate `vTaskDelay()` calls

### Phase 3 — Future (Requires Testing)

9. Auto Light-sleep with GPIO wakeup for CS_SENSE
10. Investigate further TX power reduction to 5 dBm as default

---

## 7. Configuration Reference (Post-Implementation)

### New Defaults

```ini
# Power management (new defaults — optimized for AirSense 11 compatibility)
CPU_SPEED_MHZ = 80
WIFI_TX_PWR = MID
WIFI_PWR_SAVING = MODEM
```

### All TX Power Options

| Config Value | dBm | Description |
|:-------------|:----|:------------|
| `LOW` | 5.0 | Minimum practical. Router must be very close. |
| `MID` | 8.5 | **Default.** Good for typical bedroom placement. |
| `HIGH` | 11.0 | Router in adjacent room or through walls. |
| `MAX` | 19.5 | Maximum power. Only if other settings fail. Compile-time cap may limit this to 11 dBm. |

### All Power Saving Options

| Config Value | Mode | mDNS | Description |
|:-------------|:-----|:-----|:------------|
| `NONE` | No sleep | Full | Maximum responsiveness, highest power. |
| `MODEM` | MIN_MODEM | Reliable | **Default.** Wakes every DTIM for broadcasts. |
| `MAX` | MAX_MODEM | May miss | Maximum WiFi savings, may miss mDNS queries. |

---

## 8. Risks and Mitigations

| Risk | Mitigation |
|:-----|:-----------|
| TX power too low → WiFi connection fails | Configurable `WIFI_TX_PWR` with `MAX` option. Log RSSI on connect for diagnostics. |
| 802.11b-only router → can't connect | Extremely unlikely (802.11g has been standard since 2003). Document in README. |
| DFS causes WiFi instability | WiFi driver holds PM lock during active operations. Well-tested in ESP-IDF. |
| MIN_MODEM slows web UI | Impact is one DTIM interval (~100-300ms) per request. Barely perceptible. |
| 80 MHz CPU slows TLS handshake | With DFS, CPU boosts to 160 MHz during TLS. Without DFS, 80 MHz TLS takes ~2x longer but still completes. |
| Config change breaks existing users | Existing `config.txt` files with explicit settings are unaffected. Only users relying on defaults get new behavior — which is the desired outcome. |
