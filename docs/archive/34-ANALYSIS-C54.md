# Power Optimisation Analysis C54

## Scope

This document re-evaluates the conclusions in:

- `docs/archive/32-ANALYSIS-CO46.md`
- `docs/archive/33-ANALYSIS-G31.md`

with a stricter standard:

- prefer direct evidence from the current firmware and toolchain
- separate **measured / code-backed facts** from **plausible theories**
- reject suggestions that do not fit the actual product architecture
- assess whether a framework migration is justified, and what it would *really* buy

This is a research document only.

**No code changes are made here.**

---

## Executive Summary

### Bottom line

The current firmware already implements most of the practical **runtime** power mitigations that are compatible with the product architecture.

What remains blocked is mostly **compile-time** and **framework-level** optimisation:

- real ESP-IDF power management (`CONFIG_PM_ENABLE`)
- tickless idle / auto light-sleep
- compile-time Bluetooth removal
- compile-time PHY TX cap
- native mbedTLS buffer configuration

However, one important correction is needed to the previous reports:

**Even if you migrated to a framework that enables ESP-IDF PM today, the current firmware configuration would not give you large CPU DFS savings by itself.**

Why:

- in `src/main.cpp`, the PM config is currently set to:
  - `.max_freq_mhz = targetCpuMhz`
  - `.min_freq_mhz = 80`
  - `.light_sleep_enable = true`
- the default configured CPU speed is `80 MHz`
- therefore, with the current code shape, enabling PM would primarily unlock **auto light-sleep in selected states**, not a large drop from `80 MHz` to `10 MHz`

So the previous G31 conclusion that migration is automatically a **high-reward power fix because it buys ~25 mA baseline headroom** is **overstated** for this codebase as it exists today.

That does **not** mean migration is pointless.

A migration still has real value because it would unlock:

- actual light-sleep in low-power FSM states
- compile-time BT removal
- compile-time WiFi PHY TX cap during framework-controlled phases
- removal of the `rebuild_mbedtls.py` workaround
- direct control over FreeRTOS and ESP-IDF settings

But the likely brownout benefit is **incremental and conditional**, not guaranteed.

### Most likely practical ranking

1. **Hardware-side mitigation remains the strongest lever**
   - local bulk capacitance near the ESP32 power rail
   - any REF-ID-specific board/power-path differences are far more likely to explain “major vs minor vs no issues” than missing firmware tweaks alone

2. **Config-level mitigation for affected users is already available now**
   - lowest TX power
   - max modem sleep
   - 1-bit SD mode
   - relaxed brownout detector mode where appropriate

3. **Framework migration is reasonable, but should be justified as a broader platform upgrade**
   - not because PM alone is certain to solve the issue
   - but because it removes current framework ceilings and gives you a cleaner path to future power work

4. **Several suggestions from previous documents remain impractical**
   - turning WiFi off between uploads
   - burst-connect / burst-disconnect model
   - deep sleep between uploads
   - anything that breaks the always-on web UI / OTA / diagnostics model

---

## What CO46 Got Right

`32-ANALYSIS-CO46.md` is broadly correct on the major structural points.

### 1. The stock PlatformIO Arduino toolchain is the main compile-time blocker

This is directly supported by the current project files:

- `platformio.ini` uses `platform = espressif32`
- `framework = arduino`
- `board_build.sdkconfig = sdkconfig.defaults`
- the project also uses `scripts/rebuild_mbedtls.py`

The rebuild script itself explicitly states why it exists:

- stock Arduino-ESP32 ships with precompiled mbedTLS libraries
- `sdkconfig.defaults` is ignored for those libraries
- the script recompiles only the TLS portion as a workaround

This is strong evidence that the project is already working around framework limitations.

### 2. Runtime WiFi power mitigations are already implemented

The code confirms the following are already present:

- early TX power application before association in `WiFiManager::applyTxPowerEarly()`
- configurable TX power in `WiFiManager::applyPowerSettings()`
- modem sleep modes:
  - `WIFI_PS_MIN_MODEM`
  - `WIFI_PS_MAX_MODEM`
- `listen_interval = 10` for max power save
- `802.11b` disabled during connection
- brownout-recovery boot path forcing lowest TX power + max power save
- timed mDNS shutdown after 60 seconds

