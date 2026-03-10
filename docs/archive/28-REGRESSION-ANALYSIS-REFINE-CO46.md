# Refinement of Regression Findings — CO46

This document validates the claims in `docs/26-REGRESSION-ANALYSIS-REFINE-C54.md`
and `docs/27-REGRESSION-ANALYSIS-REFINE-G31.md`, addresses the WiFi RF calibration
spike question in detail, proposes a simplified WiFi TX power naming scheme, and
specifies the new Force Upload behaviour for Scheduled Mode.

This is **analysis and specification only**. No code changes are made here.

---

## 1. Validation of `docs/26-REGRESSION-ANALYSIS-REFINE-C54.md`

All core claims in `docs/26` have been re-checked against the current codebase
and **remain valid**:

| Claim | Status | Evidence |
| :--- | :--- | :--- |
| Config key is `WIFI_TX_PWR` (not `WIFI_TX_POWER`) | **Correct** | `src/Config.cpp` line `key == "WIFI_TX_PWR"` |
| Constructor default is `POWER_LOW` / 5 dBm | **Correct** | `src/Config.cpp` constructor: `wifiTxPower(WifiTxPower::POWER_LOW)` |
| Parser fallback for invalid values is `POWER_MID` / 8.5 dBm | **Correct** | `src/Config.cpp` `parseWifiTxPower()` returns `WifiTxPower::POWER_MID` |
| Platform supports 2 dBm and -1 dBm | **Correct** | `WiFiGeneric.h` defines `WIFI_POWER_2dBm` and `WIFI_POWER_MINUS_1dBm` |
| Build caps PHY max at 11 dBm | **Correct** | `sdkconfig.defaults`: `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11` |
| `REBOOTING` badge stuck is a stale-state rendering bug | **Correct** | `web_ui.h`: `renderStatus()` only redraws when `_newSt !== currentFsmState` |
| Force Upload rejected outside window in Scheduled mode | **Correct** | `src/CpapWebServer.cpp` `handleTriggerUpload()` returns rejection JSON |
| Brownout-disable needs GUI warning, not just logs | **Correct** | Requirement / recommendation (no code claim) |

**No corrections needed for `docs/26`.**

---

## 2. Validation of `docs/27-REGRESSION-ANALYSIS-REFINE-G31.md`

### 2.1 What is correct

- Dropping 19.5 dBm mode — rationale is sound: the build already caps at 11 dBm,
  making the `MAX` → `WIFI_POWER_19_5dBm` mapping misleading and non-functional.
- RF calibration spike explanation — the general mechanism is correct.
- Force Upload requirement — valid specification.

### 2.2 What needs correction

**The WiFi init spike explanation contains an imprecise claim.**

`docs/27` states:

> the runtime application code hasn't had a chance to call `WiFi.setTxPower()`

This implies `config.txt` is read too late. **That is not the real problem.**

The actual boot sequence in `src/main.cpp` is:

1. **Line 408** — `config.loadFromSD()` reads `config.txt` from SD card
2. **Line 451** — SD card released back to CPAP
3. **Line 467** — `wifiManager.applyTxPowerEarly(config.getWifiTxPower())`

Inside `applyTxPowerEarly()` (`src/WiFiManager.cpp:333-349`):

```
WiFi.mode(WIFI_STA);           ← triggers esp_wifi_start() → RF calibration
WiFi.setTxPower(espTxPower);   ← only affects SUBSEQUENT transmissions
WiFi.disconnect(true);
```

So **`config.txt` IS already read before WiFi init**. The config value is
available. The real blocker is:

- `WiFi.mode(WIFI_STA)` internally calls `esp_wifi_start()`, which triggers
  RF calibration
- RF calibration uses the compile-time `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` value
- `WiFi.setTxPower()` is called after `WiFi.mode()`, so it only limits
  subsequent transmissions, not the calibration burst
- There is **no ESP-IDF API** to inject a runtime TX power cap before
  `esp_wifi_start()`

This is an important distinction: the problem is not timing of config reads,
it is the absence of a runtime API to cap calibration power.

**The naming table in `docs/27` has too many levels (8).** This is addressed
in Section 4 below.

---

## 3. WiFi RF Calibration Spike — Deep Analysis

### 3.1 The question

> Regardless of the set mode (e.g., 2 dBm), during WiFi initialisation, can
> the power still spike to `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11`? Can this be
> avoided without a separate compiled firmware?

### 3.2 Answer

**Yes, it can spike. No, it cannot be avoided without recompiling.**

### 3.3 Detailed reasoning

The ESP32 WiFi initialisation sequence is:

```
WiFi.mode(WIFI_STA)
  └→ esp_wifi_init(&cfg)       — allocates WiFi driver resources
  └→ esp_wifi_set_mode(STA)    — sets station mode
  └→ esp_wifi_start()          — starts WiFi stack
       └→ esp_phy_init()       — RF calibration happens HERE
            └→ reads compiled PHY init data
            └→ calibrates at CONFIG_ESP_PHY_MAX_WIFI_TX_POWER
```

