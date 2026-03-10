# Regression Analysis Summary — C54

This document re-checks the recent regression-analysis documents:

- `docs/23-REGRESSION-ANALYSIS-C54.md`
- `docs/24-REGRESSION-ANALYSIS-G31.md`
- `docs/25-REGRESSION-ANALYSIS-CO46.md`
- `docs/26-REGRESSION-ANALYSIS-REFINE-C54.md`
- `docs/27-REGRESSION-ANALYSIS-REFINE-G31.md`
- `docs/28-REGRESSION-ANALYSIS-REFINE-CO46.md`

It also consolidates the implementation changes that should be made.

This is **analysis only**. No code changes are made here.

---

## 1. Executive Summary

### 1.1 High-level status of the recent documents

- `docs/23` is **mostly correct** on the core regressions:
  - ULP/PCNT conflict
  - SSE fallback being too heavy
  - brownout threshold increase making resets more likely on weak hardware
- `docs/24` is **mostly correct** and still useful, especially on:
  - Level 0 as the pre-change brownout default
  - ULP not being a viable replacement for hardware PCNT
  - reverting away from Level 7 for marginal devices
- `docs/25` is **not fully authoritative** and contains important factual errors
  and incomplete explanations
- `docs/26` is **factually correct** against the current codebase
- `docs/27` is **partly correct**, but its WiFi-init explanation is imprecise and
  its proposed naming surface is too large
- `docs/28` is **mostly factually correct**, with two caveats noted below

### 1.2 Most important consolidated conclusions

1. The ULP/PCNT conflict is real and should be removed from the SD-card safety path.
2. SSE should remain the preferred logging transport; the current reconnect and
   tab-switch behavior is wrong.
3. The `REBOOTING` issue has **two** separate causes:
   - false-positive entry after too few failed polls
   - stale status badge rendering after recovery
4. `WIFI_TX_PWR` is the correct config key; `WIFI_TX_POWER` is wrong.
5. The current constructor default is **5 dBm**, but the parser fallback is still
   **8.5 dBm**; these should be aligned.
6. The WiFi RF calibration spike cannot be fully controlled from `config.txt`.
   A different compile-time PHY cap is required if the initial spike must be
   reduced.
7. In Scheduled Mode, outside-window `Force Upload` for recent data is **not**
   implemented today, but should be.
8. If brownout disable is ever offered, it must have a **persistent GUI warning**,
   not only a log line.

### 1.3 Important policy statement

**Backward compatibility is not required.**

That means future implementation should optimize for a **clean, correct,
low-risk design**, not for preserving legacy names, legacy aliases, or legacy
behavioral quirks.

In practical terms:

- old WiFi TX power names do **not** need alias support
- wrong legacy config names do **not** need compatibility shims
- the UI and config surface can be simplified cleanly
- recommendation quality should take priority over preserving old semantics

---

## 2. Validation of Each Recent Document

## 2.1 `docs/23-REGRESSION-ANALYSIS-C54.md`

### Verdict

**Mostly correct.**

### What remains valid

- ULP is interfering with the activity-detection architecture
- the current ULP path is not actually used to decide bus safety
- SSE reconnect behavior is too heavy because it falls back to `/api/logs/full`
- raising brownout threshold to Level 7 can increase resets on marginal hardware

### What should be viewed as superseded/refined

- the brownout policy discussion was later refined by `docs/26` and `docs/28`
- `docs/23` is directionally right, but later documents are more precise on:
  - WiFi TX power key names
  - default vs parser-fallback distinction
  - `REBOOTING` root cause
  - scheduled-mode `Force Upload`

---

## 2.2 `docs/24-REGRESSION-ANALYSIS-G31.md`

### Verdict

**Mostly correct.**

### What remains valid

- Level 0 was the pre-change default brownout threshold
- ULP cannot replace hardware PCNT for this use case
- disabling brownout is risky
- reverting away from Level 7 is reasonable for marginal hardware

### Caveat

The document is still broadly sound, but later documents provide better detail on:

- WiFi TX power config surface
- `REBOOTING` rendering behavior
- scheduled-mode `Force Upload`

---

## 2.3 `docs/25-REGRESSION-ANALYSIS-CO46.md`

### Verdict

**Contains important factual errors and should not be treated as authoritative.**