So a large portion of the advice in prior notes was either:

- already implemented, or
- partially implemented more carefully than the advice suggested

### 3. WiFi-off-between-uploads is a poor fit for this product

This remains correct.

The firmware is intentionally built as a network-connected appliance, not a burst-sensor:

- the web server runs continuously in the main loop
- SSE logs are pushed for live diagnostics
- OTA is integrated with the web server path
- the web UI is expected to be available without a deliberate reconnect workflow
- reconnects are already treated as risky/high-current events and are carefully guarded

Additional direct evidence from `main.cpp`:

- there is explicit reconnect logic when WiFi drops
- reconnects are guarded to avoid lwIP corruption during uploads
- the firmware relaxes brownout detection specifically around reconnect because reconnect is known to be a high-risk current event

That is the opposite of a design that benefits from frequent intentional WiFi teardown.

### 4. pioarduino is the most practical migration path

This remains correct.

Compared with moving to full ESP-IDF + Arduino as component:

- `pioarduino` keeps the PlatformIO + Arduino workflow intact
- it is much less invasive than a CMake / component-based migration
- it appears to be the best compromise between modern ESP-IDF control and project continuity

---

## What G31 Got Right

`33-ANALYSIS-G31.md` added one useful idea, but mixed it with overstatements.

### 1. The “total current budget” theory is plausible

The useful part of G31 is not the exact numbers. It is the framing.

If REF IDs map to different CPAP hardware revisions, regulators, cable losses, connector tolerances, or power-limited accessory ports, then the problem may not be purely:

- “one short transient spike causes a rail dip”

It may instead be:

- “some hardware variants have less current headroom, so the same transient becomes fatal on those units”

That theory is plausible and consistent with the observed pattern:

- some REF IDs: major issues
- others: minor issues
- others: no issues

It is reasonable to suspect **hardware-side margin differences**.

### 2. G31 was right to reject WiFi-off and burst-upload advice

That part fits the codebase and architecture.

### 3. G31 was right that migration could still be worthwhile

Yes — but the reasons need to be stated more carefully.

Migration is worthwhile because it would:

- remove present framework ceilings
- allow compile-time settings to take effect
- simplify TLS configuration
- unlock real PM/light-sleep support
- reduce dependence on framework hacks

Those are valid reasons.

---

## What G31 Overstated or Got Wrong

This is the most important section.

### 1. The claim that PM would buy a large CPU baseline drop in the current firmware shape

This is **not supported by the current code**.

In `src/main.cpp`, the PM config is:

```cpp
esp_pm_config_esp32_t pm_config = {
    .max_freq_mhz = targetCpuMhz,
    .min_freq_mhz = 80,
    .light_sleep_enable = true
};
```

And the default config in `Config.cpp` is:

- `cpuSpeedMhz(80)`

So with the code as it exists now:

- default `max_freq_mhz = 80`
- `min_freq_mhz = 80`
- therefore **DFS is effectively disabled**

That means the main benefit of PM, if enabled by a framework migration, would be:

- **auto light-sleep in states where the PM lock is released**

Not:

- a large CPU idle-frequency reduction from `80 MHz` to `10 MHz`

So the “~25 mA returned as headroom” argument in G31 is not grounded in the actual current firmware configuration.

### 2. The idea that migration is automatically a high-reward brownout fix

This is too strong.

A migration may help, but the size of the benefit depends on several things that are not proven yet:

- how much time the firmware really spends in states where light-sleep can engage
- whether affected REF IDs are failing due to transient sag, sustained port limit, or both
- how much of the problem is actually upstream of the ESP32 board (CPAP port / cable / connector / board regulator / layout)

So the more defensible conclusion is:

- **migration is a reasonable enabler of additional optimisation**
- **migration is not yet proven to be the fix**

### 3. The fixed PHY-rate recommendation is still weakly supported

G31 softened this versus earlier advice, but it still leaned too optimistic.

Forcing a fixed low WiFi PHY rate may reduce instantaneous RF current in some conditions, but it also:

- increases airtime
- may worsen total energy per transfer
- bypasses rate adaptation that may already be choosing efficient rates
- could affect reliability in noisy or marginal RSSI conditions

For this codebase, the safer conclusion is:

- **possible experiment**, not a recommendation
- only worth considering after measurement on affected REF IDs

