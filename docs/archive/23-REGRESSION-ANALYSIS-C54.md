# Regression Analysis for Recent `docs/22` Changes — C54

## Scope

This document analyzes the regressions reported after implementing the last batch of changes from `docs/22-FURTHER-FINDINGS-REVIEW-CO46.md`.

This is an analysis-only document.

**No firmware code changes are made here.**

---

## Executive Summary

There are three distinct issues:

1. **Regression 1 is real and high-severity.**
   The new ULP-based CS_SENSE path is the most likely cause of false "bus silence confirmed" decisions during active therapy.

2. **Regression 2 is also real.**
   The logging UX now strongly prefers a very expensive fallback path: when SSE drops, the web UI requests `/api/logs/full`, which streams persisted internal-flash logs plus the current RAM log buffer. This is much heavier than either live SSE or a small polling tail.

3. **Regression 3 is partly a regression and partly a policy question.**
   Raising the brownout threshold did **not** create brownout resets from nothing — the old logs show brownout detection was already active before the latest change. However, raising the threshold to level 7 can increase reset frequency on marginal hardware because it trips earlier. Lowering the threshold is safer than disabling brownout entirely, but both reduce data-integrity protection.

My overall recommendation is:

- **First priority:** remove the ULP path from any decision that grants SD access.
- **Second priority:** restore SSE-first logging behavior and stop using `/api/logs/full` as the default reconnect path.
- **Third priority:** keep brownout detection enabled, but consider a user-selectable compatibility tier for weaker CPAP machines rather than a one-size-fits-all level 7 policy.

---

## Regression 1: False "62s of bus silence confirmed" During Active Therapy

## Observed Behavior

The regression log shows a repeating pattern during therapy:

- LISTENING runs for ~62 seconds
- The firmware declares bus silence
- It acquires the card
- Pre-flight finds no work
- It releases the card after ~1.8 seconds
- It waits 10 minutes and repeats

This is consistent across many cycles in `regression.txt.tmp`.

That pattern strongly suggests the firmware is **blind to real card activity** while therapy is running.

## What Changed

The recent changes introduced `UlpMonitor` and started it in normal runtime:

- `src/main.cpp`
  - initializes `ulpMonitor.begin()` during setup
  - restarts it in `handleReleasing()`
- `src/UlpMonitor.cpp`
  - reconfigures GPIO 33 as RTC GPIO input
  - enables RTC GPIO hold
  - samples the pin at 10 Hz

At the same time, LISTENING still decides bus silence using the existing PCNT-based `TrafficMonitor` path:

- `src/main.cpp`, `handleListening()` uses `trafficMonitor.isIdleFor(...)`
- `src/main.cpp`, loop calls `trafficMonitor.update()` during LISTENING

## Key Technical Findings

### 1. The ULP result is not actually used in the silence decision

`UlpMonitor::checkActivity()` and `UlpMonitor::getActivityCount()` exist, but they are not called anywhere outside `UlpMonitor.cpp`.

So the ULP monitor is **not** currently part of the logic that decides whether the bus is idle.

That means the current design is not:

- "PCNT replaced by ULP"
- and not even "PCNT confirmed by ULP"

Instead it is effectively:

- **PCNT still decides idle/busy**
- while **ULP separately reconfigures the same GPIO**

That is a dangerous combination.

### 2. The ULP code reconfigures the same GPIO that PCNT depends on

`TrafficMonitor.begin()` configures GPIO 33 for PCNT edge counting.

Then `UlpMonitor.begin()` does all of the following to that same pin:

- `rtc_gpio_init((gpio_num_t)CS_SENSE)`
- `rtc_gpio_set_direction(..., RTC_GPIO_MODE_INPUT_ONLY)`
- `rtc_gpio_hold_en(...)`

Later `UlpMonitor.stop()` deinitializes the RTC GPIO and converts it back to a normal GPIO input.

Even though PCNT was originally configured earlier, the ULP path is now mutating the pad configuration of the same pin behind PCNT's back.

