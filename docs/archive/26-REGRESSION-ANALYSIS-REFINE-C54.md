# Refinement of Regression Findings — C54

This document re-validates the recent regression-analysis documents against the
current codebase and corrects several inaccurate or overstated claims,
especially in `docs/25-REGRESSION-ANALYSIS-CO46.md`.

This is **analysis only**. No code changes are proposed here.

---

## Scope

This refinement focuses on the specific concerns raised during review:

- validating WiFi TX power config names, defaults, and supported levels
- validating whether lower TX levels than 5 dBm exist in the current platform
- validating whether the current build already caps maximum TX power
- correcting the explanation for the `REBOOTING` status getting stuck
- validating scheduled-mode `Force Upload` behavior against helper text
- assessing whether a brownout-disable warning should be logs-only or also GUI-visible

---

## 1. High-Level Verdict

### `docs/24-REGRESSION-ANALYSIS-G31.md`

`docs/24` is **mostly still valid** for its core conclusions:

- the ULP/PCNT conflict is real
- brownout was already happening before the threshold change
- Level 0 was the pre-change brownout default
- disabling brownout entirely is risky
- ULP is not a viable replacement for hardware PCNT

I did **not** find a major contradiction to those central findings in the current code.

### `docs/25-REGRESSION-ANALYSIS-CO46.md`

`docs/25` contains **several incorrect or insufficiently validated statements**.
The most important ones are:

- it uses the wrong config key name: `WIFI_TX_POWER` instead of `WIFI_TX_PWR`
- it states the default TX power as `MID / 8.5 dBm`, which is **not the current constructor default**
- its explanation for the `REBOOTING` status sticking is incomplete / wrong
- it assumes too much certainty about the precise brownout trigger point from the logs
- it proposes a logs-only warning for brownout disable, which is not sufficient for the level of risk involved

---

## 2. WiFi TX Power — What the Code Actually Supports

## 2.1 Accepted config key

The config parser accepts:

- `WIFI_TX_PWR`
- `WIFI_PWR_SAVING`

This is implemented in `src/Config.cpp`:

- `key == "WIFI_TX_PWR"` → `wifiTxPower = parseWifiTxPower(value);`
- `key == "WIFI_PWR_SAVING"` → `wifiPowerSaving = parseWifiPowerSaving(value);`

I found **no support** for `WIFI_TX_POWER`.

### Correction

Any report or recommendation that tells the user to set:

```ini
WIFI_TX_POWER=LOW
```

is incorrect for the current codebase.

The accepted key is:

```ini
WIFI_TX_PWR=LOW
```

---

## 2.2 Current application-level TX power levels

The app-level enum in `include/Config.h` exposes exactly four named levels:

- `LOW`
- `MID`
- `HIGH`
- `MAX`

Those map in `src/WiFiManager.cpp` to:

- `LOW` → `WIFI_POWER_5dBm`
- `MID` → `WIFI_POWER_8_5dBm`
- `HIGH` → `WIFI_POWER_11dBm`
- `MAX` → `WIFI_POWER_19_5dBm`

So, at the application/config level, the currently exposed choices are:

- **5 dBm**
- **8.5 dBm**
- **11 dBm**
- **19.5 dBm**

---

## 2.3 Does the platform support lower than 5 dBm?

**Yes.**

The installed Arduino-ESP32 WiFi API (`WiFiGeneric.h`) exposes these additional
runtime constants:

- `WIFI_POWER_2dBm`
- `WIFI_POWER_MINUS_1dBm`

So the platform supports lower settings than the firmware currently exposes.

### Important distinction

- **Platform capability**: supports 2 dBm and -1 dBm
- **Current firmware UI/config surface**: only exposes 5 / 8.5 / 11 / 19.5 dBm

So the statement "the lowest setting is 5 dBm" is only true for the **current
firmware config interface**, not for the underlying WiFi stack.

---

## 2.4 What is the current default?

This needs careful wording because there are **two different defaults** in the code.

### Constructor default

In `src/Config.cpp`, the `Config` constructor currently sets:

- `wifiTxPower(WifiTxPower::POWER_LOW)`

