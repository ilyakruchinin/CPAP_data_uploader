# Implementation Plan — CO46

This document is the authoritative implementation plan for all regression fixes,
WiFi TX power redesign, build target changes, alias removal, and documentation
updates identified in the recent analysis series (`docs/23` through `docs/29`).

**No code changes are made in this document.** The user must review this plan
before implementation begins.

---

## Table of Contents

1. [Critical New Finding: PHY Cap Minimum Is 10 dBm, Not 2](#1-critical-new-finding-phy-cap-minimum-is-10-dbm-not-2)
2. [Boot Sequence Compound Spikes — What Else Happens Around WiFi Init](#2-boot-sequence-compound-spikes--what-else-happens-around-wifi-init)
3. [Implementation Phases](#3-implementation-phases)
   - [Phase 1: Safety — Remove ULP From SD Takeover Path](#phase-1-safety--remove-ulp-from-sd-takeover-path)
   - [Phase 2: Build Targets — Remove Non-OTA](#phase-2-build-targets--remove-non-ota)
   - [Phase 3: WiFi TX Power Redesign](#phase-3-wifi-tx-power-redesign)
   - [Phase 4: Boot Sequence Power Spike Reduction](#phase-4-boot-sequence-power-spike-reduction)
   - [Phase 5: SSE Architecture Fix](#phase-5-sse-architecture-fix)
   - [Phase 6: REBOOTING UI Fix](#phase-6-rebooting-ui-fix)
   - [Phase 7: Scheduled-Mode Force Upload](#phase-7-scheduled-mode-force-upload)
   - [Phase 8: Brownout Policy](#phase-8-brownout-policy)
   - [Phase 9: Alias Removal (No Backward Compatibility)](#phase-9-alias-removal-no-backward-compatibility)
   - [Phase 10: Documentation Sweep](#phase-10-documentation-sweep)
4. [Files Changed Per Phase](#4-files-changed-per-phase)
5. [Verification Checklist](#5-verification-checklist)

---

## 1. Critical New Finding: PHY Cap Minimum Is 10 dBm, Not 2

### The question

> Can `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` be set to 2 for devices with power
> issues? Will setting it to 2 still allow people who set `WIFI_TX_PWR` to
> `MID` (5 dBm) or `MAX` (11 dBm) to work at the reduced power just fine?

### The answer

**No. The Kconfig range for `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` is 10–20 dBm.**

The ESP-IDF v4.4 Kconfig definition is:

```
config ESP_PHY_MAX_WIFI_TX_POWER
    int "Max WiFi TX power (dBm)"
    range 10 20
    default 20
```

Source: `components/esp_phy/Kconfig` in ESP-IDF v4.4.

Setting this to 2 would be **rejected at build time**. The absolute minimum
compile-time PHY cap is **10 dBm**.

### What this means for the low-power build

The current standard build already uses `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11`.
The low-power build can only go down to **10 dBm** — a reduction of just 1 dBm.

In terms of current draw savings during the RF calibration spike:

| PHY Cap | Approximate Peak TX Current | Delta vs 11 dBm |
| :--- | :--- | :--- |
| 11 dBm | ~190–220 mA | — |
| 10 dBm | ~185–215 mA | **~5–10 mA** |

This is a **negligible** difference. A separate low-power build target solely
for the PHY cap provides almost no benefit.

### Where the real power savings come from

The meaningful power reduction for weak CPAP hardware comes from **runtime**
settings, not the compile-time PHY cap:

- `WiFi.setTxPower(WIFI_POWER_2dBm)` — reduces steady-state TX to ~150–170 mA
- `WiFi.setTxPower(WIFI_POWER_MINUS_1dBm)` — reduces to ~140–160 mA

These runtime settings work fine after WiFi starts. The brief RF calibration
spike at 10–11 dBm lasts only milliseconds. The **sustained** TX power during
association, DHCP, uploads, and web serving is what matters for devices that
brownout during operation.

### Revised recommendation

Since the PHY cap can only go from 11→10 (negligible savings), a separate
low-power build variant is **not justified** solely for the PHY cap.

Instead, the focus should be on:

1. **Exposing lower runtime TX power levels** (`LOWEST` = -1 dBm, `LOW` = 2 dBm)
   so users on weak hardware can dramatically reduce sustained TX current
2. **Reducing compound power spikes around WiFi init** (see Section 2)
3. **Advising users to place a WiFi extender near the CPAP** if they need
   `LOWEST` or `LOW` TX power

A separate low-power build target may still be useful for **other** sdkconfig
differences (e.g., different brownout threshold, different lwIP tuning), but
the PHY TX cap alone does not justify it.

**Decision**: No separate low-power OTA build. The PHY cap difference is
negligible. Instead, the standard build will use `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10`
(the absolute minimum allowed) and the runtime `MAX` level will map to 10 dBm.

---

## 2. Boot Sequence Compound Spikes — What Else Happens Around WiFi Init

### The question

> What else is happening around the same time WiFi gets initialised that spikes
> power, and can it be slightly delayed until WiFi has stabilised?

### Boot sequence around WiFi init (current)

```
Line 451  sdManager.releaseControl()        — SD card released
Line 457  setCpuFrequencyMhz(targetCpuMhz)  — ⚡ CPU FREQUENCY CHANGE
Line 464  wifiManager.setupEventHandlers()   — lightweight, no spike
Line 467  wifiManager.applyTxPowerEarly()    — ⚡ WiFi.mode(WIFI_STA) = RF CALIBRATION SPIKE
Line 470  wifiManager.connectStation()       — ⚡ WiFi.begin() = SCAN + ASSOCIATION + DHCP
Line 493  wifiManager.startMDNS()            — ⚡ mDNS MULTICAST ANNOUNCEMENT
Line 500  esp_pm_configure()                 — DFS/PM configuration (lightweight)
```

### Identified compound spike sources

#### Spike A: CPU frequency change immediately before WiFi init

At line 457, if the user has `CPU_SPEED_MHZ=160` in their config, the CPU
jumps from 80→160 MHz. This triggers a **PLL relock** that causes a brief
current spike (~10–20 mA for a few milliseconds).

This happens **immediately before** the WiFi RF calibration spike at line 467.
The two spikes can compound on a marginal power supply.

**Fix**: Delay `setCpuFrequencyMhz(targetCpuMhz)` until **after** WiFi has
connected and stabilised. During boot, the extra CPU speed is not needed — the
boot sequence at this point is I/O-bound (WiFi connection), not CPU-bound.
The `esp_pm_configure()` call at line 500 already sets up DFS with the target
frequency, so the CPU will scale up naturally when needed.

#### Spike B: mDNS multicast immediately after WiFi connects

At line 493, `wifiManager.startMDNS()` is called immediately after WiFi
connects. mDNS startup involves multicast announcements — additional TX bursts
on a power rail that just handled the association + DHCP sequence.

**Fix**: Add a small delay (200–500 ms) between WiFi connection success and
mDNS start, or defer mDNS start to the first main loop iteration. This lets
the power rail recover from the WiFi association burst before the mDNS
announcement adds another TX spike.

### Recommended boot sequence (revised)

```
Line 451  sdManager.releaseControl()
          // CPU stays at 80 MHz — no PLL relock spike
Line 464  wifiManager.setupEventHandlers()
Line 467  wifiManager.applyTxPowerEarly()    — RF calibration (isolated spike)
Line 470  wifiManager.connectStation()       — scan + association + DHCP
          // — small stabilisation delay (200ms) —
Line 493  wifiManager.startMDNS()            — mDNS (after rail recovery)
Line 500  esp_pm_configure()                 — DFS setup
Line NEW  setCpuFrequencyMhz(targetCpuMhz)  — CPU boost (after WiFi stable)
```

This ensures:
- RF calibration spike is not compounded by a PLL relock
- mDNS does not fire immediately on top of the DHCP burst
- CPU frequency increase happens after WiFi is fully stable

---

## 3. Implementation Phases

## Phase 1: Safety — Remove ULP From SD Takeover Path

**Priority**: Highest (safety-critical)

### Rationale

The ULP monitor reconfigures GPIO 33 as RTC GPIO, blinding the hardware PCNT
detector that the FSM relies on for bus silence decisions. This causes false
SD card takeover during active therapy.

### Changes

| File | Change |
| :--- | :--- |
| `src/main.cpp` | Remove `ulpMonitor.begin()` (line 302), `ulpMonitor.stop()` calls, `ulpMonitor.begin()` restart in `handleReleasing()`, global declaration, and `#include "UlpMonitor.h"` |
| `src/UlpMonitor.cpp` | Delete file |
| `include/UlpMonitor.h` | Delete file |

### Verification

- Build succeeds for all targets
- PCNT-based `TrafficMonitor` remains the sole bus silence detector

---

## Phase 2: Build Targets — Remove Non-OTA

| File | Change |
| :--- | :--- |
| `platformio.ini` | Remove entire `[env:pico32]` section. Change `default_envs` from `pico32, pico32-ota` to `pico32-ota`. Keep `[env:native]` for tests. |
| `sdkconfig.defaults` | Change `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11` to `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10` (the absolute Kconfig minimum). |

No separate low-power build variant.

### Verification

- `pio run -e pico32-ota` succeeds
- `pio run -e pico32` fails or is absent (removed)
- `pio test -e native` still works

---

## Phase 3: WiFi TX Power Redesign

### Part A: Update the WifiTxPower enum

| File | Change |
| :--- | :--- |
| `include/Config.h` | Replace 4-value enum with 5-value enum: `POWER_LOWEST` (-1 dBm), `POWER_LOW` (2 dBm), `POWER_MID` (5 dBm), `POWER_HIGH` (8.5 dBm), `POWER_MAX` (10 dBm) |

### Part B: Update the parser (no aliases)

| File | Change |
| :--- | :--- |
| `src/Config.cpp` | `parseWifiTxPower()`: Accept exactly `LOWEST`, `LOW`, `MID`, `HIGH`, `MAX`. Remove `MAXIMUM` and `MEDIUM` aliases. Default/fallback → `POWER_MID` (5 dBm). |
| `src/Config.cpp` | Constructor: change default from `POWER_LOW` to `POWER_MID` (both are 5 dBm under the new naming — this is a name change only, same dBm value) |

### Part C: Update WiFiManager mapping

| File | Change |
| :--- | :--- |
| `src/WiFiManager.cpp` | `applyTxPowerEarly()`: Map new enum values to platform constants: `POWER_LOWEST`→`WIFI_POWER_MINUS_1dBm`, `POWER_LOW`→`WIFI_POWER_2dBm`, `POWER_MID`→`WIFI_POWER_5dBm`, `POWER_HIGH`→`WIFI_POWER_8_5dBm`, `POWER_MAX`→`WIFI_POWER_11dBm` (closest platform constant to 10 dBm; capped at 10 by PHY). Remove `WIFI_POWER_19_5dBm`. Update default case to `WIFI_POWER_5dBm`. |
| `src/WiFiManager.cpp` | `applyPowerSettings()`: Same mapping update. |

### Part D: Update brownout-recovery TX power

| File | Change |
| :--- | :--- |
| `src/main.cpp` | Line 490: Change `WifiTxPower::POWER_LOW` to `WifiTxPower::POWER_LOW` (still 2 dBm under new naming — verify this is the desired brownout-recovery level) |

### New config surface

| `config.txt` Value | dBm | Platform Enum | Use Case |
| :--- | :--- | :--- | :--- |
| `LOWEST` | -1 | `WIFI_POWER_MINUS_1dBm` | Router on same nightstand |
| `LOW` | 2 | `WIFI_POWER_2dBm` | Router within 1–2 metres |
| **`MID`** | **5** | `WIFI_POWER_5dBm` | **Default.** Typical bedroom (3–5m) |
| `HIGH` | 8.5 | `WIFI_POWER_8_5dBm` | Router in adjacent room |
| `MAX` | 10 | `WIFI_POWER_11dBm` | Through walls, last resort. Platform maps to 11 dBm constant but PHY caps at 10. |

### Defaults

- **Constructor default**: `POWER_MID` (5 dBm)
- **Parser fallback for unrecognised values**: `POWER_MID` (5 dBm)

Both aligned. No more inconsistency.

### Verification

- Build succeeds
- Unit test `test_config` updated to reflect new levels
- Setting `WIFI_TX_PWR=LOWEST` in config.txt → runtime TX at -1 dBm
- Setting `WIFI_TX_PWR=MAX` → runtime TX at 10 dBm (PHY-capped)
- Invalid/missing `WIFI_TX_PWR` → runtime TX at 5 dBm

---

## Phase 4: Boot Sequence Power Spike Reduction

### Change A: Delay CPU frequency boost

| File | Change |
| :--- | :--- |
| `src/main.cpp` | Move `setCpuFrequencyMhz(targetCpuMhz)` from line 457 (before WiFi init) to after `esp_pm_configure()` (after line 526). Keep the `LOGF("CPU frequency: %dMHz", ...)` log near the new location. |

This ensures the CPU stays at the boot default of 80 MHz during WiFi RF
calibration and association. The PLL relock spike does not compound with
the WiFi spike.

### Change B: Add stabilisation delay before mDNS

| File | Change |
| :--- | :--- |
| `src/main.cpp` | After WiFi connects successfully (after line 484) and before mDNS start (line 493), add a 200 ms delay: `delay(200);` with a brief log comment. |

This allows the 3.3V rail to recover from the WiFi association + DHCP burst
before mDNS fires its multicast announcement.

### Verification

- Boot logs show CPU at 80 MHz during WiFi init, then frequency change after
- mDNS still starts and works (just 200ms later)
- No functional regression

---

## Phase 5: SSE Architecture Fix

### Change A: Keep SSE alive across tab switches

| File | Change |
| :--- | :--- |
| `include/web_ui.h` | In `tab()` function (line 366): Remove `stopSse()` from the `else` branch. When returning to Logs tab, if SSE is not connected, call `startSse()` instead of `startLogPoll()`. |

### Change B: Fix SSE reconnect strategy

| File | Change |
| :--- | :--- |
| `include/web_ui.h` | In `sseSource.onerror` handler: Replace immediate `fetchBackfill()` with: attempt SSE reconnect (2–3 retries with backoff). If SSE cannot reconnect, fall back to `startLogPoll()`. Reserve `fetchBackfill()` only for first Logs tab open or confirmed reboot. |

### Change C: Add server-side SSE keepalive

| File | Change |
| :--- | :--- |
| `src/CpapWebServer.cpp` | In `pushSseLogs()`: When there are no new log bytes, periodically send an SSE comment line (`: keepalive\n\n`) every ~15 seconds. This prevents browser/proxy idle timeouts. |

### Change D: Remove SSE suppression in brownout-recovery mode

| File | Change |
| :--- | :--- |
| `src/main.cpp` | Remove or relax the `g_brownoutRecoveryBoot` gate on `pushSseLogs()`. SSE is the lightest log transport — suppressing it forces heavier alternatives. If throttling is needed, reduce push frequency rather than disabling entirely. |

### Verification

- SSE stays connected when switching from Logs to Dashboard and back
- SSE reconnects automatically after brief disconnects
- No `/api/logs/full` requests on normal SSE drops
- Keepalive comments visible in browser DevTools Network tab

---

## Phase 6: REBOOTING UI Fix

### Change A: Fix stale badge rendering

| File | Change |
| :--- | :--- |
| `include/web_ui.h` | In `renderStatus()`: After hiding the reboot overlay and resetting `statusFailCount`, **always** redraw the state badge regardless of whether `_newSt === currentFsmState`. This ensures the badge is restored from the stale `REBOOTING` text even when the real state hasn't changed. |

### Change B: Increase failure threshold

| File | Change |
| :--- | :--- |
| `include/web_ui.h` | In `pollStatus()` catch handler: Change threshold from `statusFailCount >= 2` to `statusFailCount >= 5` (15 seconds instead of 6). |

### Change C: Add visibility change handler

| File | Change |
| :--- | :--- |
| `include/web_ui.h` | Add a `document.addEventListener('visibilitychange', ...)` handler that resets `statusFailCount` to 0 when the tab becomes visible. This gives a fresh grace period after mobile tab resume. |

### Change D: Add intermediate "Offline" state

| File | Change |
| :--- | :--- |
| `include/web_ui.h` | When `statusFailCount` is 1–4: show `Offline — reconnecting…` badge. When `statusFailCount >= 5` or `rebootExpected`: show `REBOOTING` overlay. |

### Verification

- Mobile: switch away and back → no false REBOOTING
- Simulate 2 failed polls → shows "Offline — reconnecting"
- Simulate 5+ failed polls → shows REBOOTING overlay
- Real reboot → REBOOTING shown immediately (via `rebootExpected`)
- After recovery → badge shows correct state, not stale REBOOTING

---

## Phase 7: Scheduled-Mode Force Upload

### Change A: Accept Force Upload outside window

| File | Change |
| :--- | :--- |
| `src/CpapWebServer.cpp` | In `handleTriggerUpload()`: Remove the rejection block for scheduled-mode outside-window requests. Instead, set a flag (e.g., `g_forceRecentOnlyFlag = true`) and accept the request. |

### Change B: Support recent-only uploads

| File | Change |
| :--- | :--- |
| `src/ScheduleManager.cpp` | Add a method or flag that allows `canUploadFreshData()` to return `true` outside the window when the force-recent flag is set. `canUploadOldData()` remains gated by `isInUploadWindow()`. |
| `src/main.cpp` | In `handleUploading()` or the trigger path: consume `g_forceRecentOnlyFlag`, pass it to the scheduler so it knows to allow fresh-only outside the window. Clear the flag after use. |

### Change C: Helper text (no change needed)

The existing helper text already says:

> Force Upload (not recommended) → forces an upload of recent data now.

This is correct. The firmware behaviour will now match the text.

### Verification

- In Scheduled mode, outside window: pressing Force Upload triggers a session
- Only recent data (per `RECENT_FOLDER_DAYS`) is uploaded
- Old data is skipped
- Inside window: no change to existing behaviour

---

## Phase 8: Brownout Policy

### Change A: Revert default threshold to Level 0

| File | Change |
| :--- | :--- |
| `sdkconfig.defaults` | Remove `CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_7=y`. Optionally add explicit `CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_0=y` for clarity. Update the surrounding comment. |

### Change B: Implement `BROWNOUT_DETECT=OFF` config key

| File | Change |
| :--- | :--- |
| `include/Config.h` | Add `bool brownoutDetectOff` member (default `false`), getter `isBrownoutDetectOff()`. |
| `src/Config.cpp` | Parse `BROWNOUT_DETECT` key. If value is `OFF` (case-insensitive), set `brownoutDetectOff = true`. Any other value or absent key → `false` (brownout detection stays enabled). |
| `src/main.cpp` | After `config.loadFromSD()` succeeds, check `config.isBrownoutDetectOff()`. If true, disable brownout detection by writing to RTC_CNTL_BROWN_OUT_REG (clear enable bit). Log a prominent warning. |
| `include/web_ui.h` | Add a persistent warning banner on the Dashboard tab when brownout detection is disabled. The banner should be visually distinct (e.g., orange/yellow background) and say: "Brownout detection is disabled. The device will not reset on power drops — this may cause data corruption." |
| `src/CpapWebServer.cpp` | Expose `brownout_detect_off` in the status JSON so the web UI can render the banner. |

### Implementation detail: disabling brownout at runtime

```cpp
#include "soc/rtc_cntl_reg.h"

if (config.isBrownoutDetectOff()) {
    LOG_WARN("[POWER] BROWNOUT_DETECT=OFF — disabling brownout detection per config");
    LOG_WARN("[POWER] WARNING: Device will NOT reset on power drops. Risk of data corruption.");
    CLEAR_PERI_REG_MASK(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
}
```

This must happen **after** config is loaded from SD but **before** WiFi init
(which is the highest-current phase). The brownout detector is disabled by
clearing the enable bit in the RTC control register. This is a runtime
operation — the compile-time brownout threshold (Level 0) remains as the
default for normal operation.

### Verification

- Build produces firmware with Level 0 brownout threshold
- Devices that were resetting frequently at Level 7 show reduced reset rate
- `BROWNOUT_DETECT=OFF` in config.txt → log warning + brownout disabled
- Dashboard shows persistent warning banner when brownout is disabled
- Absent or non-OFF value → brownout detection remains enabled (no banner)

---

## Phase 9: Alias Removal (No Backward Compatibility)

**Policy**: Backward compatibility is NOT required. Remove all aliases.

### Config key aliases to remove

| File | Current Alias | Primary Key | Action |
| :--- | :--- | :--- | :--- |
| `src/Config.cpp` | `GMT_OFFSET` | `GMT_OFFSET_HOURS` | Remove `GMT_OFFSET` from the `||` condition |
| `src/Config.cpp` | `SAVE_LOGS` | `PERSISTENT_LOGS` | Remove `SAVE_LOGS` from the `||` condition |
| `src/Config.cpp` | `LOG_TO_SD_CARD` | `PERSISTENT_LOGS` | Remove `LOG_TO_SD_CARD` from the `||` condition |
| `src/Config.cpp` | `SKIP_REBOOT_BETWEEN_BACKENDS` | `MINIMIZE_REBOOTS` | Remove `SKIP_REBOOT_BETWEEN_BACKENDS` from the `||` condition |

### Parser value aliases to remove

| File | Function | Alias | Action |
| :--- | :--- | :--- | :--- |
| `src/Config.cpp` | `parseWifiTxPower()` | `MAXIMUM` | Remove (only accept `MAX`) |
| `src/Config.cpp` | `parseWifiTxPower()` | `MEDIUM` | Remove (only accept `MID`) |
| `src/Config.cpp` | `parseWifiPowerSaving()` | `MEDIUM` | Remove (only accept `MID`) |
| `src/Config.cpp` | `parseWifiPowerSaving()` | `OFF` | Remove (only accept `NONE`) |
| `src/Config.cpp` | `parseWifiPowerSaving()` | `HIGH` | Remove (only accept `MAX`) |

**Note**: `MODEM` as an alias for `MID` in `parseWifiPowerSaving()` — this is
documented in CONFIG_REFERENCE.md and release/README.md. Per strict no-alias
policy, it should be removed too. Users should use `MID` or `MAX`.

### Verification

- Build succeeds
- Unit tests updated to remove alias test cases
- Config files using old aliases produce `WARN: Unknown config key` logs
- Config files using old value aliases produce fallback-default behaviour

---

## Phase 10: Documentation Sweep

### Files with `WIFI_TX_POWER` (wrong key — must be corrected to `WIFI_TX_PWR`)

| File | Line(s) | Issue |
| :--- | :--- | :--- |
| `docs/25-REGRESSION-ANALYSIS-CO46.md` | 258, 262 | Uses `WIFI_TX_POWER` |

**Note**: Analysis documents (`docs/23`–`docs/29`) are historical records.
Only `docs/25` contains the wrong key in a **recommendation context** that
could be mistaken for guidance. All other references in analysis docs are in
the context of "this is wrong" corrections. Recommend correcting `docs/25`
only; leave others as historical.

### Files with outdated WiFi TX power levels

| File | Issue | Required Update |
| :--- | :--- | :--- |
| `docs/CONFIG_REFERENCE.md` | Line 76: shows `LOW`/`MID`/`HIGH`/`MAX` with old dBm values (5/8.5/11/19.5) | Update to 5-level scheme: `LOWEST`(-1)/`LOW`(2)/`MID`(5)/`HIGH`(8.5)/`MAX`(11). Update default description. Remove alias mentions from Removed/Legacy section. |
| `release/README.md` | Lines 353–358: shows `MAX = 19.5 dBm` | Update to 5-level scheme. Remove `MAX = 19.5 dBm`. Update `SAVE_LOGS` reference (line 379) to `PERSISTENT_LOGS`. |
| `docs/DEVELOPMENT.md` | Lines 48, 184: mentions old levels | Update to 5-level scheme. |
| `docs/specs/configuration-management.md` | Line 63: old levels, wrong default ("mid" = 8.5 dBm) | Update to 5-level scheme, correct default to `MID` = 5 dBm. |
| `docs/specs/main-application.md` | Line 101: "TX power default 8.5 dBm" | Change to "TX power default 5 dBm". |
| `docs/specs/wifi-network-management.md` | Line 59: mentions PHY cap | Update if PHY cap value changes. |
| `docs/07-POWER-REDUCTION-SUMMARY.md` | Multiple references | Update TX power levels and defaults. |

### Documentation for removed aliases

| File | Issue | Required Update |
| :--- | :--- | :--- |
| `docs/CONFIG_REFERENCE.md` | Lines 52, 65, 103: mention backward-compatible aliases | Remove all alias mentions. Add removed aliases to the "Removed / Legacy Keys" table. |
| `release/README.md` | Line 379: uses `SAVE_LOGS` instead of `PERSISTENT_LOGS` | Change to `PERSISTENT_LOGS`. |

### Documentation for low-power build (if created)

| File | Required Addition |
| :--- | :--- |
| `release/README.md` | Add a "Build Variants" section explaining OTA vs low-power OTA. |
| `docs/DEVELOPMENT.md` | Add build instructions for low-power variant. |

### Documentation for removed non-OTA build

| File | Required Update |
| :--- | :--- |
| `release/README.md` | Remove any references to non-OTA flashing workflow. |
| `docs/DEVELOPMENT.md` | Update build instructions to reflect OTA-only. |

---

## 4. Files Changed Per Phase

| Phase | Files Modified | Files Deleted | Files Created |
| :--- | :--- | :--- | :--- |
| 1 | `src/main.cpp` | `src/UlpMonitor.cpp`, `include/UlpMonitor.h` | — |
| 2 | `platformio.ini`, `sdkconfig.defaults` | — | — |
| 3 | `include/Config.h`, `src/Config.cpp`, `src/WiFiManager.cpp`, `src/main.cpp` | — | — |
| 4 | `src/main.cpp` | — | — |
| 5 | `include/web_ui.h`, `src/CpapWebServer.cpp`, `src/main.cpp` | — | — |
| 6 | `include/web_ui.h` | — | — |
| 7 | `src/CpapWebServer.cpp`, `src/ScheduleManager.cpp`, `src/main.cpp` | — | — |
| 8 | `sdkconfig.defaults`, `include/Config.h`, `src/Config.cpp`, `src/main.cpp`, `include/web_ui.h`, `src/CpapWebServer.cpp` | — | — |
| 9 | `src/Config.cpp` | — | — |
| 10 | `docs/CONFIG_REFERENCE.md`, `release/README.md`, `docs/DEVELOPMENT.md`, `docs/specs/configuration-management.md`, `docs/specs/main-application.md`, `docs/07-POWER-REDUCTION-SUMMARY.md`, `docs/25-REGRESSION-ANALYSIS-CO46.md` | — | — |

---

## 5. Verification Checklist

### Build verification

- [ ] `pio run -e pico32-ota` — builds cleanly
- [ ] `pio test -e native` — all tests pass
- [ ] `pio run -e pico32` — fails or absent (removed)

### Functional verification

- [ ] PCNT-based bus silence detection works (ULP removed)
- [ ] WiFi connects at configured TX power
- [ ] `WIFI_TX_PWR=LOWEST` → -1 dBm effective
- [ ] `WIFI_TX_PWR=MAX` → 10 dBm effective (PHY-capped)
- [ ] Missing/invalid `WIFI_TX_PWR` → 5 dBm default
- [ ] SSE stays alive across Dashboard↔Logs tab switches
- [ ] SSE reconnects after brief drop without hitting `/api/logs/full`
- [ ] SSE keepalive prevents idle timeouts
- [ ] Mobile tab resume: no false REBOOTING
- [ ] REBOOTING badge clears correctly after recovery
- [ ] Scheduled mode, outside window: Force Upload works for recent data only
- [ ] Old config aliases produce `WARN: Unknown config key` log
- [ ] CPU frequency stays at 80 MHz during WiFi init (log verification)
- [ ] mDNS starts 200ms after WiFi connect (log verification)

### Documentation verification

- [ ] No remaining references to `WIFI_TX_POWER` (wrong key) in user-facing docs
- [ ] No remaining references to 19.5 dBm or 11 dBm as a valid MAX option
- [ ] All alias mentions removed from CONFIG_REFERENCE.md
- [ ] All default values in docs match code
- [ ] `BROWNOUT_DETECT=OFF` disables brownout, shows dashboard warning
- [ ] `BROWNOUT_DETECT` absent or non-OFF → brownout stays enabled