### Key problems

- uses `WIFI_TX_POWER` instead of `WIFI_TX_PWR`
- treats `MID / 8.5 dBm` as the effective default without separating:
  - constructor default
  - parser fallback
- does not correctly explain why `REBOOTING` can remain stuck while other
  fields continue updating
- treats some brownout conclusions with more certainty than the evidence supports
- is outdated on the scheduled-mode `Force Upload` interpretation

### Status

This document should be treated as **superseded by `docs/26` and `docs/28`**.

---

## 2.4 `docs/26-REGRESSION-ANALYSIS-REFINE-C54.md`

### Verdict

**Factually correct.**

### Confirmed-correct points

- correct config key: `WIFI_TX_PWR`
- current constructor default: `LOW / 5 dBm`
- invalid-value parser fallback: `MID / 8.5 dBm`
- platform supports lower levels than current firmware exposes
- build already caps PHY TX power at 11 dBm
- `REBOOTING` stuck-state explanation is correct
- scheduled-mode outside-window force upload is not implemented today
- logs-only warning for brownout disable is insufficient

---

## 2.5 `docs/27-REGRESSION-ANALYSIS-REFINE-G31.md`

### Verdict

**Partly correct, but not final.**

### Correct points

- dropping 19.5 dBm is sensible
- force-upload requirement is valid
- general RF-spike explanation is directionally correct

### Problems

1. **The WiFi-init explanation is imprecise.**
   
   The problem is **not** that `config.txt` is read too late.
   `config.txt` is already loaded before WiFi init starts.
   
   The real problem is:
   
   - `WiFi.mode(WIFI_STA)` triggers WiFi start / RF calibration
   - `WiFi.setTxPower()` happens afterward
   - there is no public API to set the calibration ceiling before WiFi start

2. **The proposed TX-power naming table is too large.**
   
   It exposes too many levels for this product and includes levels above the
   build's current practical cap.

### Status

This document is useful as an intermediate refinement, but it is **superseded by
`docs/28`** for WiFi power recommendations.

---

## 2.6 `docs/28-REGRESSION-ANALYSIS-REFINE-CO46.md`

### Verdict

**Mostly factually correct.**

### What is correct

- `docs/26` remains valid
- `docs/27` needs correction on the WiFi-init explanation
- RF calibration spike cannot be fully controlled from `config.txt`
- compile-time PHY cap is the only real control point for the calibration spike
- simplified WiFi TX naming is the right direction
- scheduled-mode `Force Upload` should support recent-data-only uploads outside
  the window

### Caveats / corrections

#### 1. Backward-compatibility recommendation should be removed

`docs/28` includes a backward-compatibility section for old WiFi TX power names.

That is **not required** and should **not** drive the implementation.

A clean break is preferable.

#### 2. The low-power build-target example is conceptually correct, but the exact
`sdkconfig` inheritance wording should be treated as illustrative

The core finding is correct:

- a separate low-power firmware target is feasible
- it should use a lower `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER`

However, the exact mechanics of how the new target reuses base settings are a
build-system implementation detail. The **recommendation itself is valid**, but
its example should be read as schematic, not as a guaranteed drop-in final config.

#### 3. Current-draw figures are engineering estimates, not project measurements

The order-of-magnitude reasoning is sound, but the numbers should be treated as:

- typical ESP32-level estimates
- not a calibrated measurement from this exact hardware stack

### Overall status of `docs/28`

**Yes — materially, it is factually correct.**

Its only meaningful adjustments are:

- remove backward-compatibility as a requirement
- treat build-target inheritance details as implementation-specific
- keep the current figures clearly labeled as estimates

---

## 3. What Needs to Be Implemented

This section consolidates **all** changes that should be implemented, based on
all recent findings.

## 3.1 Priority 1 — Remove ULP from SD-card takeover safety path

### Why

This is the highest-risk issue because it can cause false SD takeover during
active therapy.

### Required changes

- remove the current `UlpMonitor` path from normal runtime SD-safety logic
- use only `TrafficMonitor` / PCNT for:
  - Smart Wait
  - LISTENING idle detection
  - any decision that grants SD access
- remove all runtime code that reconfigures `CS_SENSE` as RTC GPIO while PCNT
  is expected to observe it

### Recommendation

