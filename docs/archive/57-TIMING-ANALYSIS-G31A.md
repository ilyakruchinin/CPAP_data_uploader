# Feasibility Re-Assessment: Time Budgets & Smart Schedule Boundaries

Your instincts were incredibly sharp. By thinking about how time budgets (`EXCLUSIVE_ACCESS_MINUTES`) and Scheduled Boundaries interact with the Finite State Machine (FSM), you've identified a hidden, critical logic flaw that exists not just in the proposed "Early Suppression", but actually actively plagues the *current* `NOTHING_TO_DO` implementation.

Here is the re-assessment of the two edge-case mechanics:

## 1. Time Budgets vs. No-Work Suppression (`EXCLUSIVE_ACCESS_MINUTES`)

**Scenario:** 30 days of data are pending, but the upload is capped at 2 minutes per cycle.
**Behavior:** Safe.

If an upload session reaches the `EXCLUSIVE_ACCESS_MINUTES` time budget (e.g., stopping after Day 10 and Day 9), `FileUploader::runFullSession()` breaks the loop and deliberately returns `UploadResult::TIMEOUT`.

In `main.cpp`, a `TIMEOUT` result does **not** assert the `g_noWorkSuppressed` flag:
```cpp
case UploadResult::TIMEOUT:
    uploadCycleHadTimeout = true; // Tracks the timeout
    transitionTo(UploadState::RELEASING);
    break; // No suppression applied!
```
The FSM will process `RELEASING -> COOLDOWN`. After 10 minutes of cooldown, it transitions to `LISTENING`. Because suppression is false, the 62-second inactivity timer will kick in, and the FSM will loop right back to `ACQUIRING` to pick up Days 8 and 7.

So, long queues of folders broken up by time budgets are perfectly safe and unaffected.

## 2. The Smart-Mode Schedule Blindspot (Critical Flaw Found!)

**Scenario:** The CPAP generates recent data at 2:00 AM. The time-window for old data opens at 8:00 AM. 
**Behavior:** Broken FSM Paralysis. Old data will never upload.

You completely nailed this scenario. Here is exactly what is happening in the current codebase (and what would happen if we used Early Suppression):

1. **2:00 AM (Outside Window):** The FSM probes the SD card. Because it's outside the old-data window, `hasWorkToUpload()` ignores the 30 days of old data. It returns `NOTHING_TO_DO` (or uploads the recent data and returns `COMPLETE`).
2. **Suppression Engages:** The FSM sets `g_noWorkSuppressed = true` and loops back to `LISTENING`.
3. **The Trap:** In Smart Mode, the FSM is hardcoded to remain in `LISTENING` 24/7. When `g_noWorkSuppressed` is true, the `handleListening()` function puts blinders on:
   ```cpp
   if (g_noWorkSuppressed) {
       if (trafficMonitor.isBusy()) { g_noWorkSuppressed = false; }
       return; // Smart Mode returns IMMEDIATELY. End of checks.
   }
   ```
4. **8:00 AM (Window Opens):** The clock strikes 8:00 AM. The system is allowed to upload old data. However, the ESP32 is still paralyzed under `g_noWorkSuppressed == true`. It will **absolutely refuse** to wake up or probe the SD card because the only thing that can clear suppression is physical PCNT pulses from the CPAP machine!
5. **Result:** The 8:00 AM - 10:00 PM schedule window passes right by. Unless the user physically uses their CPAP during the day to generate PCNT pulses, the FSM sleeps completely through the window. Old data is silently ignored forever.

### The Required Solution

To fix the current bug (and safely allow early suppression on `COMPLETE`), we must implement an **edge-trigger** in the `LISTENING` state that instantly clears PCNT suppression the moment the time window opens. 

We can track the state of the window internally:
```cpp
// In handleListening()
static bool lastWindowOpen = false;
bool currentWindowOpen = uploader->getScheduleManager()->canUploadOldData();

// If the FSM is suppressed, but the schedule window JUST opened, wake it up!
if (g_noWorkSuppressed && currentWindowOpen && !lastWindowOpen) {
    g_noWorkSuppressed = false;
    LOG("[FSM] Schedule window opened — clearing no-work suppression to scan for old data");
}
lastWindowOpen = currentWindowOpen;
```

This guarantees that:
1. At 2:00 AM, it suppresses properly (saving battery/SD wear).
2. At 8:00 AM precisely, it instantly wakes up from suppression and queues an SD scan for old folders.
3. If it uploads everything and finishes at 8:15 AM, it gets `COMPLETE`, suppresses itself again, and stays asleep until the user's next therapy session (PCNT activity) or the *next* day's 8:00 AM window.

You prevented a massive creeping bug here. 
Let me know if you would like me to push this fix to `main.cpp`!
