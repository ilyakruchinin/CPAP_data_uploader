# PCNT False Trigger Analysis

## Observation
The user observed in logs (`3-7.txt.tmp`) that the Smart Mode feature "suppressing retries until new bus activity" was being instantly defeated. 
Exactly when the device transitions from `COOLDOWN` to `LISTENING`, the logs immediately report:
`[FSM] No-work suppression cleared — new bus activity detected`

Since this happens instantly upon the transition (and the CPAP only writes data roughly once an hour outside of therapy), the user suspected a hardware/PCNT interference issue with power states or DFS (Dynamic Frequency Scaling) transitions.

## Root Cause Analysis
Upon reviewing `UploadFSM.cpp` (which is integrated within `main.cpp`) and `TrafficMonitor.cpp`, the issue was completely identified as a **software logic flaw**, not a hardware glitch.

### The Mechanism of the Bug
1. When the FSM finishes `COOLDOWN`, it calls `handleCooldown()` which executes:
   ```cpp
   trafficMonitor.resetIdleTracking();
   transitionTo(UploadState::LISTENING);
   ```
2. Inside `resetIdleTracking()`, the continuous idle timer is reset to zero:
   ```cpp
   _consecutiveIdleMs = 0;
   ```
3. The very next iteration of the main loop runs `handleListening()`, which contains the suppression check:
   ```cpp
   if (g_noWorkSuppressed) {
       if (!trafficMonitor.isIdleFor(1000)) {
           g_noWorkSuppressed = false;
           LOG("[FSM] No-work suppression cleared — new bus activity detected");
       }
   //...
   ```
4. Look at how `isIdleFor` is implemented:
   ```cpp
   bool TrafficMonitor::isIdleFor(uint32_t ms) {
       return _consecutiveIdleMs >= ms;
   }
   ```
   
Because `_consecutiveIdleMs` was just set to `0` by the cooldown exit, `isIdleFor(1000)` evaluates `0 >= 1000`, which is `false`.
Therefore, `!trafficMonitor.isIdleFor(1000)` instantly evaluates to `true`!

The ESP32 did not detect any electrical pulses on the SD CMD line. It simply fell victim to a flawed Boolean logic assumption where "not idle for exactly 1 second" was interpreted as "actively busy right now", ignoring the fact that the timer had just restarted.

## The Fix
Change the gating logic in `main.cpp`'s `handleListening()` to actually query the PCNT hardware state for active bus pulses, rather than relying on the duration of the idle timer.

**Old Code:**
```cpp
if (!trafficMonitor.isIdleFor(1000)) 
```

**New Code:**
```cpp
if (trafficMonitor.isBusy())
```

This properly relies on the 100ms hardware sampling window inside `TrafficMonitor::update()` where `_lastSampleActive` is strictly gated by real `count > 0` readings from the PCNT peripheral.