Because backward compatibility is not required, the cleanest implementation is:

- completely remove the current ULP monitor implementation and all live use sites
- do **not** retain compatibility glue around it

---

## 3.2 Priority 2 — Fix SSE architecture and reconnect behavior

### Why

The current logging path is wasting bandwidth and power, and it regresses UX.

### Required changes

#### A. Keep SSE as the preferred transport

The intended priority should be:

1. **SSE live stream**
2. **poll `/api/logs` tail**
3. **full backfill `/api/logs/full` only when truly needed**

#### B. Fix SSE reconnect behavior

On SSE loss:

- attempt to reconnect SSE first
- after a small number of failed SSE retries, fall back to `/api/logs`
- do **not** immediately call `/api/logs/full`

#### C. Reserve `/api/logs/full` for limited cases only

Use `/api/logs/full` only for:

- first-ever Logs tab open
- confirmed reboot / history restoration
- explicit manual history request

#### D. Add SSE keepalive

- send periodic SSE keepalive comments during idle periods
- do not let the connection appear dead just because no new logs were emitted

#### E. Stop breaking SSE on tab switch

- do **not** call `stopSse()` every time the user leaves the Logs tab
- when returning to Logs, prefer `startSse()` over starting polling
- let SSE remain alive across tabs unless the page is actually unloading or the
  connection is genuinely lost

#### F. Reconsider SSE suppression in brownout-recovery mode

Current behavior suppresses SSE when `g_brownoutRecoveryBoot` is active.
That pushes the UI into heavier fallback paths.

Required direction:

- do not fully disable SSE in brownout-recovery mode
- if throttling is needed, reduce frequency rather than removing SSE entirely

---

## 3.3 Priority 3 — Fix the `REBOOTING` UI issue completely

### Why

The current problem has been misdescribed in earlier documents. It is not only a
threshold issue.

### Required changes

#### A. Fix stale badge rendering

The failure path writes `REBOOTING` directly into the DOM but does not synchronize
`currentFsmState`.

Required fix direction:

- ensure successful status renders always restore the badge text correctly
- do not leave the badge dependent on `_newSt !== currentFsmState` after the DOM
  was manually overwritten by the error path

This is the root cause of the "stuck REBOOTING while other fields update" issue.

#### B. Make the reboot threshold less aggressive

Current behavior:

- after only 2 failed polls, the UI shows `REBOOTING`

Required fix direction:

- introduce a longer grace period before declaring reboot
- distinguish short transient disconnects from real reboot suspicion

#### C. Add a distinct intermediate offline/reconnecting state

Required direction:

- show something like `Offline — reconnecting...` before showing `REBOOTING`
- reserve `REBOOTING` for:
  - explicit reboot actions
  - or longer / stronger evidence of device restart

#### D. Handle mobile tab resume better

Required direction:

- use visibility/resume awareness to avoid treating brief resume failures as reboot
- reset or soften the failure counter when the tab becomes visible again

---

## 3.4 Priority 4 — Redesign WiFi TX power config surface

### Why

The current config surface is inconsistent and misleading:

- wrong key used in older docs
- runtime names do not match the build cap cleanly
- parser fallback does not match default
- 19.5 dBm is not appropriate for this product

### Required changes

#### A. Keep the correct config key

Use only:

- `WIFI_TX_PWR`

Do not support or document:

- `WIFI_TX_POWER`

#### B. Remove 19.5 dBm mode from the config surface

Because backward compatibility is not required:

- remove the 19.5 dBm mode cleanly
- do not retain it as a legacy alias

#### C. Adopt a simplified naming scheme

Recommended clean-break surface:

| `config.txt` Value | dBm |
| :--- | :--- |
| `LOWEST` | -1 |
| `LOW` | 2 |
| `MID` | 5 |
| `HIGH` | 8.5 |
| `MAX` | 11 |

This is the maximum I would expose. I would not expose more levels than this.

#### D. Align constructor default and parser fallback

Both should point to the same value:

- **`MID` = 5 dBm**

This removes the current inconsistency where missing config and invalid config
produce different effective defaults.

#### E. Clean break, no aliases

Because backward compatibility is not required:

- no old-name aliasing is needed
- no `LOW`→old 5 dBm compatibility behavior is needed
- no `MAXIMUM` legacy preservation is needed unless explicitly desired later