### 4. The PM/light-sleep benefit is state-dependent, not universal

This matters because the PM lock policy is explicit in `transitionTo()`:

- PM lock released only in `IDLE` and `COOLDOWN`
- PM lock acquired in active states

So even under a migrated framework, light-sleep would only be expected in selected states.

It would **not** apply during:

- uploading
- monitoring
- SD card activity
- active network operations
- other states where the lock is held

That reduces the practical impact versus the broad framing in G31.

---

## Current Firmware Reality: What Is Already Optimised

The codebase already does a substantial amount of careful power work.

### WiFi / RF

Already implemented:

- TX power set **before** association
- configurable TX power from `-1 dBm` through the project’s capped maximum mapping
- modem sleep modes
- max-modem listen interval tuning
- brownout-recovery degraded WiFi settings
- 802.11b disabled
- mDNS automatically stopped after the initial discovery window

### CPU / PM scaffolding

Already implemented:

- early 80 MHz boot strategy
- PM lock creation and state-based acquire/release logic
- explicit loop yields
- GPIO wakeup configuration for the sleep path

But currently blocked by framework:

- actual PM support
- actual light-sleep
- actual tickless idle benefits

### SD / I/O

Already implemented:

- SD pin drive strength reduction
- optional 1-bit SD mode
- careful mount/release sequencing
- avoidance of obviously bad concurrency patterns like log-flush overlap during upload

### Brownout handling

Already implemented:

- boot-time brownout detection awareness
- configurable detector mode
- relaxed detector around high-risk WiFi connect/reconnect windows
- degraded recovery boot profile

This all supports the same conclusion:

**there is no obvious “missing easy win” left in the current runtime architecture.**

---

## Practical Brownout Hypotheses, Ranked by Confidence

### Hypothesis A: Local rail sag / transient margin problem

**Confidence: high**

Why:

- brownout protection handling is already centered around connect/reconnect phases
- TX power is already reduced early
- SD drive strength has already been reduced
- the project comments and logic consistently treat WiFi association / RF activity as the riskiest electrical phase
- hardware-side differences between REF IDs naturally explain why some units fail and others do not

### Hypothesis B: Some REF IDs have lower total current headroom, so average draw matters more

**Confidence: medium**

Plausible, but not proven.

This is where G31 had a useful insight.

However, the evidence supports only this weaker claim:

- lower idle / baseline draw could improve margin on weak hardware

It does **not** yet support the stronger claim:

- enabling PM in the current firmware shape would definitely provide a large enough current reduction to materially solve the issue

### Hypothesis C: Missing software tweaks alone are the root cause

**Confidence: low**

The code already contains the main software-side mitigations you would expect.

That strongly suggests the remaining failures are mostly about:

- hardware margin
- platform/framework ceilings
- or REF-ID-specific electrical differences

not about an obvious absent runtime optimisation.

---

## Framework Options, Re-evaluated

## Option 1: Stay on Current Arduino Framework

### What you keep

- the known-working stack
- current upload behavior
- no migration risk

### What you cannot get

- real PM / light-sleep
- compile-time BT removal
- compile-time PHY TX cap via Kconfig
- clean application of most `sdkconfig.defaults`

### Best use case

Stay here if the goal is:

- short-term stability
- field mitigation via config and hardware guidance
- no appetite for platform risk right now

### Brownout outlook

This path can still be viable if:

- affected users are helped enough by current config options plus hardware-side fixes

---

## Option 2: Migrate to pioarduino

### What it definitely buys