That means the **current object default** is:

- `LOW`
- **5 dBm**

### Parser fallback for invalid values

In the same file, `parseWifiTxPower()` falls back to:

- `WifiTxPower::POWER_MID`

with the comment:

- `Default: 8.5dBm`

That means:

- if the config key is **missing entirely**, the constructor default remains `LOW`
- if the config key is **present but invalid / unrecognized**, the parser fallback becomes `MID`

### Correction

The claim in `docs/25` that the first boot runs at:

- `default: MID / 8.5 dBm`

is **incorrect for the current codebase as written**.

The more accurate statement is:

- **Current constructor default**: `LOW / 5 dBm`
- **Invalid-value parser fallback**: `MID / 8.5 dBm`

That distinction matters and should not be blurred.

---

## 2.5 Does the build already cap maximum TX power?

The current `sdkconfig.defaults` contains:

```ini
CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11
```

and the surrounding comment states that this is intended to:

- cap PHY-level maximum WiFi TX power at **11 dBm**
- prevent higher peak power during RF calibration and association

So, **within this repository**, the intended answer is yes:

- the build is already configured to cap maximum TX power at **11 dBm**

### What this means for current docs and UX

There is now a mismatch between:

- runtime enum labels (`MAX` maps to `WIFI_POWER_19_5dBm` in code)
- build-time cap (`CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11` in `sdkconfig.defaults`)

So in this build, `MAX` may **not** actually behave as an uncapped 19.5 dBm mode.
At minimum, this is a documentation and expectation mismatch.

### Practical conclusion

For current documentation, the safest statement is:

- the firmware **requests** up to 19.5 dBm at runtime for `MAX`
- the build configuration also **declares an 11 dBm PHY cap**
- therefore users should **not assume** that `MAX` really means 19.5 dBm in this build

---

## 3. Brownout / TX-Power Conclusions That Need Correction

## 3.1 What in `docs/25` is incorrect here

The following statements in `docs/25` are incorrect or misleading:

- `WIFI_TX_POWER` is the config key
- the current default is `MID / 8.5 dBm`
- the effective lowest relevant setting is 5 dBm without acknowledging lower platform support

## 3.2 What is safer to say instead

A corrected formulation would be:

- the current firmware config key is `WIFI_TX_PWR`
- the currently exposed named levels are `LOW`, `MID`, `HIGH`, `MAX`
- those map to 5 / 8.5 / 11 / 19.5 dBm in `WiFiManager.cpp`
- the current `Config` constructor default is `LOW` (5 dBm)
- the underlying platform also supports 2 dBm and -1 dBm, but the current firmware does not expose them
- the build currently declares `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11`, so documentation should not present `MAX` as definitely delivering 19.5 dBm in this build

---

## 4. `REBOOTING` Status Bug — Correct Root Cause

The explanation in `docs/25` was incomplete.

The bug is **not just** that the failure threshold is too aggressive.
That may cause false entry into `REBOOTING`, but it does **not** explain why the
status can remain stuck while the rest of the dashboard keeps updating.

### Actual root cause in `include/web_ui.h`

`renderStatus(d)` does this:

```javascript
var _newSt = d.state || '';
if (_newSt !== currentFsmState) {
  currentFsmState = _newSt;
  seti('d-st', badgeHtml(currentFsmState || '?'));
}
```

That means the status badge is only redrawn when the newly received state string
is different from the cached `currentFsmState`.

But on fetch failure, `pollStatus()` directly does:

```javascript
seti('d-st','<span class="badge bc">REBOOTING</span>');
```

It does **not** update `currentFsmState`.

### Resulting failure mode

A typical sequence is:

1. Real device state is `COOLDOWN`
2. `currentFsmState` is already `COOLDOWN`
3. Status fetch fails briefly
4. UI sets `d-st` to `REBOOTING`
5. Later, status fetch succeeds and returns `COOLDOWN`
6. `renderStatus()` sees `_newSt === currentFsmState`
7. Therefore it **does not redraw** `d-st`
8. All the other status fields keep updating, but the state badge stays stuck on `REBOOTING`