---

## 3.5 Priority 5 — Address the RF calibration spike properly

### Why

For weak CPAP power supplies, the initial WiFi RF calibration spike is a real
risk point.

### Confirmed constraint

This spike cannot be fully controlled from `config.txt` because the calibration
ceiling is compile-time controlled by `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER`.

### Required implementation direction

#### A. Standard build

- keep the normal build with its current higher cap for users who need range

#### B. Low-power build variant

Add a separate low-power firmware target for marginal devices with a lower PHY cap.

Recommended starting point:

- `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=5`

This is the cleanest way to reduce the initial spike without distorting the
standard build for everyone.

#### C. Documentation change

Document clearly that:

- runtime `WIFI_TX_PWR` affects post-start transmissions
- the initial calibration burst is governed by the compile-time PHY cap

---

## 3.6 Priority 6 — Implement Scheduled-Mode Force Upload correctly

### Why

The helper text expresses intended user behavior more accurately than the current
firmware implementation.

### Required changes

When in **Scheduled Mode** and outside the upload window:

- pressing `Force Upload` should be accepted
- upload should process **recent data only**
- recent-data selection should follow `RECENT_FOLDER_DAYS`
- old/historical data should remain window-gated
- normal inside-window behavior should remain unchanged

### Important note

The helper text does **not** need to be weakened.

The firmware should be changed to match the intended helper text.

---

## 3.7 Priority 7 — Revisit brownout policy and brownout-recovery behavior

### Why

Brownout policy affects both system stability and the web/logging experience.

### Required changes

#### A. Revert the default threshold away from Level 7

Recommended direction:

- revert the default threshold to Level 0

This is the most conservative compatibility move while keeping hardware fail-fast
protection.

#### B. If brownout disable is ever offered, make it explicit and high-risk

It should be:

- opt-in only
- not default
- not quietly hidden in logs

#### C. Add prominent GUI warning for brownout disable

Required UX:

- persistent warning block/banner in dashboard
- warning icon / high-visibility styling
- explicit SD-card / filesystem corruption risk wording
- log warning still present, but not the only warning

#### D. Reconsider degraded brownout-recovery side effects

If brownout-recovery mode remains, it should not unnecessarily damage observability.

Required direction:

- do not let brownout-recovery make SSE effectively unusable
- keep diagnostics visible even during degraded boot

---

## 3.8 Priority 8 — Update docs, config reference, and UI messaging

### Why

The code and the docs are currently out of sync.

### Required changes

- update all WiFi TX power documentation to use `WIFI_TX_PWR`
- remove references to 19.5 dBm as a real effective mode in this build
- document the constructor-default vs parser-fallback issue only until it is fixed
- after it is fixed, document a single clean default
- document scheduled-mode Force Upload as recent-data-only outside the window
- document RF-calibration spike behavior accurately
- document brownout-disable warnings as GUI-visible, not logs-only

---

## 4. Recommended Implementation Order

1. **Remove ULP from SD takeover decisions**
2. **Fix SSE architecture and tab-switch behavior**
3. **Fix the `REBOOTING` rendering + threshold logic**
4. **Implement clean WiFi TX power redesign**
5. **Add low-power build target for marginal CPAP devices**
6. **Implement Scheduled-Mode Force Upload outside upload window**
7. **Revert brownout default to Level 0 and add proper brownout-disable UX**
8. **Bring all docs/config reference into sync**

---

## 5. Final Conclusions

1. `docs/23` and `docs/24` remain useful for the core regressions.
2. `docs/25` should no longer be treated as authoritative.
3. `docs/26` is the strongest factual correction document.
4. `docs/27` is useful but intermediate.
5. `docs/28` is materially correct, but:
   - backward compatibility should be dropped as a requirement
   - build-target inheritance details should be treated as implementation-specific
6. The most important implementation themes are:
   - remove ULP/PCNT conflict
   - restore SSE-first logging
   - fix `REBOOTING` correctly
   - clean up WiFi TX power semantics
   - support a low-power build for weak CPAP hardware
   - implement recent-data-only Force Upload in Scheduled Mode

7. **Because backward compatibility is not required, the cleanest future implementation is a deliberate clean break, not an alias-heavy compatibility layer.**
