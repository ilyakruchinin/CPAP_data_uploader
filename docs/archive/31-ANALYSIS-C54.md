# Power Optimisation Analysis C54

## Scope

This document reviews the suggestions collected in `docs/archive/31-A-BUNCH-OF-ADVICES.md` against the **actual current firmware**, the **current build/toolchain constraints**, and the **practical realities of this project**.

Goals:

- Identify which suggestions are already implemented
- Identify which suggestions are incorrect, incomplete, or impractical for this codebase
- Identify which ideas are still worth considering
- Evaluate whether a newer framework could bring meaningful benefits
- Recommend a realistic next direction without changing code yet

---

## Executive Summary

The archive notes contain a mix of:

- correct observations
- already-implemented ideas
- partially-correct but oversimplified advice
- suggestions that conflict with the current architecture
- at least one recommendation that is actively at odds with the current power strategy

### Bottom line

1. The project already implements several of the most important runtime power mitigations:
   - early CPU throttling to `80 MHz`
   - early WiFi TX power limiting before association
   - modem sleep after connection
   - runtime Bluetooth memory release
   - timed mDNS shutdown
   - brownout-recovery degraded boot behavior
   - disabling `802.11b`

2. The current stock PlatformIO Arduino environment **really does block** several compile-time optimisations:
   - `CONFIG_PM_ENABLE`
   - tickless idle
   - compile-time Bluetooth removal
   - compile-time WiFi PHY TX cap
   - most `sdkconfig.defaults` tuning

3. A newer framework **could** bring real benefits, but they are likely to be **incremental rather than miraculous**:
   - proper light-sleep
   - real PM locks
   - compile-time BT disable
   - compile-time PHY TX cap
   - true control over FreeRTOS and WiFi stack configuration

4. Some archive advice is not suitable here:
   - turning WiFi fully off between uploads as a default strategy
   - forcing a fixed low PHY rate with private/internal WiFi APIs
   - assuming arbitrary delays are generally helpful

5. If a framework migration is pursued, there are two realistic paths:
   - **lowest churn / still PlatformIO-like:** `pioarduino` + Arduino 3.x
   - **most official / best long-term control:** ESP-IDF + Arduino as an Espressif-supported component

### Recommendation

If the goal is **serious further software power reduction**, the most credible route is:

- first: validate whether a newer framework produces measurable power improvements on the affected REF IDs
- second: only then decide whether the migration cost is justified

If you want the most conservative engineering answer:

- **Do not immediately rewrite the project around framework migration**
- **Do not implement WiFi-off idle behavior as the default product behavior**
- **Do not use fixed-rate private WiFi APIs as a quick fix**
- **Do evaluate a newer framework in a controlled test branch**

---

## What the firmware already does today

This matters because several archive suggestions assume the firmware is still using mostly default ESP32 behavior. It is not.

### 1. CPU is throttled early

In `src/main.cpp`, boot begins by forcing the CPU to `80 MHz` immediately.

Purpose:

- reduce boot-time current draw
- avoid starting from the default high-frequency state
- keep WiFi init at the lowest stable CPU speed

This means advice like "underclock to 80 MHz" is **already implemented**.

### 2. Bluetooth memory is released at runtime

In `src/main.cpp`, the firmware calls:

- `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)`

Purpose:

- reclaim BT controller DRAM
- avoid carrying unused Bluetooth overhead in a WiFi-only product

This means advice like "release BT memory" is **already implemented**.

### 3. TX power is reduced before association

In `src/main.cpp` + `src/WiFiManager.cpp`, the firmware applies TX power **before** `WiFi.begin()`.

This is important because it avoids full-power scan/association behavior as much as the current framework allows.

This is more advanced than the archive suggestion to merely lower TX power after WiFi starts.

This means advice like "hard-limit WiFi transmit power" is **already implemented in a better place** than suggested.

### 4. WiFi modem sleep is already used

After connection, the firmware applies configurable power-saving modes:

- `NONE`
- `MID` = `WIFI_PS_MIN_MODEM`
- `MAX` = `WIFI_PS_MAX_MODEM` with `listen_interval=10`

This means advice like "explicitly enable modem sleep" is **already implemented**.

### 5. 802.11b is disabled

During station connection, the firmware restricts the station protocol to:

- `802.11g`
- `802.11n`

and disables:

- `802.11b`

This is a key detail because one of the archive suggestions later proposes forcing `11 Mbps` legacy rates. That conflicts with the existing strategy.

### 6. mDNS is deliberately time-limited

The firmware starts mDNS so users can reach `http://cpap.local`, but it shuts mDNS down after 60 seconds.

Purpose:

- preserve easy discovery during boot
- reduce ongoing multicast-related radio wakes after the initial access window

This is already a power optimisation tied to the product UX.