This matches the user-observed behavior much better:

- status can remain stuck for minutes
- other data continues updating
- if the real state remains unchanged (for example a long `COOLDOWN`), the stale `REBOOTING` badge can persist for the entire duration

### Correct conclusion

The stuck `REBOOTING` bug is fundamentally a **state-rendering bug**, not merely
a polling-threshold bug.

### Secondary issue

The threshold logic is still arguably too aggressive:

```javascript
if(statusFailCount>=2||rebootExpected)
```

But that is a **separate** issue from the stuck-badge bug.

### Better wording for the report

The code supports these two distinct findings:

1. **False transition into `REBOOTING`** can happen because the UI declares
   `REBOOTING` after only 2 failed polls.
2. **Persistent stale `REBOOTING` badge** happens because `renderStatus()` only
   redraws the state badge when the underlying FSM state string changes, while
   the fetch-failure path mutates the badge text directly without syncing
   `currentFsmState`.

That is the correct explanation based on the current code.

---

## 5. Scheduled Mode `Force Upload` — Helper Text Is Wrong

The user-reported behavior is confirmed by code inspection.

### Current helper text in the dashboard

In `include/web_ui.h`, when mode is `SCHEDULED` and the device is outside the
upload window, the helper says:

```text
Force Upload (not recommended) → forces an upload of recent data now.
```

### Actual server behavior

In `src/CpapWebServer.cpp`, `/trigger-upload` explicitly rejects the request
outside the upload window in scheduled mode:

- if `!scheduleManager->isSmartMode()`
- and `!scheduleManager->isInUploadWindow()`
- then it returns a JSON message saying scheduled mode only uploads in the window
  and advises switching to smart mode

So the user report is correct: the helper text and behavior do not match.

### Scheduler behavior confirms the same thing

This is not only a web-route policy. The scheduler itself also confirms the mismatch.

In `src/ScheduleManager.cpp`:

- `canUploadFreshData()` returns `true` anytime only in smart mode
- in scheduled mode, `canUploadFreshData()` returns `isInUploadWindow()`
- `canUploadOldData()` returns `isInUploadWindow()` in both modes

Then in `src/main.cpp`, `handleUploading()` chooses the upload filter from:

- `canFresh`
- `canOld`

If both are false, it logs:

- `No data category eligible, releasing`

So even if the trigger route did not reject the action, the current scheduler
logic would still refuse both fresh and old uploads outside the scheduled window.

### Correct conclusion

The current codebase does **not** support:

- scheduled mode
- outside upload window
- `Force Upload`
- recent-data-only upload

That is only what the helper text claims.

### Therefore

The helper text is wrong.

It should not describe a capability that the server route and scheduler do not implement.

---

## 6. Brownout-Disable Warning — Logs Alone Are Not Enough

The earlier report suggested logging a prominent warning if brownout detection is disabled.
That is not sufficient.

I agree with the revised requirement:

- a log line alone is too easy to miss
- the risk is not routine or cosmetic
- disabling brownout protection changes the device's safety envelope in a way
  that should remain visible in the GUI

### Why GUI visibility matters

If brownout protection is disabled, the user is accepting elevated risk of:

- undefined MCU behavior during undervoltage
- SD-card write corruption
- internal flash / filesystem corruption
- harder-to-diagnose intermittent failures

That is the kind of setting that should have a **persistent, highly visible GUI warning**,
not just a line in the Logs tab.

### Appropriate UX shape

A suitable design would be something analogous in visibility to the existing top-level banners,
for example:

- a persistent warning panel near the top of the dashboard
- a warning icon
- concise risk text explaining that undervoltage protection is disabled
- explicit mention of SD-card and filesystem corruption risk

### Conclusion

For any future implementation of brownout disable, the warning should be:

- in logs
- in config/editor messaging
- and as a visible dashboard banner / warning block

A logs-only warning would be insufficient.

---

## 7. What Still Looks Valid From the Earlier Reports

The following findings remain well-supported by the current code:

### 7.1 ULP/PCNT conflict is real