That makes it highly plausible that the proven PCNT-based detector is no longer seeing the edges it used to see.

### 3. Even if wired in, the current ULP algorithm is not robust for MHz SD traffic

The ULP program samples GPIO 33 every 100 ms and only checks whether the pin's **level** changed between samples.

That is not the same as counting edges.

For a high-speed SD signal, this is an aliasing problem:

- the line may toggle many thousands of times between two ULP samples
- if the line happens to be at the same logic level at both sample instants, the ULP sees **no change**
- therefore the current ULP program can miss real bus activity entirely

The original PCNT design avoided this by counting both rising and falling edges in hardware.

So even a fully integrated version of this exact ULP algorithm would still be risky.

### 4. The regression log fits a detector-blindness failure, not a real-idle scenario

The repeated cadence is too regular:

- cooldown completes
- ~62 seconds later LISTENING says silence confirmed
- pre-flight immediately finds no work
- release after ~1.8 seconds
- repeat

That looks like the access gate is no longer seeing real therapy traffic, not like the CPAP is repeatedly going fully idle every 10 minutes.

## Conclusion on Regression 1

**Regression 1 is very likely caused by the new ULP integration.**

More precisely:

- the ULP monitor is currently **not integrated into the decision path**
- but it **does** reconfigure the same GPIO used by the decision path
- and its own sampling strategy would be unreliable for SD bus activity anyway

So this is not a minor tuning issue. It is a design-level regression in how activity detection now works.

## Recommended Fix Approach

### Immediate recommendation

Use the original PCNT-only `TrafficMonitor` path for all LISTENING / Smart Wait decisions again.

That is the lowest-risk recovery path because:

- it was already working in real therapy conditions
- it counts edges in hardware
- it does not require ULP sampling assumptions

### Recommended handling of the current ULP work

Do **not** let the current ULP design decide card safety.

If ULP is revisited later, it should be treated as a separate experimental project, not as a live gate for SD takeover.

### If ULP is ever retried

A future ULP design would need all of the following:

- explicit integration into the state machine
- no conflict with PCNT on the same pin at the same time
- a detection strategy that does not rely on 10 Hz level snapshots of a MHz signal
- evidence from bench logs that it detects active therapy reliably

Until then, PCNT should remain the authoritative detector.

---

## Regression 2: SSE Logging Became Much Worse After Disconnects

## Reported Problem

The user reports that after SSE is established, the connection now gets lost relatively quickly, and the UI then falls back to pulling a very large log history instead of only a small recent tail.

That is a valid concern.

The important point is not just that SSE disconnects happen.
The bigger problem is that the current reconnect path chooses the **most expensive** recovery strategy by default.

## What the Current Code Does

### Server side

In `src/CpapWebServer.cpp`:

- `/api/logs/stream` establishes the SSE socket
- `pushSseLogs()` only pushes when new log bytes exist
- SSE push is suppressed entirely when:
  - `uploadTaskRunning == true`
  - or `g_brownoutRecoveryBoot == true`

This happens in `src/main.cpp`:

```cpp
if (!uploadTaskRunning && !g_brownoutRecoveryBoot) {
    pushSseLogs();
}
```

### UI side

In `include/web_ui.h`:

- `startSse()` creates an `EventSource('/api/logs/stream')`
- on `sseSource.onerror`, it does this:
  - closes SSE
  - sets `backfillDone=false`
  - calls `fetchBackfill()` after 3 seconds

`fetchBackfill()` then requests `/api/logs/full`.

### Why `/api/logs/full` is expensive

In `handleApiLogsFull()` the server does all of the following:

- flushes the circular buffer to internal flash
- streams all saved internal-flash log files
- appends the current RAM log buffer

So `/api/logs/full` is not just a small tail request.
It is the heavy "give me the full history" path.

## Why This Is Worse Than Before

### 1. Ordinary SSE loss is treated like a reboot-history recovery event

The UI currently uses the full-history backfill path for normal SSE disconnects.

That is too aggressive.

A transient SSE drop should not automatically mean:

- read all persisted logs from internal flash
- send them over WiFi again
- rebuild the browser-side log view from scratch

### 2. SSE is intentionally starved during uploads and brownout-recovery mode

Because `pushSseLogs()` is skipped during upload and brownout-recovery mode, the browser may see a silent SSE connection for a while.

Also, even outside uploads, the current SSE implementation does not send periodic heartbeat comments when there are no logs.

That makes disconnects more likely with:

- browser timeout behavior
- AP/proxy idle handling
- WiFi sleep interactions

### 3. The reconnect strategy defeats the original power-saving goal

The intended optimization was:

- prefer SSE for lightweight incremental logs
- avoid unnecessary network churn during sensitive phases

But the current reconnect behavior can be worse than old polling because a single SSE drop causes a much larger transfer.

So the regression is not only UX-related; it also undermines the power-saving rationale.

## Important Clarification About "128KB"

In the current build configuration, the RAM circular log buffer is set to 4 KB via `platformio.ini`.

However, `/api/logs/full` also streams **persisted LittleFS log files**, so the total transfer can be far larger than the RAM buffer alone.

So the user's complaint is directionally correct even if the expensive part is not purely RAM.
The fallback path is still much heavier than SSE or a short tail poll.

## Conclusion on Regression 2

**Regression 2 is real and architectural.**

The main problem is not merely that SSE disconnects happen.
The main problem is that the UI interprets ordinary SSE loss as a reason to fetch complete log history.

## Recommended Fix Approach

### Preferred behavior order

The logging priority should be:

1. **SSE live stream**
2. **small tail polling**
3. **full-history backfill only when explicitly needed**

That matches the user's stated preference and is also the best choice for power.

### Recommended reconnect logic

On SSE loss:

- first try to reconnect SSE directly
- if SSE cannot be re-established, fall back to `/api/logs` tail polling
- do **not** request `/api/logs/full` unless there is strong evidence that full history is required

### When `/api/logs/full` should be used

Use full-history backfill only for cases like:

- first opening the Logs tab
- confirmed reboot / boot-banner recovery case
- explicit manual "load history" action

Not for routine SSE reconnects.

### Additional hardening that would help

If SSE remains the preferred transport, it should get either:

- periodic heartbeat comments / keepalive frames
- or a UI state that distinguishes "temporarily paused" from "fully disconnected"

Especially during uploads, if live log data is intentionally reduced, the connection should ideally stay open in a lightweight way rather than forcing the browser into the heavy fallback path.

---

## Regression 3: Brownout Threshold, Reboots, and Whether Lowering or Disabling BOD Is Better

## What the Latest Change Actually Did

The recent change set `CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_7=y` in `sdkconfig.defaults`.

That means the hardware brownout detector is still enabled, but it now trips at a **higher voltage threshold** than before.

This does **not** create a new kind of reset.
It changes **when** the reset occurs.

## What the Older Logs Prove

The older `v1.0i-beta1` log already shows:

- `Reset reason: Brown-out reset (low voltage)`

That means brownout detection was already active before the latest change.

So the latest brownout-related change did **not** introduce the idea of rebooting on brownout.
It only made the device more willing to reset earlier in a sag.

## What Raising the Threshold Changes in Practice

### Higher threshold

- resets sooner
- protects SD / flash better
- may cause more resets on weak power sources

### Lower threshold

- resets later
- may reduce reset frequency
- allows more operation at low voltage
- increases risk of corruption or undefined behavior

### Disabled brownout detector

- may reduce or eliminate forced resets
- but allows the ESP32 and attached storage to continue operating deeper into undervoltage conditions
- gives up the hardware fail-fast safety net entirely

## Direct Answer to the User's Question

### "Will the system forcibly reboot itself if a brownout is detected?"

Yes.

That is exactly what the ESP32 hardware brownout detector is supposed to do.

### "Could disabling brownout make the ESP work?"

Possibly in the narrow sense of **appearing to stay running longer**.

But that does **not** mean it is truly safe or correct.
Without brownout reset, the ESP may continue executing while:

- CPU logic is outside safe voltage margins
- SD card writes are unsafe
- flash / filesystem operations become unreliable
- WiFi and peripheral behavior becomes undefined

So disabling brownout can trade "visible reboot" for "invisible corruption or erratic behavior".

That is a bad default recommendation for a storage-heavy product.

## My Recommendation on Brownout Policy

### I do not recommend disabling brownout by default

For this project, disabling BOD globally would be too risky because the firmware:

- mounts SD cards
- reads and writes persistent state
- uses internal flash logging
- relies on network stacks that do not behave well under unstable power

### Lowering the threshold is safer than disabling it

If there must be a compatibility escape hatch for weak CPAP hardware, then:

- **lowering** the brownout threshold is preferable to **disabling** it
- because it preserves some fail-fast protection
- while still reducing the number of resets compared with level 7

### But lowering the threshold should be opt-in, not the default

For users with marginal CPAP power rails, a compatibility option may be reasonable.
But it should be explicit, because the tradeoff is real:

- fewer resets
- higher corruption risk

### Suggested brownout policy tiers

A reasonable policy would be:

- **Safe default:** higher threshold / strong protection
- **Compatibility mode:** lower threshold, still enabled
- **Experimental diagnostic mode only:** brownout fully disabled

I would only consider "brownout disabled" as a last-resort test mode for a user who explicitly prefers uptime over data integrity and understands the risks.

## Interaction With Brownout-Recovery Mode

One more side effect matters here:

When a brownout reset happens, the firmware now enters a degraded boot mode that:

- skips mDNS
- suppresses SSE
- forces lower TX power / stronger WiFi power saving

That means more brownout resets can indirectly worsen the web logging experience too, because each reset can place the device into a mode where live log streaming is intentionally reduced.

So brownout threshold policy and logging UX are now more tightly coupled than before.

---

## Recommended Remediation Order

## 1. Fix the SD safety regression first

The false LISTENING idle detection is the highest-risk issue because it can cause card takeovers during therapy.

Priority recommendation:

- remove the current ULP path from the SD access decision
- return to the known-good PCNT-only detector for LISTENING / Smart Wait

## 2. Restore logging behavior to SSE-first, tail-poll second, full-history last

Priority recommendation:

- treat SSE reconnects as lightweight reconnects
- do not use `/api/logs/full` as the default fallback for ordinary SSE loss
- reserve full backfill for initial load, confirmed reboot, or explicit user action

## 3. Revisit brownout threshold after the detector regression is removed

Do not evaluate brownout policy while the device is still falsely taking the card during therapy.

A bad activity detector can itself create extra stress and secondary failures, which can make the power picture look worse than it really is.

## 4. Offer a compatibility policy instead of a global rollback

If some CPAP models are clearly marginal on power, the better long-term approach is:

- keep the safer default for normal hardware
- provide a weaker brownout threshold option for problematic machines
- keep "brownout disabled" out of the default path

---

## Recommended Positions

## On ULP for CS_SENSE

**Recommendation:** do not rely on the current ULP implementation for therapy-time SD safety.

## On live logs

**Recommendation:** SSE should remain the preferred transport.

If SSE drops:

- reconnect SSE first
- fall back to tail polling if needed
- avoid full-history pulls unless truly necessary

## On brownout threshold

**Recommendation:**

- do not disable brownout by default
- if a compatibility concession is required, lower the threshold before considering disable
- treat full brownout disable as experimental only

---

## Final Assessment

The evidence points to the following:

- **Regression 1** is most likely a direct consequence of the ULP monitor change.
- **Regression 2** is a real reconnect-strategy regression in the web logging path.
- **Regression 3** is not proof that brownout support itself is new; it is evidence that the threshold increase may be too aggressive for some hardware, but disabling BOD entirely would be a poor default response.

If only one action is taken first, it should be:

**Restore the known-good PCNT-only activity detector for SD takeover decisions.**

That is the most urgent safety fix and the cleanest way to separate real power issues from detector-induced regressions.
