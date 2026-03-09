# Refinement of Regression Findings — G31

This document addresses the latest configuration and behavioural requirements regarding WiFi transmit power and Scheduled Mode "Force Upload" functionality. 

This is **analysis and specification only**. No code changes are made here.

---

## 1. WiFi TX Power Configuration

### 1.1 Exposing All Power Levels
The underlying ESP32 platform (`WiFiGeneric.h`) supports a wide range of power levels. To make them all available via `config.txt` while dropping the 19.5 dBm mode (discussed below), the configuration parser should be updated to accept the following mapped values:

| `config.txt` Value | Mapped Enum | Transmit Power |
| :--- | :--- | :--- |
| `MINIMAL` | `WIFI_POWER_MINUS_1dBm` | -1 dBm |
| `LOWEST` | `WIFI_POWER_2dBm` | 2 dBm |
| `LOW` | `WIFI_POWER_5dBm` | 5 dBm |
| `MID_LOW` | `WIFI_POWER_7dBm` | 7 dBm |
| `MID` | `WIFI_POWER_8_5dBm` | 8.5 dBm |
| `HIGH` | `WIFI_POWER_11dBm` | 11 dBm |
| `VERY_HIGH` | `WIFI_POWER_15dBm` | 15 dBm |
| `MAX` | `WIFI_POWER_18_5dBm` | 18.5 dBm |

*(Note: intermediate values like 13 dBm and 17 dBm exist in the framework, but the above provides a comprehensive, named gradient).*

**Default & Fallback Alignment:**
The requirement to align the defaults is noted. Both the initial system constructor and the "parser failed" fallback must point to `LOW` (5 dBm). This ensures that an invalid entry in `config.txt` does not accidentally boost power to an unexpected level, maintaining a safe, low-power baseline.

### 1.2 Dropping 19.5 dBm Mode
**Opinion:** I strongly agree with dropping the 19.5 dBm (`WIFI_POWER_19_5dBm`) mode entirely. 

**Rationale:**
1. **Current Spikes & Stability:** At 19.5 dBm, the ESP32 radio can draw transient currents exceeding 350–400 mA. This is the primary catalyst for voltage sags and brownout resets, especially on a device heavily reliant on SD card operations.
2. **Use-Case Suitability:** A CPAP data uploader is an indoor device typically residing in a bedroom, relatively close to a home router. Maximum RF output is unnecessary and introduces massive stability risks for negligible gain.
3. **Current Reality:** Since the codebase already caps the PHY level to 11 dBm at compile time (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11`), the 19.5 dBm mode is already non-functional. Dropping it formally aligns the configuration options with reality and enforces a safer hardware power envelope.

### 1.3 The WiFi Initialisation Power Spike
**Question:** *Regardless of the set mode (e.g., 2 dBm), during WiFi initialisation, can the power still spike to the compile-time limit of 11 dBm? Can this be avoided dynamically?*

**Answer:** **Yes, it can still spike, and No, it cannot be fully avoided dynamically.**

**Why this happens:**
When the ESP32 first turns on its WiFi baseband (e.g., calling `WiFi.mode(WIFI_STA)`), it performs mandatory RF calibration and an initial environmental scan. During this extremely early hardware phase, the runtime application code hasn't had a chance to call `WiFi.setTxPower()`. 

Because of this, the ESP32 PHY layer falls back to the maximum allowed limit compiled into its baseband configuration—which is currently defined by `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11` in `sdkconfig.defaults`.

Even if your `config.txt` says `LOWEST` (2 dBm), the device must:
1. Boot up.
2. Initialise the file system.
3. Read `config.txt`.
4. Turn on WiFi (this triggers the calibration spike up to the compile-time limit).
5. Apply the 2 dBm limit to subsequent transmissions.

**Can we honour the config dynamically to avoid the spike?**
Unfortunately, no. The only way to stop the initial calibration from reaching 11 dBm is to lower the compile-time cap (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER`) itself. However, that compile-time limit acts as a hard physical ceiling for the *entire* firmware lifecycle. If we compile it with a cap of 5 dBm to prevent the spike, the user will *never* be able to select `HIGH` (11 dBm) in `config.txt`, because the hardware will refuse to exceed the compiled limit.

Therefore, the initial calibration burst is an unavoidable hardware characteristic if we want to retain the *capability* of transmitting at higher levels later.

---

## 2. Force Upload in Scheduled Mode

### 2.1 New Specification
Instead of changing the helper text, the firmware behaviour will be updated to fulfill the promise of the text.

**New Requirement:**
When the device is in **Scheduled Mode**, pressing the `Force Upload` button **outside** of the designated upload hours will:
1. Immediately trigger an upload session.
2. Upload **only recent data** (based on the `RECENT_FOLDER_DAYS` config limit).
3. Bypass the `isInUploadWindow()` check strictly for this user-initiated trigger.

**Handling Old Data:**
During this out-of-hours forced upload, "old" historical data will **not** be uploaded. The device will adhere to the core philosophy of Scheduled Mode, deferring heavy historical backfills to the configured upload window.

This effectively maps the button to an "Upload Today's Data Now" action, matching user intent perfectly without breaking the protective boundaries of Scheduled Mode.