The RF calibration power ceiling is baked into the PHY init data blob at
**compile time** from `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER`. There is no public
ESP-IDF API to modify this value at runtime before `esp_wifi_start()`.

Specifically:

- `esp_wifi_set_max_tx_power()` — requires WiFi to be **already started**
  (returns `ESP_ERR_WIFI_NOT_STARTED` otherwise)
- `wifi_init_config_t` — has **no** max TX power field
- `esp_phy_init_data_t` — internal structure, not a public API; modifying the
  PHY partition at runtime would be fragile and unsupported

### 3.4 Could we split the init to inject the config value?

Hypothetically, one could bypass `WiFi.mode()` and call the ESP-IDF functions
individually:

```
esp_wifi_init(&cfg);
esp_wifi_set_mode(WIFI_MODE_STA);
// ← want to set max TX power HERE
esp_wifi_start();  // ← RF calibration happens here
```

But `esp_wifi_set_max_tx_power()` explicitly requires `esp_wifi_start()` to
have already completed. There is no intermediate injection point.

### 3.5 Conclusion

**`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` is the only mechanism to cap the RF
calibration spike.** A different compiled value requires a different firmware
build.

---

## 4. Separate Build Target for Low-Power Devices

### 4.1 Feasibility

PlatformIO fully supports multiple build environments sharing the same source
code but with different `sdkconfig` files. A low-power build would only need
a different `sdkconfig.defaults` with a lower PHY cap.

Example addition to `platformio.ini`:

```ini
[env:pico32-lowpower]
extends = env:pico32
board_build.sdkconfig = sdkconfig.lowpower.defaults
```

With `sdkconfig.lowpower.defaults` containing:

```ini
# Inherits all other settings from sdkconfig.defaults but with a lower PHY cap.
# For devices on power-constrained CPAP machines.
CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=5
```

All other settings (brownout threshold, lwIP tuning, TLS ciphers, etc.) would
be inherited from the base `sdkconfig.defaults`.

### 4.2 Current draw savings estimate

From ESP32 datasheet typical TX current values (802.11g/n OFDM):

| PHY Cap | Approximate Peak TX Current | Notes |
| :--- | :--- | :--- |
| 11 dBm | ~190–220 mA | Current standard build |
| 8.5 dBm | ~175–200 mA | ~15–20 mA savings |
| 5 dBm | ~160–180 mA | **~30–50 mA savings** vs 11 dBm |
| 2 dBm | ~150–170 mA | ~40–60 mA savings vs 11 dBm |

These are **transient** spikes lasting only milliseconds during RF calibration,
but on a marginally-powered device, even brief 30–50 mA spikes can be the
difference between a clean boot and a brownout reset.

### 4.3 Recommendation: PHY cap of 5 for the low-power build

A `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=5` cap for the low-power variant is a
reasonable choice:

- Saves ~30–50 mA during the calibration spike compared to the current 11 dBm cap
- Still allows the device to operate at `MID` (5 dBm) for typical bedroom distances
- Users who need `HIGH` (8.5 dBm) or `MAX` (11 dBm) from `config.txt` would need
  the standard build — which is the correct trade-off (if your power supply can't
  handle 11 dBm calibration, you shouldn't be running at 11 dBm steady-state either)

### 4.4 The ideal vs reality

The ideal solution would be to honour `WIFI_TX_PWR` from `config.txt` as both
the runtime TX limit **and** the RF calibration cap. This would eliminate the
need for a separate build entirely.

Unfortunately, this is **not possible** with the current ESP-IDF/Arduino
framework. The PHY calibration power is a compile-time constant with no runtime
override API. This is a hardware abstraction limitation, not a firmware design
choice.

If Espressif were to add a `esp_phy_set_max_tx_power()` or similar API callable
before `esp_wifi_start()`, this could be revisited. As of the current ESP-IDF
v4.4 / Arduino-ESP32 framework, no such API exists.

---

## 5. Simplified WiFi TX Power Naming

### 5.1 Rationale for simplification

The 8-level scheme proposed in `docs/27` is excessive for this use case. The
device is a bedroom appliance, not a long-range radio. Users should not need
to understand fine-grained dBm values.

The range should be capped at **11 dBm** (matching the compile-time PHY cap in
the standard build), and only practically useful levels should be exposed.

### 5.2 Proposed scheme (5 levels)

| `config.txt` Value | dBm | Platform Enum | Use Case |
| :--- | :--- | :--- | :--- |
| `LOWEST` | -1 | `WIFI_POWER_MINUS_1dBm` | Router on the same nightstand |
| `LOW` | 2 | `WIFI_POWER_2dBm` | Router within 1–2 metres |
| `MID` | 5 | `WIFI_POWER_5dBm` | **Default.** Typical bedroom (3–5m) |
| `HIGH` | 8.5 | `WIFI_POWER_8_5dBm` | Router in adjacent room |
| `MAX` | 11 | `WIFI_POWER_11dBm` | Through walls, last resort before extender |

### 5.3 Defaults (both aligned to `MID` / 5 dBm)

- **Constructor default**: `MID` (5 dBm) — used when `WIFI_TX_PWR` is absent
  from `config.txt`
- **Parser fallback**: `MID` (5 dBm) — used when `WIFI_TX_PWR` is present but
  the value is unrecognised

This fixes the current inconsistency where the constructor defaults to `LOW`
(5 dBm) but the parser fallback defaults to `MID` (8.5 dBm). Under the new
naming, both point to the same 5 dBm level, now called `MID`.

### 5.4 Why `LOWEST` (-1 dBm) is worth keeping

At -1 dBm, effective range is approximately 1–3 metres line-of-sight. This is
extremely short, but valid for users who:

- Place a WiFi extender/repeater on the same nightstand
- Have the router literally next to the bed
- Are on the most power-constrained CPAP machines

It provides the absolute minimum RF power draw and serves as the
ultra-low-power option without needing a separate build.

### 5.5 Why 7 dBm, 13 dBm, 15 dBm, 17 dBm, 18.5 dBm, and 19.5 dBm are dropped

- **7 dBm**: Too close to 5 and 8.5 to justify a separate named level
- **13, 15, 17, 18.5 dBm**: All above the 11 dBm compile-time cap, making them
  non-functional in the standard build and misleading
- **19.5 dBm**: Already non-functional, draws dangerous current, no valid use case

### 5.6 Backward compatibility

The parser should continue to accept the old names for backward compatibility:

| Old Value | Maps To |
| :--- | :--- |
| `LOW` (old, was 5 dBm) | `MID` (new, still 5 dBm) — **same dBm, name shift only** |
| `MID` (old, was 8.5 dBm) | `HIGH` (new, still 8.5 dBm) — **same dBm, name shift only** |
| `HIGH` (old, was 11 dBm) | `MAX` (new, still 11 dBm) — **same dBm, name shift only** |
| `MAX` / `MAXIMUM` (old, was 19.5 dBm) | `MAX` (new, capped at 11 dBm) — **dBm reduced** |

This ensures existing `config.txt` files continue to work. Only users who had
`MAX` will see a behavioural change (from a non-functional 19.5 dBm request to
an actual 11 dBm cap), which is a strictly positive correction.

---

## 6. Force Upload in Scheduled Mode — Revised Specification

### 6.1 Context

The helper text in the dashboard currently says:

> Force Upload (not recommended) → forces an upload of recent data now.

The server (`src/CpapWebServer.cpp`) rejects this request outside the upload
window. The scheduler (`src/ScheduleManager.cpp`) also disallows it.

**The helper text is correct in its intent. The firmware behaviour is wrong.**

### 6.2 New requirement

When the device is in **Scheduled Mode** and the user presses `Force Upload`:

#### Outside the upload window

1. The `/trigger-upload` endpoint **accepts** the request (does **not** reject
   it or advise switching to Smart mode).
2. The upload session processes **only recent data** based on `RECENT_FOLDER_DAYS`.
3. Old/historical data is **not** uploaded — that remains gated by the upload
   window.
4. The FSM proceeds through the normal `ACQUIRING → UPLOADING → RELEASING`
   cycle.

#### Inside the upload window

No change to current behaviour. Both recent and old data are eligible, same as
today.

### 6.3 Implementation scope (for future reference)

Two code paths need modification:

1. **`src/CpapWebServer.cpp` → `handleTriggerUpload()`**: Remove the rejection
   block for scheduled-mode outside-window requests. Instead, set a flag
   indicating "force recent only".

2. **`src/ScheduleManager.cpp` or `src/main.cpp` → upload eligibility**: When
   the force-recent flag is set, `canUploadFreshData()` should return `true`
   regardless of window, while `canUploadOldData()` remains gated by
   `isInUploadWindow()`.

### 6.4 Helper text

The existing helper text is correct and does **not** need modification:

> Force Upload (not recommended) → forces an upload of recent data now.

This accurately describes the intended behaviour once the firmware is updated.

---

## 7. Summary of Recommendations

| Item | Recommendation |
| :--- | :--- |
| **WiFi TX naming** | 5 levels: `LOWEST`(-1), `LOW`(2), `MID`(5), `HIGH`(8.5), `MAX`(11) |
| **Default & fallback** | Both → `MID` / 5 dBm |
| **Drop 19.5 dBm** | Yes — non-functional, dangerous, misleading |
| **RF calibration spike** | Cannot be config-driven; compile-time `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER` is the only cap |
| **Low-power build** | Add `[env:pico32-lowpower]` with `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=5`, saving ~30–50 mA during cal spike |
| **Force Upload (Scheduled)** | Allow outside-window, recent-data-only uploads; keep existing helper text |
| **Backward compat** | Old `LOW`/`MID`/`HIGH` map to same dBm under new names; old `MAX` reduced to 11 dBm |
