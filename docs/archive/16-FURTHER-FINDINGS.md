# Further Findings: Brownout Risk, Power Draw, and Remaining Optimization Opportunities

## Scope

This document analyzes whether the current firmware can still contribute to brownouts or SD-card-related instability on constrained CPAP hardware, with special attention to:

- External field reports from `amanuense/CPAP_data_uploader` issues `#29`, `#42`, and `#26`
- Current firmware behavior in this repository
- WiFi, web server, upload paths, power management, and build configuration
- The correctness of the supplementary AP/network advice

This is **analysis only**. No firmware changes are proposed here as code edits.

---

## Executive Summary

## Bottom line

The current firmware already implements the **major high-value power reductions**:

- CPU throttled to `80 MHz` at boot
- `802.11b` disabled at runtime
- WiFi TX power reduced early, before association
- WiFi modem sleep enabled by default
- Bluetooth disabled at build time and released at runtime
- Auto light-sleep enabled in low-power FSM states
- TLS stack trimmed toward lower CPU cost

These were the right direction.

## Main conclusion

The **web server is not the primary brownout source**.

The highest-risk power events are still:

- **WiFi association / reconnect bursts**
- **Cloud / SleepHQ TLS handshakes and long TLS uploads**, especially large `BRP.edf` files
- **Repeated reconnect / re-auth / mDNS restart cycles** during unstable network periods
- **Any mode that keeps the device out of low-power states for long periods**

The web server can still **amplify** power usage when a browser is open and actively polling / streaming logs, but it is not the main driver compared with RF + TLS activity.

## Important nuance

Not every reported `SD Card Error` is a brownout.

The external issues strongly suggest there are **at least two overlapping failure classes**:

- **Power integrity / RF current spike problems**
- **SD card handoff / card state / timing issues**

That distinction matters because some field reports persisted even in `SMB-only` tests or immediately after CPAP stop events, which points to **card-state / release timing issues**, not only WiFi current draw.

---

## Evidence from External Issues

## Issue #29: `Wifi powerdown during upload`

Observed behavior:

- Device went offline during an upload window
- Later came back on its own
- Manual upload later succeeded

What this suggests:

- The failure is **load-dependent**, not a permanent inability to connect
- It is consistent with either:
  - transient power instability during heavier upload activity, or
  - a reconnect / transport failure that later self-recovers

The later comments are especially important because they show:

- repeated `SleepHQ` streaming failures on large files
- reconnect attempts
- WiFi disconnect reasons like `8` and `201`
- repeated mDNS restarts
- failures clustering around large Cloud uploads rather than idle use

This is strong evidence that **Cloud/TLS traffic is a much more likely power trigger than the passive web UI itself**.

## Issue #42: AirSense 11 field reports

This issue is the strongest evidence set.

Observed patterns include:

- web UI works initially
- CPAP later shows `SD Card Error`
- Cloud uploads are especially problematic
- large file uploads fail during streaming
- WiFi disconnect / reconnect loops occur during Cloud activity

One especially important detail from the discussion:

- the reporter also saw problems in an `SMB-only` test
- the error could appear around CPAP stop / card activity timing

This means:

- **brownout is likely real**, but
- **brownout alone does not explain all SD-card failures**

The issue likely combines:

- device-to-device CPAP power variation
- SD handoff/state sensitivity
- network stack and upload load
- possible CPAP-specific sensitivity to card state after release

## Issue #26: older fork / early context

This issue helps explain why some failures are **not purely power**.

Important comments indicate:

- earlier versions suffered from CPAP/ESP timing conflicts
- the `CS_SENSE` signal may not perfectly represent true card bus ownership / state on all hardware
- the original project owner suspected the CPAP may leave the card in a state the ESP or CPAP later mishandles

That supports the view that:

- `SD Card Error` can be caused by **card-state and handoff problems**, even if power is also a factor
- analyzing brownout risk in isolation is necessary, but not sufficient

---

## Current Firmware: What Is Already Good

## 1. Early CPU throttling is correct

In `src/main.cpp`, `setup()` immediately forces:

- `setCpuFrequencyMhz(80);`

This is a very good choice for constrained hardware because it reduces boot-time current draw before almost anything else happens.

## 2. Bluetooth disable / release is correct

Current implementation does both:

- compile-time `CONFIG_BT_ENABLED=n` in `sdkconfig.defaults`
- runtime `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` in `main.cpp`

That is appropriate and likely worth keeping.

## 3. Disabling `802.11b` was the right move

This was worth re-checking carefully.

The current code **does** disable `802.11b` at runtime:

- `src/WiFiManager.cpp`
- `esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);`

This is likely one of the highest-value RF-side mitigations in the current firmware.

Conclusion:

- **Yes, disabling the older WiFi mode was the right decision.**
- The supplementary text claiming `802.11b` is the lower-power safer mode is **not correct for this ESP32 use case**.
- On ESP32-class hardware, `802.11b` fallback is exactly the mode you generally want to avoid when trying to suppress peak TX current.

## 4. Early TX power limiting is correct

The firmware currently applies TX power twice:

- **before association** via `applyTxPowerEarly()`
- **after connection** via `applyPowerSettings()`

That is the right general strategy.

It reduces the risk that association/scanning happens at an unnecessarily high power level.

## 5. Default low-power runtime settings are directionally correct

Defaults in `src/Config.cpp` are now:

- `CPU_SPEED_MHZ = 80`
- `WIFI_TX_PWR = LOW`
- `WIFI_PWR_SAVING = MID`

That is a sensible default profile for the most power-sensitive users.

## 6. Light-sleep support exists and is real

The firmware enables:

- `CONFIG_PM_ENABLE=y`
- tickless idle
- `esp_pm_configure(...)`
- GPIO wakeup
- PM lock management across FSM states

That is good architecture.

## 7. Cloud path no longer explicitly disables WiFi sleep

This matters.

The current `SleepHQUploader.cpp` explicitly documents that modem sleep does **not** need to be manually disabled during uploads, because the WiFi driver already takes care of PM locks during active traffic.

This is good and avoids the old failure mode where a Cloud path could accidentally leave WiFi in high-power mode permanently.

---

## What Still Looks Risky or Incomplete

## 1. The web server is secondary, but open browser sessions still cost power

The web server itself is lightweight compared with upload traffic, but it is not free.

### What the browser does now

`include/web_ui.h` currently shows:

- `/api/status` poll every `3s`
- `/api/logs` poll every `4s` if SSE is unavailable
- `/api/sd-activity` poll every `2s` while monitoring
- `EventSource('/api/logs/stream')` for live logs when supported

### Why this matters

If a browser tab is left open:

- the radio is kept more active
- the CPU handles more frequent socket activity
- SSE keeps a long-lived connection open
- the system may spend less time in deep idle conditions

### Assessment

- **Not the primary brownout source**
- **Still a real amplifier** of power usage and network churn
- Most relevant during uploads or unstable WiFi conditions

### Extra nuance from current code

The firmware already contains protections that suggest this path has caused pressure before:
 
- upload-time log throttling
- socket release via `server->client().stop()` in some pressure paths
- rate-limited log output while uploading

That is consistent with prior evidence that aggressive refresh / log activity can worsen network pressure.

## 2. Smart mode is inherently more power-hungry than scheduled mode

This is one of the most important findings.

The PM lock logic in `main.cpp` releases low-power mode only in:

- `IDLE`
- `COOLDOWN`

But **`LISTENING` is treated as an active state**.

That means:

- in **scheduled mode**, the device can spend long periods in true lower-power `IDLE`
- in **smart mode**, the device can spend large amounts of time in `LISTENING`, where light-sleep is intentionally inhibited

This implies:

- scheduled mode is structurally safer for power-sensitive machines
- smart mode may remain too expensive for the weakest AS11 units even with all current optimizations

This is not necessarily a bug. It may be a required tradeoff for bus monitoring.

But it is highly relevant operationally.

## 3. Cloud / TLS upload remains the most power-stressful path

This is the clearest runtime hotspot.

### Why

The SleepHQ path still does all of the following:

- TLS handshake
- encrypted file streaming
- long large-file writes
- reconnect loops on failure
- response waits and timeout recovery
- repeated mDNS restart after reconnect

### Code indicators

In `src/SleepHQUploader.cpp`:

- `CLOUD_UPLOAD_BUFFER_SIZE` is `4096`
- large files are streamed over TLS
- failed writes trigger reconnect / retry logic
- failed connects can lead to coordinated WiFi cycles
- there are multiple wait loops and reset paths around transport failure

### Field correlation

Issue `#29` comments line up with this exactly:

- failures cluster around large `BRP.edf` uploads
- disconnects and reconnects happen during streaming
- manual or later uploads may succeed

### Assessment

If the question is *"what part of the firmware is still most likely to trigger brownout-like behavior?"*, the answer is:

- **Cloud upload path first**
- **WiFi reconnect / reassociation behavior second**
- **Web UI only as a secondary amplifier**

## 4. Reconnect cycles are expensive and can create bursty load