`UlpMonitor` still reconfigures GPIO 33 into RTC mode, while `TrafficMonitor`
uses PCNT on the same signal for bus silence detection.

The architectural conflict remains real.

### 7.2 The FSM still does not use ULP activity for silence decisions

The upload FSM still relies on `TrafficMonitor` / idle timing for bus-silence decisions.
The ULP path is not the authoritative source for those transitions.

### 7.3 Brownout default before the explicit change was Level 0

The repository still treats the explicit Level 7 setting in `sdkconfig.defaults`
as an override, which is consistent with the earlier finding that the prior
state was the framework default Level 0.

### 7.4 Raising the threshold to Level 7 still increases reboot sensitivity

The current `sdkconfig.defaults` still sets:

```ini
CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_7=y
```

So the general concern remains valid:

- a higher threshold means resets happen sooner during rail sag

That part of the earlier brownout analysis still stands.

---

## 8. What Should Be Corrected in Future Reporting

If these findings are rolled into a future consolidated report, the corrected statements should be:

### WiFi TX power

Use:

- `WIFI_TX_PWR`

Do not use:

- `WIFI_TX_POWER`

State clearly that:

- current firmware-exposed levels are 5 / 8.5 / 11 / 19.5 dBm
- current constructor default is `LOW / 5 dBm`
- invalid config fallback is `MID / 8.5 dBm`
- platform supports 2 dBm and -1 dBm but firmware does not currently expose them
- build config declares a PHY max TX cap of 11 dBm

### `REBOOTING` bug

Describe it as:

- partly a false-positive threshold problem
- primarily a stale state-badge rendering problem caused by `currentFsmState`
  not being updated when the failure path writes `REBOOTING` directly into the DOM

### Scheduled-mode force upload

Describe it as:

- helper text claims recent-data force upload outside window
- current web route explicitly rejects that behavior
- current scheduler also disallows it
- therefore the current firmware behavior contradicts the helper text

### Brownout-disable UX

Describe it as:

- requiring both logs and a prominent persistent GUI warning
- not logs alone

---

## 9. Final Conclusions

1. `docs/24-REGRESSION-ANALYSIS-G31.md` remains broadly sound.
2. `docs/25-REGRESSION-ANALYSIS-CO46.md` contains important factual errors and should not be treated as fully authoritative.
3. The most important corrections are:
   - the correct config key is `WIFI_TX_PWR`
   - current default TX power is `LOW / 5 dBm` at constructor level, not `MID / 8.5 dBm`
   - the platform supports lower power levels than the firmware currently exposes
   - the `REBOOTING` stuck bug is mainly caused by stale state rendering logic
   - scheduled-mode outside-window `Force Upload` is not implemented, despite the helper text claiming otherwise
   - a future brownout-disable option would require a visible GUI warning, not just logging

---

## Short Answer to the User's Specific Questions

| Question | Answer |
|---|---|
| Is `WIFI_TX_POWER` the right config key? | **No.** The current parser accepts `WIFI_TX_PWR`. |
| Does the platform support 2 dBm? | **Yes.** The installed WiFi API exposes `WIFI_POWER_2dBm` and even `WIFI_POWER_MINUS_1dBm`. |
| Can max TX power be limited below the currently described top value? | **The repo already declares a build-time PHY cap of 11 dBm** in `sdkconfig.defaults`, while runtime code still exposes `MAX=19.5 dBm`, creating a mismatch. |
| Is `MID / 8.5 dBm` the current default? | **Not as the constructor default.** Current constructor default is `LOW / 5 dBm`; `MID / 8.5 dBm` is only the parser fallback for invalid values. |
| Why does `REBOOTING` stay stuck while other fields update? | Because the failure path writes `REBOOTING` directly to the DOM, but `renderStatus()` only redraws the state badge when the FSM state string changes. If the real state remains `COOLDOWN`, the badge can stay stale for the whole cooldown. |
| In scheduled mode outside the window, should Force Upload upload recent data now? | **The helper says yes, but the current code says no.** The route rejects it, and the scheduler also disallows it. |
| Is a log warning alone enough for disabled brownout detection? | **No.** A prominent GUI warning/banner would also be needed. |