### 7. Brownout-specific degraded mode already exists

If the previous reset reason is `ESP_RST_BROWNOUT`, the firmware boots in a degraded low-power mode:

- skips mDNS
- forces lowest TX power
- forces max WiFi power save

This is already a targeted mitigation for weak hardware.

### 8. Brownout detector behavior is configurable

The firmware supports:

- `ENABLED`
- `RELAXED`
- `OFF`

with `RELAXED` disabling the detector only around the worst WiFi connection spikes, then re-enabling it.

This is already a thoughtful compromise between survivability and safety.

### 9. PM/light-sleep is attempted, but blocked by framework build configuration

The firmware does try to configure:

- DFS / PM
- auto light-sleep
- PM locks
- GPIO wakeup for CS sense

However, on the current stock PlatformIO Arduino core, PM support is not compiled in, so this path is effectively inert.

This is a real limitation of the current framework/toolchain combination, not a logic bug in the application code.

### 10. The architecture intentionally keeps WiFi available

This project is not a fire-and-forget batch uploader with no online UX.

The firmware keeps WiFi availability because the product depends on:

- local web UI
- OTA updates
- web status polling
- SSE/live logs
- local discovery window via mDNS
- remote diagnostics/troubleshooting
- controlled reconnection and recovery behaviors

That makes some generic IoT advice much less applicable here.

---

## What the archive notes get right

## 1. Stock PlatformIO Arduino ignores most `sdkconfig.defaults`

This is correct for the current environment.

The project is currently using:

- `platform = espressif32`
- `framework = arduino`
- Arduino core `2.0.17`
- precompiled framework libraries

That means many `sdkconfig.defaults` options do **not** take effect in the stock build.

Examples that are effectively blocked in the current environment:

- `CONFIG_PM_ENABLE`
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE`
- `CONFIG_FREERTOS_HZ=100`
- `CONFIG_BT_ENABLED=n`
- `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10`

This part of the archive analysis is fundamentally correct.

## 2. PM is unavailable in the current stock toolchain

Also correct.

The application code tries to configure PM, but the stock framework was built without PM enabled.

So the archive claim that proper PM/light-sleep cannot be obtained in the current stock PlatformIO Arduino setup is valid.

## 3. A newer framework would provide more real control

Also correct.

A newer framework/build path can unlock:

- real PM / auto light-sleep
- real tickless idle
- compile-time Bluetooth removal
- compile-time PHY TX capping
- proper application of `sdkconfig`

So the archive instinct that "a newer framework might bring benefits" is reasonable.

---

## What the archive notes get wrong or miss

## 1. "Release BT memory" is presented as missing, but it is already implemented

This suggestion is outdated relative to the codebase.

The firmware already calls:

- `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)`

So this is not a new optimisation opportunity.

## 2. "Enable modem sleep" is presented as missing, but it is already implemented

This is also already implemented.

The project already exposes configurable WiFi power-saving modes and applies them after association.

So this suggestion is not a new action item.

## 3. "Lower TX power" is mostly already implemented

The project already:

- exposes TX power as config
- applies it before association
- reapplies it after connection
- has a brownout-recovery boot that forces the lowest setting

The only remaining question is not **whether** TX power limiting should exist.
It already exists.

The real remaining question is:

- whether the defaults should be made even more conservative for specific affected hardware
- whether the REF-ID-dependent problem set justifies different recommended defaults or a dedicated low-power profile

## 4. "Turn WiFi off between uploads" is not a drop-in optimisation here

This is the biggest practical mismatch between the archive advice and the actual product.

### Why it is attractive in theory

Yes, fully shutting WiFi off can dramatically reduce idle current.

### Why it is problematic here

In this project, WiFi is not only used for upload bursts. It is also part of the product experience and recovery model.

Turning WiFi off by default would change or degrade:

- web UI availability
- OTA access
- live logs / SSE
- quick troubleshooting access
- NTP availability patterns
- local network discovery flow
- recovery behavior after transient network failures

### Why it can also worsen spikes

This is not just a UX concern.

For a power-constrained CPAP source, repeated full reconnects may be worse than staying associated in a low-power modem-sleep state, because reconnect involves:

- active scanning
- association
- DHCP activity
- possible RF calibration bursts
- possible TLS reconnect churn soon after

That means WiFi-off idle is **not** a generally safe recommendation for this project.

### Verdict

- **Not recommended as the default behavior**
- could only be considered as a separate opt-in operating mode with explicit product tradeoffs

## 5. "Add arbitrary delays between operations" is weak advice

The archive suggestion to "spread WiFi operations" by adding delays is too generic.

In a system like this:

- arbitrary delays can extend the total time spent awake
- longer awake windows can increase total energy use
- delays do not reliably reduce the peak of association/TX spikes

Targeted sequencing already implemented in this firmware is the kind of delay that makes sense:

- keep CPU at 80 MHz through WiFi startup
- delay mDNS start slightly after WiFi comes up
- stop mDNS after 60 seconds

Those are purposeful.

Generic "insert more delays" advice is not strong enough to recommend on its own.

## 6. The PHY-rate advice is especially problematic

The later archive suggestion proposes using:

- `esp_wifi_internal_set_fix_rate(...)`
- `WIFI_PHY_RATE_11M`

This is a poor fit here for multiple reasons.

### Problem A: it conflicts with the current design

The firmware already disables `802.11b` to avoid the older legacy DSSS/CCK behavior.

Forcing `11M` is effectively steering the device back toward legacy 802.11b-class operation, which cuts against the current strategy.

### Problem B: it uses a private/internal API

`esp_wifi_internal_set_fix_rate(...)` is an internal/private API, not a robust public application API.

That means:

- it is less stable across framework versions
- it may change or disappear
- it is not a good foundation for a production mitigation unless the gain is clearly proven

### Problem C: longer airtime may offset any claimed benefit

Even if fixed lower rates reduce some instantaneous peaks, they also increase airtime and can increase:

- association fragility
n- time-on-air
- contention exposure
- total upload duration

For this project, which can push real uploads and not just tiny telemetry packets, the "5 KB example" in the archive notes is not representative enough.

### Verdict

- **Do not recommend fixed PHY rate as a next optimisation step**
- especially **do not recommend the private/internal API route** without hard measurements showing clear benefit on the affected REF IDs

## 7. The archive understates what the project already does for brownout mitigation

The archive text largely treats the system like a generic ESP32 app.

It misses several project-specific mitigations already present, including:

- early CPU downclock before most boot work
- TX power reduction before association
- protocol restriction to `11g/n`
- timed mDNS shutdown
- brownout-recovery degraded mode
- relaxed brownout detector during connect/reconnect

So while parts of the advice are directionally correct, they are not based on a full reading of the current codebase.

---

## Where the original PM conclusion was too strong

The earlier conclusion that PM is "low impact" is too absolute.

A more accurate statement is:

- PM will **not** eliminate the main WiFi association / TX spike problem by itself
- but PM **can** lower the baseline enough to buy extra headroom on marginal power sources

That matters because the total instantaneous draw seen by the CPAP source is:

- baseline system draw
n- plus transient radio / SD / CPU spikes

Reducing baseline does not remove the spike source, but it can still help if the CPAP power path is close to its limit.

So:

- **PM is not a cure-all**
- **PM is not irrelevant either**

The realistic expectation is:

- modest but real headroom improvement
- likely most noticeable in idle/listening/cooldown periods
- possibly some indirect improvement in stability on borderline REF IDs
- unlikely to fully fix the worst affected units by itself

---

## Framework migration options

## Current state

The project currently uses stock PlatformIO Arduino 2.x.

That is the safest path for compatibility with the existing codebase, but it blocks many of the compile-time levers you want.

## Option A: Stay on current stock PlatformIO Arduino 2.x

### Pros

- lowest migration risk
- known behavior
- no build-system rewrite
- existing custom `rebuild_mbedtls.py` already compensates for part of the stock-framework limitation

### Cons

- no real PM
- no real tickless idle
- no compile-time BT disable
- no compile-time PHY TX cap
- many `sdkconfig.defaults` entries remain ineffective

### Verdict

Best short-term stability, but limited further software-only headroom.

## Option B: Migrate to `pioarduino` stable (Arduino 3.x / IDF 5.5.x)

### What it gives you

- Arduino 3.x
- newer Espressif core and IDF base
- `custom_sdkconfig` / hybrid compile support
- a PlatformIO-like workflow
- likely the easiest path to getting PM/tickless/BT-off/PHY-cap working without rewriting the app around raw ESP-IDF

### Pros

- least disruptive route to modern framework capabilities
- likely enough to answer the practical question: "does enabling real PM materially help these REF IDs?"
- keeps project structure closer to current PlatformIO Arduino usage

### Cons

- community-maintained, not vendor-official PlatformIO integration
- migration from Arduino 2.x to 3.x is still non-trivial
- API and behavior changes may affect WiFi, timers, peripherals, and libraries
- you still need a careful compatibility and regression pass

### Verdict

This is the **best experimental migration path** if the goal is to quickly test whether a newer framework yields meaningful power gains without immediately re-architecting the project.

## Option C: ESP-IDF 5.5 + Arduino as an official Espressif component

Espressif documents this path officially and currently supports Arduino `3.3.7` as an ESP-IDF component.

### What it gives you

- the most official/vendor-backed modern path
- full ESP-IDF configuration control
- full `menuconfig` / real `sdkconfig` behavior
- no need to depend on community PlatformIO integration for Arduino 3.x

### Pros

- most future-correct architecture
- maximum control over WiFi/PM/FreeRTOS configuration
- best long-term answer if this product needs serious platform-level tuning

### Cons

- highest engineering effort
- build system and project structure changes are much larger
- more migration work than `pioarduino`
- not the fastest way to answer the short-term question of whether PM helps enough to matter

### Verdict

This is the **best long-term strategic path** if you decide that the project now genuinely needs ESP-IDF-level control as a first-class requirement.

---

## Which migration path should be preferred?

That depends on the question being asked.

### If the question is:

"Can a newer framework materially improve power behavior on the problematic REF IDs?"

Then the best answer is:

- **trial `pioarduino` first**

Reason:

- lower migration cost
- faster time to data
- sufficient to validate PM / tickless / compile-time WiFi settings

### If the question is:

"What should the project eventually standardize on if deep platform control becomes a permanent requirement?"

Then the best answer is:

- **ESP-IDF + Arduino component**

Reason:

- official Espressif-documented path
- strongest long-term control
- most sustainable if platform tuning becomes central to the project

---

## Suggestions from the archive: final verdict

## Good / correct

- stock Arduino core ignores most `sdkconfig.defaults`
- current stock framework prevents real PM
- newer framework could bring meaningful configurability
- hardware-side bulk capacitance is plausible as a mitigation for weak REF IDs

## Correct but already implemented

- lower CPU speed
- lower TX power
- enable modem sleep
- release BT memory

## Partly correct, but overstated

- PM is irrelevant to brownout prevention
- arbitrary delays help
- simply lowering rates or lower-level WiFi tuning is obviously a win

## Not recommended in current architecture

- turn WiFi off between uploads as a default behavior
- rely on fixed PHY-rate private APIs as a mitigation

## Possibly worth exploring, but only as advanced/opt-in ideas

- pinning AP channel / BSSID to reduce scan spikes
- REF-specific recommended defaults
- documenting external capacitor / hardware-buffering options for users with the worst REF IDs

---

## Practical next recommendations

## 1. Do not treat the archive notes as an implementation plan

They are useful as prompts, but not as code-ready advice.

## 2. Keep the current runtime mitigations as the baseline

The existing firmware already contains the strongest practical mitigations available in the current stock environment.

## 3. Do not default to WiFi-off idle behavior

That would change the product more than it improves it, and it risks replacing one power problem with another.

## 4. Do not adopt fixed PHY-rate private APIs as a quick win

The API choice is fragile, and the proposed `11M` direction conflicts with the current `11b`-disabled strategy.

## 5. If you want real further software gains, test a newer framework

The most sensible staged plan is:

### Stage 1: experimental branch

Try a modern framework branch to answer one question only:

- how much measurable power/stability improvement do we actually get?

### Stage 2: choose migration path based on results

- If the gain is marginal: stay where you are
- If the gain is meaningful and migration pain is acceptable:
  - use `pioarduino` if you want minimal code churn
  - use ESP-IDF + Arduino component if you want the most official long-term architecture

## 6. Judge success using measurements, not theory

The affected REF-ID problem is hardware-specific enough that theoretical forum advice is not sufficient.

The right evaluation criteria are:

- boot-time current spike
- association spike
- idle/listening draw
- cooldown draw
- upload-time draw
- stability on the worst REF ID
- stability on the minor-issue REF ID
- regression risk on known-good units

---

## Final recommendation

My recommendation is:

- **Do not make further power changes based directly on `31-A-BUNCH-OF-ADVICES.md`**
- **Do consider a newer framework, because it is one of the few remaining paths to real additional software control**
- **If you want a practical near-term experiment, try `pioarduino` first**
- **If you want the strongest long-term foundation, aim for ESP-IDF + Arduino component**

Most importantly:

- expect a newer framework to deliver **incremental headroom and configurability**, not a guaranteed cure for the worst affected REF IDs
- the worst brownout cases may still require hardware mitigation or REF-specific operating recommendations even after a framework upgrade

---

## Decision summary

### Not worth pursuing immediately

- WiFi fully off between uploads as default behavior
- fixed PHY rate via private WiFi APIs
- generic delay insertion as a power strategy

### Worth pursuing only with measurements

- newer framework with real PM enabled
- compile-time BT disable
- compile-time PHY TX cap
- tickless idle / auto light-sleep
- optional AP channel/BSSID pinning

### Most likely best next research action

Create a framework-upgrade test branch and compare:

- current stock PlatformIO Arduino 2.x
- `pioarduino` Arduino 3.x with real `custom_sdkconfig`

If that branch shows meaningful gains on the worst REF ID without breaking core functionality, then decide whether to:

- stay on `pioarduino`, or
- invest in a full ESP-IDF + Arduino component migration