`main.cpp`, `NetworkRecovery.cpp`, and `SleepHQUploader.cpp` show multiple reconnect / cycle paths.

These are useful for resilience, but from a power perspective they are expensive because they can cause:

- active scan / reassociation work
- fresh RF bursts
- fresh TLS handshakes
- mDNS restarts
- repeated socket churn

This is especially important on marginal hardware because a sequence of retries can create a cluster of current spikes instead of a single event.

## 5. `WIFI_PWR_SAVING = MAX` exists, but no `listen_interval` support is implemented

This is a real gap.

The firmware exposes:

- `SAVE_NONE`
- `SAVE_MID`
- `SAVE_MAX`

But I found **no code** configuring station listen interval via `esp_wifi_set_config()` / `wifi_config_t`.

That means:

- `MAX_MODEM` may not deliver the full theoretical benefit users might assume
- it may behave inconsistently depending on AP behavior
- documentation suggesting strong MAX-mode savings should be treated cautiously

This is one of the clearest remaining optimization opportunities.

## 6. Documentation overstates the effective maximum TX power

There is an important mismatch between code/build reality and docs.

### Build-time reality

`sdkconfig.defaults` contains:

- `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11`

This caps PHY maximum TX power at **11 dBm**.

### Runtime/docs reality

Runtime config still exposes:

- `LOW` = 5 dBm
- `MID` = 8.5 dBm
- `HIGH` = 11 dBm
- `MAX` = 19.5 dBm

### Practical implication

If the compile-time cap is active, then:

- `WIFI_TX_PWR = MAX` is **not really 19.5 dBm** in the built firmware
- `MAX` likely collapses to the compile-time cap

### Why this matters

This is not a brownout risk by itself, but it affects support expectations.

Users may think they can raise power above `11 dBm` when the firmware build may not actually allow it.

## 7. Build configuration may not fully guarantee the intended TLS compile-time savings

This is another important practical gap.

`platformio.ini` still references:

- `pre:scripts/rebuild_mbedtls.py`

But in the current workspace state that script is missing.

Also, `sdkconfig.defaults` itself already warns that some mbedTLS settings only matter if the framework actually rebuilds those libraries.

### Practical implication

The project intends to benefit from compile-time TLS reductions such as:

- ChaCha20 disabled
- AES-256 disabled
- variable TLS buffer sizes
- reduced TLS memory footprint

But depending on the build environment:

- those settings may not fully take effect
- or the build may fail because the referenced prebuild script is absent

### Power relevance

If mbedTLS is not actually rebuilt with the expected options, then:

- CPU cost during TLS may be higher than assumed
- memory pressure may be higher than assumed
- Cloud upload could be more stressful than the docs imply

## 8. Persistent log flushing is not free

The main loop flushes logs to `LittleFS` every `10 seconds`:

- `LOG_FLUSH_INTERVAL_MS = 10 * 1000`

This is internal flash I/O, not SD-card I/O, so it is much safer than the old SD logging model.

Still, it is not zero-cost:

- CPU wakes to flush
- flash writes occur regularly
- log-heavy periods will cause more internal activity

This is probably **not** a top brownout trigger, but it is a small constant background cost.

### Also note a docs mismatch

`docs/CONFIG_REFERENCE.md` currently says persisted logs flush every **30 seconds**, but the code flushes every **10 seconds**.

## 9. Some docs no longer match current defaults

I found documentation inconsistencies that matter to power analysis:

- some docs still describe `WIFI_TX_PWR` default as `MID`, while code default is now `LOW`
- config reference advertises `MAX = 19.5 dBm`, while build config caps PHY TX power at `11 dBm`
- config reference says `PERSISTENT_LOGS` default is `false`, but code currently enables log saving unconditionally via `enableLogSaving(true, &LittleFS)`
- config reference says log flush every `30s`, but code flushes every `10s`

These are support/documentation issues, but they also affect how users reason about power-saving behavior.

---

## Is the WebServer Pulling Too Much Power?

## Short answer

**No, not by itself.**

## Detailed answer

### Why it is probably not the main culprit

The web server path is relatively lightweight compared with TLS uploads:

- static HTML served from flash
- status JSON assembled with `snprintf`
- single-threaded request servicing in the main loop
- no evidence of heavy heap allocation in normal status handling

### What *can* make it matter

It becomes relevant when:

- a browser is left open on Logs/System/Monitor tabs
- SSE log stream is active
- a user repeatedly refreshes during upload
- unstable WiFi causes retries and reconnects

In those cases the web UI can:

- keep the radio active more often
- increase socket churn
- reduce effective idle periods
- interact poorly with an already stressed upload session

### My assessment

- **Not primary brownout cause**
- **Secondary contributor**
- Worth treating as a multiplier during unstable Cloud sessions

---

## Assessment of the Supplementary Network/AP Advice

The supplementary text contains a mix of valid ideas, overstatements, and incorrect claims.

## Likely correct / useful

### 1. Stronger RSSI helps

Yes.

If the AP is closer and the signal is stronger:

- retransmissions usually decrease
- airtime decreases
- the device is less likely to need aggressive RF behavior
- association and upload complete faster

So recommending:

- AP closer to bedroom
- strong dedicated 2.4 GHz coverage

is sensible.

### 2. Use 20 MHz channels on 2.4 GHz

Reasonable advice.

It will not magically fix brownout, but it can improve robustness and reduce retries in noisy environments.

### 3. Pick a clean channel manually

Reasonable.

Lower interference means fewer retries and less time spent with the radio active.

### 4. Disable aggressive band steering / roaming for this device

Reasonable.

The ESP32 is 2.4 GHz STA here. If an AP/mesh environment causes needless roaming or disruptive deauth behavior, that can absolutely worsen reconnection churn and power stress.

### 5. Dedicated 2.4 GHz SSID is a good operational recommendation

Yes, especially for troubleshooting.

It simplifies the environment and reduces AP-side surprises.

## Partially correct / too strong

### 6. DTIM tuning can matter, but not as a first-line cure

Increasing DTIM can reduce idle wake frequency under power-save operation.

But:

- it is AP-dependent in effect
- it will not solve large Cloud/TLS upload spikes
- it may trade away responsiveness

Useful as a secondary tuning knob, not a primary fix.

### 7. `802.11b/g only` is **not** good advice here

This is the biggest incorrect recommendation in the supplementary text.

The text argues that forcing older legacy mode is safer.

For this firmware on ESP32-class hardware, the current implementation is more credible:

- disable `802.11b`
- keep `11g/n`

That was the correct move.

### 8. Claims about exact exponential TX-current relationships are overstated

The overall idea that higher TX power costs more current is correct.

But some of the wording in the supplementary text is too absolute and too certain. It should be treated as engineering intuition, not validated device-specific characterization.

## Unsupported / speculative / too strong

### 9. Claims that AP changes alone can "reliably cure" brownouts are too strong

They can **help a lot**, but the issue reports show there are multiple mechanisms involved:

- CPAP hardware variation
- card-state/handoff sensitivity
- WiFi reconnect behavior
- Cloud/TLS path stress

### 10. Artificial upload throttling may help, but is not yet supported by direct evidence here

This is plausible:

- smaller bursts
- deliberate pauses
- reduced average current

But in this codebase analysis, it remains a **candidate optimization**, not a proven cause/fix.

It belongs in the "future opportunities" bucket, not as a confirmed explanation.

---

## Prioritized Remaining Optimization Opportunities

These are ordered by likely value for minimizing power draw and brownout exposure.

## Tier 1: Highest-value next opportunities

### 1. Treat `scheduled` mode as the preferred safety mode for weak AS11 hardware

Reason:

- `IDLE` allows low-power behavior
- `smart` mode tends to sit in `LISTENING`, which keeps the PM lock held

This is likely the single most important **operational** recommendation not yet emphasized strongly enough.

### 2. Minimize browser-open time during uploads

Reason:

- open Logs/System/Monitor pages add network traffic
- SSE/polling keep the radio more active
- not primary, but avoidable

### 3. Consider whether mDNS should be optional for ultra-constrained systems

mDNS is convenient, but it adds:

- multicast activity
- responder maintenance
- restart behavior after reconnects

This is not the main problem, but it is one of the few always-on network conveniences that could be made optional in a future design.

### 4. Implement real `listen_interval` support if `WIFI_PWR_SAVING = MAX` is meant to be meaningful

Current code exposes MAX mode but does not appear to configure station listen interval.

If MAX mode is going to stay user-facing, this is the most obvious missing technical piece.

## Tier 2: Medium-value opportunities

### 5. Revisit whether status snapshot updates need to run every 3 seconds even with no clients

Current behavior:

- `updateStatusSnapshot()` runs every 3 seconds in the main loop regardless of whether a browser is connected

This is not expensive, but it is still periodic always-on work.

Savings would be modest, not dramatic.

### 6. Make persistent log flushing policy power-aware

Current behavior:

- logs flush every 10 seconds continuously

Possible future idea:

- flush less often in steady-state idle
- flush more often only during failure-sensitive phases

Again, not a primary brownout lever, but a real background activity source.

### 7. Review whether SSE should be auto-preferred on constrained devices

SSE is efficient in many ways, but it also keeps a long-lived connection alive.

This may still be better than repeated full-polling in many cases, but for power-sensitive deployments it is worth measuring rather than assuming.

## Tier 3: Higher-risk or more speculative

### 8. Cloud upload pacing / deliberate throttling

Plausible future mitigation:

- smaller write bursts
- explicit inter-chunk pauses
- longer but smoother upload profile

This might reduce peak stress, but it needs measurement. It should be treated as experimental.

### 9. More aggressive feature shedding for ultra-constrained builds

Potential future direction:

- no OTA
- optional no-mDNS build
- optional no-SSE build
- cloud-disabled build for SMB-only users

This is more of a deployment/profile strategy than a universal recommendation.

---

## Specific Findings by Area

## WiFi / RF

### Findings

- `802.11b` is correctly disabled
- early TX power limiting is correctly applied
- default TX power is low
- modem sleep default is sensible
- reconnect loops remain a major stressor

### Conclusion

The WiFi stack is already much better than earlier versions, but reconnection-heavy scenarios remain a power hotspot.

## Web Server / UI

### Findings

- not primary power consumer
- can amplify radio activity with open tabs
- SSE/log streaming is the heaviest UI-side behavior
- status updates are lightweight but always-on

### Conclusion

The web server is not "too expensive" in the main sense, but it is still a multiplier under load.

## Cloud / SleepHQ

### Findings

- still the heaviest path
- large file TLS streaming is the strongest remaining risk
- repeated reconnects compound that risk

### Conclusion

If one subsystem is most likely to trigger brownout-like behavior, it is still the Cloud upload path.

## SMB

### Findings

- still active network load, but generally less computationally intense than TLS
- buffer is larger (`8192` bytes)
- synchronous networking still exists, but current code includes timeout and recovery hardening
- field evidence suggests SMB can still participate in SD-error scenarios, but not necessarily through peak power in the same way Cloud does

### Conclusion

SMB is not free, but Cloud/TLS remains the more likely pure-power trigger.

## Logging / Internal Flash

### Findings

- internal log persistence is safer than SD logging
- periodic flush every 10 seconds is still background work
- docs do not fully match behavior

### Conclusion

Small contributor, not primary.

---

## Practical Recommendations for Users Right Now

Without changing code, the safest operating guidance is:

- Prefer **`scheduled` mode** on the most power-sensitive AS11 units
- Keep the AP **very close** and use a **clean dedicated 2.4 GHz SSID**
- Prefer **20 MHz** channel width
- Avoid leaving **Logs/System/Monitor** tabs open during heavy uploads
- Keep `WIFI_TX_PWR` at `LOW` unless clearly necessary
- Use `MID` power saving by default; test `NONE` only as a diagnostic tradeoff
- Be cautious with `ENABLE_SD_CMD0_RESET=true` on AS11
- Treat Cloud upload as the highest-risk workload on marginal hardware

---

## Final Conclusion

The current firmware is **not obviously missing a single giant power bug** in the web server.

Most of the major power reductions are already in place, and they were the right moves.

The remaining picture is:

- **Brownout risk has likely been reduced substantially**, but not eliminated
- **Cloud/TLS upload and reconnect behavior remain the strongest power-risk areas**
- **The web UI is a secondary contributor, not the main cause**
- **Some SD-card errors are likely not power-related at all**, but instead arise from card-state/handoff/timing sensitivity
- **Scheduled mode is materially safer than smart mode** for weak AS11 units because it allows much more time in low-power states

The most important remaining opportunities are not broad rewrites. They are:

- better matching user-facing config/docs to real build/runtime behavior
- adding true `listen_interval` support if `MAX_MODEM` is kept
- further reducing unnecessary always-on network behavior for ultra-constrained deployments
- keeping Cloud/TLS and reconnect paths under the strongest scrutiny

---

## Appendix: Key Code Locations Reviewed

- `src/main.cpp`
- `src/WiFiManager.cpp`
- `src/CpapWebServer.cpp`
- `include/web_ui.h`
- `src/SleepHQUploader.cpp`
- `src/SMBUploader.cpp`
- `src/TrafficMonitor.cpp`
- `src/ScheduleManager.cpp`
- `src/Logger.cpp`
- `src/Config.cpp`
- `include/Config.h`
- `sdkconfig.defaults`
- `platformio.ini`