- `sdkconfig.defaults` becomes meaningful
- `CONFIG_PM_ENABLE` can work
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE` can work
- `CONFIG_BT_ENABLED=n` can work
- `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=10` can work
- native mbedTLS config becomes possible without the custom rebuild path

### What it does **not** automatically buy

- a big CPU DFS drop below `80 MHz` in the current firmware setup
- guaranteed elimination of brownouts
- guaranteed compatibility without regression testing

### Likely real power benefit in the current code shape

Most likely gains:

- lower idle draw in `IDLE` / `COOLDOWN` due to real light-sleep
- some cleanup of baseline overhead via BT removal and tickless idle
- better compile-time control during phases that runtime API calls do not fully cover

Less likely / overstated gains:

- dramatic CPU-frequency-based current reduction under the present `min_freq_mhz = 80` policy

### Migration risk

Still moderate.

The main reasons are not massive API rewrites; they are validation burden:

- WiFi behavior
- power-save behavior
- SD handoff behavior
- TLS / OTA behavior
- PCNT legacy-driver future compatibility
- full upload FSM regression

### Best use case

This is the best migration path if you want:

- a practical platform upgrade
- better long-term maintainability
- real access to ESP-IDF features without abandoning Arduino

---

## Option 3: Official ESP-IDF + Arduino as Component

### What it buys

- strongest vendor-backed path
- full official ESP-IDF control
- maximum Kconfig flexibility

### Why it is still not the best immediate recommendation

For this project it is much heavier than the likely power benefit justifies right now:

- different build model
- more integration churn
- more validation surface
- less incremental than pioarduino

### Best use case

Choose this only if the project is already planning a broader platform re-architecture for long-term reasons.

For power optimisation alone, this is too expensive as a first step.

---

## What Is Actually Practical to Recommend

### 1. Keep the current architecture assumptions

Do **not** recommend:

- WiFi off between uploads
- deep sleep between uploads
- burst-only connectivity

Those ideas conflict with the codebase and product model.

### 2. Use configuration guidance more aggressively for affected REF IDs

This is the most practical immediate step because these controls already exist.

For users with the worst brownout susceptibility, the most conservative profile is:

- lowest TX power
- max WiFi modem sleep
- 1-bit SD mode
- relaxed brownout mode if needed

This does not require framework migration.

### 3. Treat framework migration as a platform improvement, not a silver bullet

Migrating to `pioarduino` is reasonable if you want to unlock:

- actual PM/light-sleep
- compile-time power settings
- cleaner TLS configuration
- less build-system hackery

But it should be sold internally as:

- **moderate-risk, moderate-reward, strategically useful**

not as:

- **certain cure for REF-ID-specific brownouts**

### 4. Prioritise hardware and REF-ID correlation work

The pattern by REF ID is one of the strongest clues in the whole problem statement.

That strongly suggests the next research step should be:

- map affected REF IDs to board/power differences if possible
- compare current draw / reset behavior across at least one representative device from each REF-ID cluster
- identify whether “major issue” units are failing at:
  - connect/association
  - upload start
  - sustained upload
  - reconnect
  - idle background operation

This is more likely to produce a decisive answer than another round of generic ESP32 power advice.

---

## Recommendation

### Recommended strategy

#### Phase 1 — Immediate / no framework change

Use and document the most conservative existing runtime settings for affected REF IDs.

That is the fastest low-risk mitigation path because these levers already exist in the firmware.

#### Phase 2 — Hardware-first support guidance

Document a hardware-side mitigation path for the worst-affected units.

Given the nature of the failures and the REF-ID spread, hardware margin remains the most likely dominant variable.

#### Phase 3 — Controlled pioarduino migration branch

If you want to pursue a newer framework, `pioarduino` is still the right first candidate.

But the expected benefit should be framed accurately:

- real PM/light-sleep support in low-power states
- compile-time config control
- cleaner toolchain
- incremental brownout margin improvement
- not a guaranteed standalone fix

#### Phase 4 — Only consider official ESP-IDF migration if the project is already moving that way strategically

For this specific problem, it is too large a hammer.

---

## Final Verdict

### CO46

Mostly correct.

Its strongest conclusions still stand:

- runtime mitigations are already mature
- framework ceilings are real
- pioarduino is the practical migration path
- hardware mitigation remains highly important

### G31

Useful mainly for one idea:

- lower baseline draw can matter if REF-ID-specific hardware has less total current headroom

But G31 overreached when it implied that current-framework migration would, by itself and in the current code shape, unlock a large CPU-idle current reduction.

That is **not** what the present PM configuration would do.

### Best grounded conclusion

- The current codebase is already doing most of the practical runtime work.
- The remaining realistic gains are mostly framework-level or hardware-level.
- A move to `pioarduino` is justified if you want a cleaner, more capable platform.
- But the strongest immediate advice is still:
  - use the existing conservative config options for affected REF IDs
  - treat hardware margin and REF-ID-specific power-path differences as first-class suspects
  - view migration as an enabler, not a guaranteed cure
