# Consolidated Regression Analysis & Recommendations — CO46

This document consolidates, validates, and extends the findings from the two
preliminary reports (`docs/23-REGRESSION-ANALYSIS-C54.md` and
`docs/24-REGRESSION-ANALYSIS-G31.md`).  It also addresses several additional
questions and UI bugs raised after those reports were written.

**No firmware code changes are made in this document.**

---

## Table of Contents

1. [Validation of Previous Reports (C54 & G31)](#1-validation-of-previous-reports-c54--g31)
2. [Regression 1 — False Bus Silence (ULP Blinds PCNT)](#2-regression-1--false-bus-silence-ulp-blinds-pcnt)
3. [Regression 2 — SSE / Logging Degradation](#3-regression-2--sse--logging-degradation)
4. [Regression 3 — Brownout Threshold & Reboot Behaviour](#4-regression-3--brownout-threshold--reboot-behaviour)
5. [What Was the ESP Doing When the Brownout Hit?](#5-what-was-the-esp-doing-when-the-brownout-hit)
6. [Can ULP Do PCNT?](#6-can-ulp-do-pcnt)
7. [UI Bug — "REBOOTING" Shown Prematurely on Mobile Tab Resume](#7-ui-bug--rebooting-shown-prematurely-on-mobile-tab-resume)
8. [UI Bug — SSE Breaks on Tab Switch (Even Pre-Change)](#8-ui-bug--sse-breaks-on-tab-switch-even-pre-change)
9. [Brownout Config Policy — Level 0 Default + Optional Disable](#9-brownout-config-policy--level-0-default--optional-disable)
10. [Recommended Fix Plan (Priority Order)](#10-recommended-fix-plan-priority-order)

---

## 1. Validation of Previous Reports (C54 & G31)

Both reports are **substantially correct**.  Specific validations:

| Finding | C54 | G31 | Validated? |
|---|---|---|---|
| ULP reconfigures GPIO 33, blinding PCNT | ✓ | ✓ | **Yes** — confirmed by code inspection. `UlpMonitor::begin()` calls `rtc_gpio_init()` + `rtc_gpio_hold_en()` on the same pin PCNT is counting. |
| `checkActivity()` is never called by the FSM | ✓ | ✓ | **Yes** — grep confirms the only call sites are inside `UlpMonitor.cpp` itself and `UlpMonitor.h`. No FSM code reads the result. |
| SSE fallback triggers `/api/logs/full` on every reconnect | ✓ | ✓ | **Yes** — `web_ui.h` line 845: `sseSource.onerror` → `fetchBackfill()` → `/api/logs/full`. |
| Brownout was already happening at Level 0 before the change | ✓ | ✓ | **Yes** — user-provided `v1.0i-beta1` logs show `Reset reason: Brown-out reset`. |
| Default brownout level before the change was Level 0 (~2.43 V) | — | ✓ | **Yes** — confirmed by reading the framework's compiled `sdkconfig`: `CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_0=y`, `CONFIG_ESP32_BROWNOUT_DET_LVL=0`. |
| ULP cannot access the hardware PCNT peripheral | — | ✓ | **Yes** — see [Section 6](#6-can-ulp-do-pcnt) for detailed explanation. |

### Minor Corrections

- C54 mentioned "128 KB" for the log pull size.  The actual RAM circular buffer
  is **4 KB** (`-DLOG_BUFFER_SIZE=4096` in `platformio.ini`).  However,
  `/api/logs/full` also streams all **persisted LittleFS syslog rotation files**
  (up to 4 × 32 KB = 128 KB on a full rotation set), so the user's observation
  of a large transfer is correct — the expensive part is the flash read, not the
  RAM buffer.  G31 was more precise on this point.

---

## 2. Regression 1 — False Bus Silence (ULP Blinds PCNT)

### Root Cause (Confirmed)

When `UlpMonitor::begin()` runs during `setup()` (line 302 of `main.cpp`), it
calls:

```
rtc_gpio_init((gpio_num_t)CS_SENSE);        // reconfigure pad as RTC GPIO
rtc_gpio_set_direction(..., RTC_GPIO_MODE_INPUT_ONLY);
rtc_gpio_hold_en((gpio_num_t)CS_SENSE);     // lock RTC config across sleep
```

This **steals** GPIO 33 away from the digital GPIO matrix that the PCNT
peripheral is connected to.  From that point forward, PCNT sees zero edges.

The `handleListening()` function still uses `trafficMonitor.isIdleFor(...)`,
which reads the PCNT counter.  Since PCNT is now blind, it always reports idle.
After the configured inactivity timeout (62 seconds in this user's config), the
FSM declares silence and acquires the SD card.

### Evidence From the Regression Log

The pattern repeats with mechanical regularity from 18:03 to 23:36 — every
single cycle reports exactly "62s of bus silence confirmed", acquires the card,
finds nothing to do, releases it after ~1.8 s, and enters a 10-minute cooldown.
This is consistent with a detector that is permanently blind, not with real
intermittent CPAP idle periods.

### Recommended Fix

- **Remove the entire `UlpMonitor` class** and all references to it in
  `main.cpp` (`begin()`, `stop()`, `isRunning()` calls).
- **Rely exclusively on the proven PCNT-based `TrafficMonitor`** for all bus
  silence decisions.
- PCNT continues to count edges in hardware even when the main CPU enters
  FreeRTOS light sleep, so there is **no power-saving benefit lost** by
  removing the ULP path.

---

## 3. Regression 2 — SSE / Logging Degradation

### Root Cause (Confirmed)

There are **two separate contributing factors**:

#### Factor A: SSE is suppressed during brownout-recovery mode

In the main loop (`main.cpp` line 1027):

```cpp
if (!uploadTaskRunning && !g_brownoutRecoveryBoot) {
    pushSseLogs();
}
```

If the device has experienced a brownout reset, `g_brownoutRecoveryBoot = true`
and SSE push is **completely disabled** until a successful upload cycle clears
the flag.  On marginal hardware that brownouts frequently, this flag may be
permanently set, making SSE permanently dead.

#### Factor B: Any SSE drop triggers a full-history backfill

In `web_ui.h` (line 840-845), the SSE `onerror` handler:

```javascript
sseSource.onerror = function() {
    sseConnected = false;
    if (sseSource) { sseSource.close(); sseSource = null; }
    backfillDone = false;
    set('log-st', 'SSE lost — reconnecting…');
    setTimeout(function() {
        if (curTab === 'logs') { fetchBackfill(); }
        else { startLogPoll(); }
    }, 3000);
};
```

`fetchBackfill()` calls `/api/logs/full`, which:
1. Flushes the RAM buffer to internal flash
2. Streams **all** persisted syslog rotation files from LittleFS
3. Appends the current RAM buffer

This is the heavy "give me everything" path.  Using it as the default reconnect
strategy means every transient SSE drop causes a transfer that is orders of
magnitude larger than the few log lines that were actually missed.

### Recommended Fix

#### Priority order for log delivery (as the user requested)

1. **SSE live stream** — preferred, lowest overhead
2. **Polling `/api/logs`** — lightweight fallback (RAM buffer only, ~4 KB max)
3. **Full backfill `/api/logs/full`** — only on first Logs tab open or confirmed
   reboot detection

#### Specific changes needed

1. **SSE `onerror` handler**: On disconnect, attempt to re-establish SSE after a
   short delay.  If SSE cannot reconnect after 2-3 attempts, fall back to
   **polling** (`startLogPoll()` → `/api/logs`), **not** `fetchBackfill()`.
2. **Remove the `g_brownoutRecoveryBoot` gate on SSE push**.  SSE is the
   lightest log transport available — suppressing it forces the UI into much
   heavier alternatives.  If power savings are needed post-brownout, reduce SSE
   push frequency rather than disabling it entirely.
3. **Add SSE keepalive comments**.  Periodically send an SSE comment line
   (`: keepalive\n\n`) when there are no new log lines.  This prevents browser
   timeouts, proxy timeouts, and WiFi power-save idle disconnects.  A 15-second
   interval is typical.

---

## 4. Regression 3 — Brownout Threshold & Reboot Behaviour

### What Was the Brownout Level Before the Change?

| | Before (ESP-IDF default) | After (docs/22 change) |
|---|---|---|
| **Setting** | `CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_0=y` | `CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_7=y` |
| **Trip Voltage** | ~2.43 V | ~2.73 V |
| **Enabled** | Yes | Yes |

The ESP-IDF framework ships with brownout detection **enabled at Level 0** by
default.  The recent change raised it to Level 7.  This means the device now
triggers a reset ~300 mV sooner during a voltage sag.

### Does the System Forcibly Reboot on Brownout Detection?

**Yes.**  The ESP32's hardware brownout detector is a hard reset source.  When
the supply voltage drops below the configured threshold, the chip performs an
immediate hardware reset — equivalent to pulling the reset pin low.  There is no
software callback, no graceful shutdown, no opportunity to close files.  It is
instantaneous.

### Impact of Raising to Level 7

For a CPAP machine whose power supply is already marginal enough to sag below
2.43 V (as proven by the `v1.0i-beta1` logs), raising the threshold to 2.73 V
means:

- The device will reset **more frequently** because smaller sags now trigger it
- Each reset enters brownout-recovery mode, which **disables SSE** and forces
  lowest TX power
- The device may enter a **reset loop** if the sag is recurrent (e.g., during
  WiFi TX bursts or SD card mount surges)

### Recommendation

**Revert to Level 0 (~2.43 V).**  This restores the pre-change behaviour and
gives the maximum possible voltage headroom before a forced reset.

See [Section 9](#9-brownout-config-policy--level-0-default--optional-disable)
for the optional-disable configuration proposal.

---

## 5. What Was the ESP Doing When the Brownout Hit?

The `v1.0i-beta1` boot logs provided by the user show this sequence after each
brownout reset:

```
Reset reason: Brown-out reset (low voltage)
...
Waiting 15s for electrical stabilization...
Checking for CPAP SD card activity (Smart Wait)...
Smart Wait: 5000ms of bus silence — CPAP is idle
Boot delay complete, attempting SD card access...
SD card mounted successfully
Loading configuration...
Config file parsed successfully
...
Configuration loaded successfully
```

This tells us:

1. The brownout happened **during the previous boot's operation**, not during
   the current boot (the current boot completed successfully).
2. The device successfully booted, waited 15 seconds, mounted the card, loaded
   config, connected to WiFi, and started running.
3. Then at some later point **during normal operation** the voltage sagged
   below 2.43 V and the hardware reset fired.

### Most Likely Trigger Points

The highest instantaneous current draws in this firmware are:

| Activity | Typical Current Draw | Why It Spikes |
|---|---|---|
| **WiFi TX burst** | +180-350 mA above baseline | Radio PA at full power during packet transmission |
| **SD card mount (SDMMC init)** | +50-100 mA for ~200 ms | SD card's internal voltage regulator + flash init |
| **WiFi association + DHCP** | +200-300 mA for 2-5 s | Sustained TX/RX during 4-way handshake |
| **TLS handshake (mbedTLS)** | +40-60 mA (CPU) + WiFi TX | CPU-intensive crypto + network packets |
| **Simultaneous SD + WiFi** | Compound: up to 400+ mA | Worst case during upload phase |

For a CPAP machine with a marginal power supply, the **single most dangerous
moment** is a **WiFi TX burst** — especially during initial association or
during an upload.  The ESP32's radio PA draws 180-350 mA in short bursts
(microseconds to milliseconds).  If the CPAP's 3.3 V rail cannot deliver that
current without sagging below 2.43 V, a brownout reset occurs.

### What Could Be Optimised at That Single Point?

The most impactful single optimisation is **reducing WiFi TX power**.

The current firmware already supports configurable TX power via `WIFI_TX_POWER`
in `config.txt`.  If the user's CPAP machine is triggering brownouts, the single
most effective mitigation is:

- Set `WIFI_TX_POWER=LOW` (5 dBm) in `config.txt`
- This reduces peak TX current from ~350 mA to ~120 mA
- At typical indoor distances (same room), 5 dBm is usually sufficient

This is already implemented but is not the default.  Making it the default for
brownout-recovery mode is already done (line 490 of `main.cpp`), but the
problem is that this only kicks in **after** a brownout has already occurred and
been detected.  The very first boot on marginal hardware still runs at whatever
the user configured (default: MID / 8.5 dBm).

**Additional potential optimisations:**

- **Stagger SD mount and WiFi**: Currently `wifiManager.connectStation()` (line
  470) runs after SD card release (line 451).  This is already correct — they
  don't overlap.  But if WiFi reconnects happen during LISTENING while the card
  is mounted, both could draw simultaneously.
- **Add a bulk decoupling capacitor**: This is a hardware recommendation, not a
  firmware change.  A 470 µF–1000 µF electrolytic capacitor across the ESP32's
  3.3 V rail would absorb the short TX current spikes that cause the sag.  This
  is the most reliable fix for marginal power supplies.

---

## 6. Can ULP Do PCNT?

**No.  The ULP cannot reliably perform pulse counting for SD bus activity.**

### Hardware PCNT

The ESP32 has 8 hardware PCNT units.  These are peripherals in the **digital
domain** (APB bus).  The ULP coprocessor runs in the **RTC domain** and can only
access RTC peripherals and RTC slow memory.  It **cannot** read PCNT counter
registers.

### Software Pulse Counting on ULP

The ULP could theoretically poll the GPIO in a tight loop and count state
changes.  However:

- The ULP clock runs at **8 MHz**.  Each instruction takes 2-4 cycles.  The
  fastest possible poll rate is roughly **1-2 MHz**.
- The SD bus CS line toggles at **20-50 MHz** during active SDMMC transfers.
- By the Nyquist theorem, you need to sample at **≥2× the signal frequency**
  to avoid aliasing.  A 2 MHz sample rate for a 20 MHz signal misses **>90%**
  of the edges.
- Furthermore, running the ULP in a continuous tight loop draws **1-2 mA**
  continuously, which negates the power savings over keeping the main CPU in
  light sleep (~2-3 mA) with the hardware PCNT running.

### Current ULP Design's Specific Flaw

The existing `UlpMonitor` samples GPIO 33 only once every **100 ms** (10 Hz).
It checks whether the pin's **level** changed between two consecutive samples.

For SD bus traffic at 20 MHz, the probability that two samples taken 100 ms
apart happen to see different logic levels is essentially random — it depends
on whether the bus happens to be in a transaction at the exact microsecond of
each sample.  Most real SD bus activity consists of short bursts (a few ms)
separated by idle gaps.  A 10 Hz sampler will miss most of these bursts entirely.

### Conclusion

**PCNT is the only viable activity detection mechanism.**  The hardware PCNT
peripheral counts every edge at full bus speed (40+ MHz counter input) with zero
CPU involvement.  It works during FreeRTOS light sleep.  There is no ULP-based
alternative that can match this.

---

## 7. UI Bug — "REBOOTING" Shown Prematurely on Mobile Tab Resume

### The Bug

When a user leaves the web UI tab inactive on a mobile phone (e.g., switches to
another app), the browser suspends the tab.  JavaScript timers stop running.
When the user returns to the tab, all paused timers fire at once.  The 3-second
`pollStatus()` interval has accumulated missed cycles, so the first poll attempt
may fail (the WiFi connection to the ESP may need a moment to re-establish after
the phone's radio wakes up).

The current logic in `web_ui.h` (lines 508-515):

```javascript
.catch(function() {
    statusFailCount++;
    if (statusFailCount >= 2 || rebootExpected) {
        document.getElementById('reboot-overlay').style.display = 'block';
        seti('d-st', '<span class="badge bc">REBOOTING</span>');
    } else {
        set('d-st', 'Offline');
    }
});
```

- **First failed poll**: shows "Offline" — this is correct
- **Second failed poll (6 seconds later)**: shows "REBOOTING" overlay — **too
  aggressive**

On a mobile phone resuming from background, it is completely normal for the
first 1-2 network requests to fail while the radio reconnects.  Declaring
"REBOOTING" after only 6 seconds of unreachability is misleading.

### Why "REBOOTING" Gets Stuck

Once `renderStatus(d)` runs successfully, it resets `statusFailCount = 0` and
hides the overlay (line 393-395).  So the overlay **does** eventually disappear
when a poll succeeds.

However, if the overlay is shown and the user is looking at it, they may not
wait long enough for the next successful poll (3 seconds).  It *appears* stuck
because:

1. The user sees "REBOOTING" immediately on tab resume
2. The next poll is 3 seconds away
3. If that poll also fails (radio still waking), it stays on "REBOOTING"
4. The user may switch away again before recovery

### Recommended Fix

1. **Increase the failure threshold** before showing the reboot overlay.
   Instead of `statusFailCount >= 2` (6 seconds), use `statusFailCount >= 5`
   (15 seconds).  This gives the mobile radio ample time to reconnect.

2. **Add an intermediate "Offline" state with a visual indicator** that is
   clearly distinct from "REBOOTING".  For example:
   - 1-4 missed polls (3-12 seconds): show `Offline — reconnecting…` as a
     subtle status badge
   - 5+ missed polls (15+ seconds): show the "REBOOTING" overlay
   - `rebootExpected` flag: show "REBOOTING" immediately (only set by explicit
     user actions like "Soft Reboot" or "Save & Reboot")

3. **Consider using `document.visibilitychange` event** to detect tab resume
   and reset the failure counter, giving a fresh grace period after the user
   returns.

---

## 8. UI Bug — SSE Breaks on Tab Switch (Even Pre-Change)

### The Bug

The user reports that even in `v1.0i-beta1` (before the recent changes), SSE
would sometimes become inoperable after switching from the Logs tab to Dashboard
and back.

### Root Cause (Confirmed by Code Inspection)

In `web_ui.h` line 366, the `tab()` function:

```javascript
function tab(t) {
    ...
    if (t === 'logs') {
        if (!backfillDone) { fetchBackfill(); }
        else if (!sseConnected) { startLogPoll(); }
    } else {
        stopLogPoll();
        stopSse();    // ← THIS IS THE PROBLEM
    }
    ...
}
```

**Every time the user navigates away from the Logs tab, `stopSse()` is called**,
which closes the `EventSource` and sets `sseSource = null`.

When the user navigates back to the Logs tab, the condition
`!sseConnected` is true, so it calls `startLogPoll()` (polling fallback)
**instead of** `startSse()`.  SSE is only re-established through the
`fetchBackfill()` path (which is only triggered if `backfillDone` is false).

So the sequence is:

1. User opens Logs tab → `fetchBackfill()` runs → SSE starts after backfill
2. User clicks Dashboard → `stopSse()` kills SSE
3. User clicks Logs tab again → `backfillDone` is `true`, `sseConnected` is
   `false` → falls through to `startLogPoll()` → **polling, not SSE**
4. SSE is never re-established unless something sets `backfillDone = false`
   (which only happens on SSE `onerror` or reboot detection)

This is a **pre-existing bug** unrelated to the recent power-saving changes.

### Why SSE Should Be Preferred

The user is correct that SSE should be the number one preference.  SSE is:

- **Lowest overhead on the ESP**: no request/response cycle, just incremental
  push of new log bytes
- **Lowest latency**: logs appear in real-time
- **Lowest power**: no periodic HTTP requests waking the radio

Polling `/api/logs` every 4 seconds requires a full HTTP request-response cycle
each time, which wakes the WiFi radio, allocates server resources, and transfers
the entire RAM buffer even if nothing changed.

### Recommended Fix

1. **Do not call `stopSse()` on tab switch.**  SSE is cheap when idle (no data
   sent = no CPU/radio cost).  Keep the `EventSource` alive across all tabs.
   Only close it on explicit page unload or after a prolonged disconnect.

2. **When returning to the Logs tab**, if SSE is still connected, simply
   continue rendering.  If SSE has dropped (detected by `sseConnected` flag),
   attempt to restart SSE **first**, and only fall back to polling if SSE
   cannot be established.

3. **Revised tab switch logic:**
   ```
   if (t === 'logs') {
       if (!backfillDone) {
           fetchBackfill();  // first-ever open: get history, then start SSE
       } else if (!sseConnected) {
           startSse();       // SSE dropped: try SSE first, not polling
       }
       // else: SSE is running, nothing to do
   }
   // Remove: else { stopSse(); }  — let SSE live across tabs
   ```

4. **SSE keepalive** (as mentioned in Section 3): a periodic server-side
   comment frame prevents the browser from timing out the connection during
   quiet periods.

---

## 9. Brownout Config Policy — Level 0 Default + Optional Disable

### Is This a Good Idea?

**Yes, with caveats.**  This is a reasonable policy.

### Recommended Implementation

#### Default: Level 0 (lowest threshold, ~2.43 V), always enabled

- This is the ESP-IDF factory default
- It provides the maximum voltage headroom before a forced reset
- It still protects against catastrophic low-voltage operation
- It matches the behaviour users had in `v1.0i-beta1`

#### Optional: User override to disable brownout via `config.txt`

Add a new config key, e.g.:

```
BROWNOUT_DETECT=OFF
```

When this key is set:

- At early boot (before WiFi, before SD mount), call
  `esp_brownout_disable()` (available in ESP-IDF via
  `#include "soc/rtc_cntl_reg.h"` + direct register write)
- Log a prominent warning: `[WARN] Brownout detection DISABLED by user
  config — SD card corruption risk if voltage sags occur`

#### Why This Is Acceptable

- Some CPAP machines have power supplies that sag below 2.43 V transiently
  but recover quickly enough that the ESP32 does not actually malfunction
- For these users, constant brownout resets are worse than the theoretical
  risk of a brief undervoltage event
- The user is making an informed choice and accepting the risk
- The firmware can still log `[WARN]` on every boot so the user is always
  reminded

#### Why Full Disable Should Not Be the Default

- Without brownout detection, the ESP32 will continue executing code at
  voltages where silicon timing is unreliable (< 2.4 V)
- SD card FTL corruption is a real risk: a garbled write during a voltage
  sag can brick the filesystem
- LittleFS corruption on internal flash is also possible
- There is no software recovery from hardware-level data corruption

#### Implementation Note

The brownout detector is configured very early in the ESP-IDF boot process
(before `app_main`).  To disable it at runtime, you need to clear the
brownout detector enable bit in the RTC control register:

```c
#include "soc/rtc_cntl_reg.h"
REG_CLR_BIT(RTC_CNTL_BROWN_OUT_REG, RTC_CNTL_BROWN_OUT_ENA);
```

This must happen **after** the config is loaded (so the setting can be read
from `config.txt`), but should happen **as early as possible** after config
load to prevent a brownout reset during the WiFi association phase.

However, there is a chicken-and-egg problem: the config is on the SD card,
and the SD card must be mounted first.  By the time the config is read, the
highest-risk phase (SD mount) has already passed.  The main benefit of
disabling brownout would be preventing resets during WiFi TX bursts and
upload phases.

An alternative is to store the brownout preference in **NVS (flash)** so it
can be read before SD mount.  But this adds complexity.  The simpler approach
(disable after config load) is probably sufficient for most cases because the
WiFi phase is where most brownout resets occur on marginal hardware.

---

## 10. Recommended Fix Plan (Priority Order)

### Priority 1: Remove ULP Monitor (Regression 1 — Safety Critical)

**Impact**: Prevents false SD card takeover during active therapy.

- Delete `src/UlpMonitor.cpp` and `include/UlpMonitor.h`
- Remove all `ulpMonitor.*` calls from `main.cpp`
  - `setup()`: remove `ulpMonitor.begin()` call
  - `uploadTaskFunction()`: remove `ulpMonitor.stop()` call
  - `handleReleasing()`: remove `ulpMonitor.begin()` restart
- Remove `#include "UlpMonitor.h"` from `main.cpp`
- Remove `UlpMonitor ulpMonitor;` global declaration
- Remove ULP-related `#include` directives (`esp32/ulp.h`, `ulp_common.h`,
  `driver/rtc_io.h`, etc.) if they are not used elsewhere

### Priority 2: Revert Brownout Threshold to Level 0 (Regression 3)

**Impact**: Stops increased brownout reset frequency on marginal hardware.

- In `sdkconfig.defaults`, remove:
  ```
  CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_7=y
  ```
- Optionally add an explicit (redundant but clear):
  ```
  CONFIG_ESP32_BROWNOUT_DET_LVL_SEL_0=y
  ```
- Remove the brownout-recovery degraded mode (`g_brownoutRecoveryBoot`) or
  convert it to a lightweight advisory log rather than a mode that disables
  SSE

### Priority 3: Fix SSE Tab-Switch Bug (Pre-existing Bug)

**Impact**: SSE stays alive across tab switches; no unnecessary reconnects.

- In `web_ui.h`, `tab()` function: **remove `stopSse()` from the `else`
  branch**.  Let SSE live across all tabs.
- When returning to Logs tab: if SSE is not connected, call `startSse()`
  directly instead of `startLogPoll()`.

### Priority 4: Fix SSE Reconnect Strategy (Regression 2)

**Impact**: SSE drops no longer trigger expensive full-history pulls.

- In `web_ui.h`, `sseSource.onerror` handler:
  - First 2-3 attempts: try to re-establish SSE directly
  - If SSE cannot reconnect: fall back to polling (`startLogPoll()`)
  - Do **not** call `fetchBackfill()` unless a reboot is explicitly detected
- Remove the `g_brownoutRecoveryBoot` gate on `pushSseLogs()` — SSE should
  always be available
- Add server-side SSE keepalive (`: keepalive\n\n` every 15 seconds)

### Priority 5: Fix "REBOOTING" UI Threshold (UI Bug)

**Impact**: Mobile users no longer see false "REBOOTING" on tab resume.

- Increase `statusFailCount` threshold from `>= 2` to `>= 5` before
  showing the reboot overlay
- Add `visibilitychange` listener to reset `statusFailCount` when the tab
  becomes visible again
- Consider a distinct "Offline — reconnecting" intermediate state

### Priority 6 (Optional): Brownout Disable Config Option

**Impact**: Gives users with very marginal CPAP hardware an escape hatch.

- Add `BROWNOUT_DETECT=OFF` support in `Config` class
- After config load, disable brownout via RTC register write if configured
- Log a clear warning on every boot when brownout is disabled

---

## Summary of Answers to Direct Questions

| Question | Answer |
|---|---|
| What was the brownout level before the change? | **Level 0 (~2.43 V)**, the ESP-IDF default. |
| Does the system forcibly reboot on brownout? | **Yes.** Immediate hardware reset, no graceful shutdown. |
| Can ULP do PCNT? | **No.** ULP cannot access PCNT hardware, and software polling is too slow (1-2 MHz vs 20-50 MHz bus speed). |
| What was the ESP doing when brownout hit? | Most likely a **WiFi TX burst** (~180-350 mA peak). See Section 5. |
| Should brownout be disabled completely? | **Not by default.** Level 0 is the safest minimum. An opt-in disable via `config.txt` is acceptable. |
| Why does SSE break on tab switch? | `stopSse()` is called every time the user leaves the Logs tab. This is a **pre-existing bug**. |
| Why does "REBOOTING" show on mobile tab resume? | The failure threshold is only 2 missed polls (6 seconds), which is too aggressive for mobile radio wake-up. |
