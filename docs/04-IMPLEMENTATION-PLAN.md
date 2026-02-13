# Implementation Plan

## Overview

This plan transforms the upload system from periodic-release scheduling to an exclusive-access
FSM with PCNT-based bus activity detection. The work is divided into 6 phases, designed to be
implemented and tested incrementally.

---

## Phase 1: Hardware Fix + TrafficMonitor

**Goal**: Fix CS_SENSE pin and build the PCNT-based bus activity detector.

### 1.1 Fix CS_SENSE Pin

**File**: `include/pins_config.h`

- Change `#define CS_SENSE 32` → `#define CS_SENSE 33`
- Update the TODO comment to: `// Chip Select sense - detects host bus activity (VERIFIED from schematic)`

### 1.2 Create TrafficMonitor

**New file**: `include/TrafficMonitor.h`

```
class TrafficMonitor {
public:
    void begin(int pin);              // Initialize PCNT on given GPIO
    void update();                    // Call every loop() — non-blocking ~100ms sample
    bool isBusy();                    // True if activity detected in last sample window
    bool isIdleFor(uint32_t ms);      // True if no activity for at least ms milliseconds
    uint32_t getConsecutiveIdleMs();  // How long has the bus been silent?
    void resetIdleTracking();         // Reset silence counter
};
```

**New file**: `src/TrafficMonitor.cpp`

Implementation details:
- `begin()`: Configure GPIO as INPUT_PULLUP, init PCNT unit 0 channel 0
  - `pos_mode = PCNT_COUNT_INC`, `neg_mode = PCNT_COUNT_INC` (count both edges)
  - Glitch filter value = 10 (~100ns)
  - Start counter
- `update()`: Called every loop iteration
  - Check if 100ms has elapsed since last sample
  - Read counter value, clear counter
  - If count > 0: bus active, reset idle tracking
  - If count == 0: accumulate idle duration
- `isBusy()`: return last sample had count > 0
- `isIdleFor(ms)`: return consecutiveIdleMs >= ms
- `getConsecutiveIdleMs()`: return tracked value

### 1.3 Test

- Flash firmware with TrafficMonitor logging
- Verify PCNT detects activity when CPAP accesses SD card
- Verify silence is correctly tracked when CPAP is idle

---

## Phase 2: Config Changes

**Goal**: Add new config parameters, handle deprecation of old ones.

### 2.1 Update Config.h

Add new private members:
```cpp
String uploadMode;           // "scheduled" or "smart"
int uploadStartHour;         // 0-23
int uploadEndHour;           // 0-23
int inactivitySeconds;       // Z
int exclusiveAccessMinutes;  // X
int cooldownMinutes;         // Y
```

Add new public getters:
```cpp
const String& getUploadMode() const;
int getUploadStartHour() const;
int getUploadEndHour() const;
int getInactivitySeconds() const;
int getExclusiveAccessMinutes() const;
int getCooldownMinutes() const;
bool isSmartMode() const;    // convenience: uploadMode == "smart"
```

### 2.2 Update Config.cpp

In `loadFromSD()`:
- Parse new fields with defaults
- Backward compatibility: if old params present and new absent, migrate
- Validation with clamping and warnings
- Log deprecation warnings for old params

```
// New parameters
uploadMode = doc["UPLOAD_MODE"] | "scheduled";
uploadStartHour = doc["UPLOAD_START_HOUR"] | 8;
uploadEndHour = doc["UPLOAD_END_HOUR"] | 22;
inactivitySeconds = doc["INACTIVITY_SECONDS"] | 125;
exclusiveAccessMinutes = doc["EXCLUSIVE_ACCESS_MINUTES"] | 5;
cooldownMinutes = doc["COOLDOWN_MINUTES"] | 10;

// Backward compat: UPLOAD_HOUR → UPLOAD_START_HOUR
if (!doc.containsKey("UPLOAD_START_HOUR") && doc.containsKey("UPLOAD_HOUR")) {
    uploadStartHour = doc["UPLOAD_HOUR"] | 8;
    uploadEndHour = uploadStartHour + 2;  // 2-hour window
    LOG_WARN("UPLOAD_HOUR is deprecated. Using UPLOAD_START_HOUR/END_HOUR instead.");
}

// Backward compat: UPLOAD_INTERVAL_MINUTES > 0 → smart mode
if (!doc.containsKey("UPLOAD_MODE") && (doc["UPLOAD_INTERVAL_MINUTES"] | 0) > 0) {
    uploadMode = "smart";
    LOG_WARN("UPLOAD_INTERVAL_MINUTES is deprecated. Setting UPLOAD_MODE=smart.");
}

// Backward compat: SESSION_DURATION_SECONDS → EXCLUSIVE_ACCESS_MINUTES
if (!doc.containsKey("EXCLUSIVE_ACCESS_MINUTES") && doc.containsKey("SESSION_DURATION_SECONDS")) {
    int secs = doc["SESSION_DURATION_SECONDS"] | 300;
    exclusiveAccessMinutes = max(1, secs / 60);
    LOG_WARN("SESSION_DURATION_SECONDS is deprecated. Using EXCLUSIVE_ACCESS_MINUTES instead.");
}

// Deprecation warnings
if (doc.containsKey("SD_RELEASE_INTERVAL_SECONDS")) {
    LOG_WARN("SD_RELEASE_INTERVAL_SECONDS is deprecated and ignored (exclusive access mode).");
}
if (doc.containsKey("SD_RELEASE_WAIT_MS")) {
    LOG_WARN("SD_RELEASE_WAIT_MS is deprecated and ignored (exclusive access mode).");
}
```

### 2.3 Test

- Load config with new params → verify correct values
- Load config with only old params → verify migration + warnings
- Load config with invalid values → verify clamping

---

## Phase 3: ScheduleManager Rewrite

**Goal**: Support upload window (two hours) and upload mode.

### 3.1 Update ScheduleManager.h

Replace single `uploadHour` with:
```cpp
int uploadStartHour;
int uploadEndHour;
String uploadMode;     // "scheduled" or "smart"
bool uploadCompletedToday;
int lastCompletedDay;  // tm_yday of last completion
```

New methods:
```cpp
bool begin(const String& mode, int startHour, int endHour, int gmtOffsetHours);
bool isInUploadWindow();        // Current hour within [start, end]
bool canUploadFreshData();      // Smart: always. Scheduled: in window.
bool canUploadOldData();        // Both modes: in window only.
bool isUploadEligible(bool hasFreshData, bool hasOldData);
void markDayCompleted();        // For scheduled mode
bool isDayCompleted();
```

### 3.2 Rewrite ScheduleManager.cpp

- `isInUploadWindow()`: handle cross-midnight (START > END)
  ```
  if (startHour <= endHour)
      return currentHour >= startHour && currentHour < endHour;
  else  // wraps midnight
      return currentHour >= startHour || currentHour < endHour;
  ```
- `canUploadFreshData()`: smart mode → true; scheduled → `isInUploadWindow()`
- `canUploadOldData()`: always → `isInUploadWindow()`
- `markDayCompleted()`: set `lastCompletedDay = today's tm_yday`
- `isDayCompleted()`: check if `lastCompletedDay == today's tm_yday`
- Remove `calculateNextUploadTime()` (no longer needed for FSM-based approach)

### 3.3 Test

- Verify window checks for normal (8→22) and cross-midnight (22→6) cases
- Verify fresh/old data eligibility in both modes

---

## Phase 4: FileUploader Modifications

**Goal**: Remove periodic SD release, add timer-based exclusive access, support data filtering.

### 4.1 Add Upload Result Enum

```cpp
enum class UploadResult {
    COMPLETE,    // All eligible files uploaded
    TIMEOUT,     // X-minute timer expired (partial upload, not an error)
    ERROR        // Upload failure
};
```

### 4.2 Add Data Filter Enum

```cpp
enum class DataFilter {
    FRESH_ONLY,  // Only fresh DATALOG + root/SETTINGS
    OLD_ONLY,    // Only old DATALOG folders
    ALL_DATA     // Everything
};
```

### 4.3 New Method: uploadWithExclusiveAccess()

```cpp
UploadResult uploadWithExclusiveAccess(
    SDCardManager* sdManager,
    int maxMinutes,        // X
    DataFilter filter
);
```

Implementation:
- Record start time
- Phase 1: Fresh DATALOG folders (if filter includes fresh)
  - Before each file: check if elapsed > maxMinutes → exit this phase
  - Upload file to all backends
- Phase 2: Old DATALOG folders (if filter includes old)
  - Before each file: check if elapsed > maxMinutes → exit this phase
  - Upload file to all backends
- Phase 3: Root/SETTINGS files *** MANDATORY — NEVER SKIPPED ***
  - Timer does NOT gate this phase
  - Upload all changed files (checksum comparison)
  - These files are small and required for a valid import
- Return COMPLETE if all done, TIMEOUT if DATALOG timer expired (but root/SETTINGS done), ERROR on failure

**Important**: An upload without root/SETTINGS files is not valid. These files
(e.g., Identification.json, STR.edf, machine settings) are required by SleepHQ
and other backends to process the import. They are always uploaded last to
finalize the import, regardless of the X-minute timer.

### 4.4 Remove Periodic Release

- Delete `checkAndReleaseSD()` method
- Delete `lastSdReleaseTime` member
- Remove all `checkAndReleaseSD()` calls from `uploadDatalogFolder()` and `uploadNewFiles()`

### 4.5 Timer Management

The X-minute timer is managed by `uploadWithExclusiveAccess()` directly (simple
`millis()` comparison). No separate timer manager class is needed. TimeBudgetManager
has been deleted.

### 4.6 Test

- Upload with FRESH_ONLY filter → verify only recent folders + root/SETTINGS files
- Upload with ALL_DATA → verify all folders + root/SETTINGS files
- Upload with 1-minute timer → verify DATALOG times out but root/SETTINGS still uploaded
- Upload with large timer → verify COMPLETE when all files done
- Verify root/SETTINGS are NEVER skipped regardless of timer state

### 4.7 Single-Pass Streaming Optimization (Implemented)

The upload logic was further optimized to minimize SD card I/O and memory usage:
- **Single-Pass**: Files are read only once. MD5 checksum is calculated *during* the upload stream.
- **Footer Hash**: SleepHQ API accepts `content_hash` in the multipart footer, allowing streamed calculation.
- **Chunked Decoding**: Custom chunked response decoder implemented to support persistent TLS connections (keep-alive) with Cloudflare.
- **Streaming Only**: In-memory buffering for small files removed to prevent heap fragmentation.

---

## Phase 5: SDCardManager Cleanup

**Goal**: Remove CS_SENSE digitalRead from takeControl().

### 5.1 Update SDCardManager.cpp

`takeControl()`:
```cpp
bool SDCardManager::takeControl() {
    if (espHasControl) return true;

    // Activity detection is handled by TrafficMonitor + FSM BEFORE this call.
    // By the time takeControl() is called, the FSM has already confirmed bus silence.

    setControlPin(true);  // ESP takes mux
    espHasControl = true;
    delay(500);           // Card stabilization

    if (!SD_MMC.begin("/sdcard", SDIO_BIT_MODE_FAST)) {
        LOG("SD card mount failed");
        releaseControl();
        return false;
    }

    // ... rest unchanged (card type check, logging)
}
```

Remove `digitalRead(CS_SENSE)` check — the FSM guarantees idle state before calling.

### 5.2 Test

- Verify SD card mounts correctly without the digitalRead gate
- Verify FSM prevents calling takeControl() when bus is active

---

## Phase 6: Main Loop FSM

**Goal**: Replace the current loop() with FSM-driven upload orchestration.

### 6.1 Define FSM State

```cpp
enum class UploadState {
    IDLE,
    LISTENING,
    ACQUIRING,
    UPLOADING,
    RELEASING,
    COOLDOWN,
    COMPLETE,
    MONITORING     // Manual mode: uploads stopped, live PCNT data in web UI
};
```

### 6.2 Global State Variables

```cpp
UploadState currentState = UploadState::IDLE;
unsigned long stateEnteredAt = 0;          // millis() when current state started
unsigned long cooldownStartedAt = 0;
bool freshDataRemaining = false;
bool oldDataRemaining = false;
bool uploadCycleHadTimeout = false;        // true if last UPLOADING ended with TIMEOUT
bool monitoringRequested = false;           // true when web "Monitor SD Activity" clicked
bool stopMonitoringRequested = false;       // true when web "Stop Monitor" clicked
```

### 6.3 Main Loop Structure

```cpp
void loop() {
    // Always: handle web server, log dumps, WiFi reconnect, NTP sync
    handleAlwaysOnTasks();

    // Always: update traffic monitor (non-blocking ~100ms sample)
    trafficMonitor.update();

    // FSM dispatch
    // Check for monitoring request (can interrupt most states)
    if (monitoringRequested) {
        monitoringRequested = false;
        // If UPLOADING: the upload loop checks this flag and gracefully
        // finishes current file + root/SETTINGS before yielding.
        // Other states: transition immediately.
        if (currentState != UploadState::UPLOADING) {
            if (currentState == UploadState::ACQUIRING) {
                sdManager.releaseControl();
            }
            currentState = UploadState::MONITORING;
            stateEnteredAt = millis();
        }
        // UPLOADING handles it internally via flag check
    }

    switch (currentState) {
        case IDLE:       handleIdle();       break;
        case LISTENING:  handleListening();  break;
        case ACQUIRING:  handleAcquiring();  break;
        case UPLOADING:  handleUploading();  break;
        case RELEASING:  handleReleasing();  break;
        case COOLDOWN:   handleCooldown();   break;
        case COMPLETE:   handleComplete();   break;
        case MONITORING: handleMonitoring(); break;
    }
}
```

### 6.4 State Handlers (Pseudocode)

**handleIdle()** (scheduled mode only):
```
- Smart mode: never enters IDLE. Initial state is LISTENING.
- Scheduled mode: every 60 seconds, check if upload window is open
  - If scheduleManager.isInUploadWindow() AND NOT isDayCompleted():
      transition to LISTENING
  - NOTE: Does NOT check data availability — always transitions to LISTENING
    when the window opens, even if all known files are marked complete.
    This ensures new DATALOG folders written by the CPAP since the last
    upload are discovered during the scan phase of the upload cycle.
```

**handleListening()**:
```
- TrafficMonitor.update() already called above
- If trafficMonitor.isIdleFor(config.getInactivitySeconds() * 1000):
      transition to ACQUIRING
- If no longer eligible (window closed, scheduled mode):
      transition to IDLE
```

**handleAcquiring()**:
```
- sdManager.takeControl()
  - Success → transition to UPLOADING
  - Failure → transition to RELEASING (will try again after cooldown)
```

**handleUploading()** (non-blocking — upload runs as FreeRTOS task on Core 0):
```
- If upload task not yet started:
  - Determine data filter (ALL_DATA / FRESH_ONLY / OLD_ONLY)
  - Disable web server in uploader (main loop handles it exclusively)
  - Spawn FreeRTOS task pinned to Core 0 (16KB stack)
    - Task calls uploader->uploadWithExclusiveAccess(sdManager, X minutes, filter)
    - On completion: sets volatile result flag and self-deletes
  - If task creation fails: fall back to synchronous (blocking) upload
- If upload task complete:
  - Restore web server in uploader
  - Read result:
    - COMPLETE → transition to COMPLETE
    - TIMEOUT → uploadCycleHadTimeout = true, transition to RELEASING
    - ERROR → transition to RELEASING
- Else: task still running — return immediately (main loop continues)

NOTE: While upload task runs on Core 0, the main loop on Core 1 continues
handling web server requests, TrafficMonitor updates, and WiFi reconnection.
SD card log dumps and web triggers (reset, trigger upload) are blocked while
the upload task is running to prevent shared-state conflicts.
```

**handleReleasing()**:
```
- sdManager.releaseControl()
- cooldownStartedAt = millis()
- transition to COOLDOWN
```

**handleCooldown()**:
```
- If elapsed < Y minutes: return (non-blocking wait)
- After Y minutes:
  - Smart mode: ALWAYS transition to LISTENING (continuous loop)
  - Scheduled mode:
      - If still in upload window AND day not completed:
          transition to LISTENING
      - Else: transition to IDLE (window closed)
```

**handleComplete()**:
```
- Smart mode:
    - transition to RELEASING (which goes to COOLDOWN → LISTENING)
    - The continuous loop ensures new data is discovered on next cycle

- Scheduled mode:
    - scheduleManager.markDayCompleted()
    - transition to IDLE
```

**handleMonitoring()**:
```
- TrafficMonitor.update() runs as normal (called in main loop)
- No upload activity, no SD card access
- Web endpoint /api/sd-activity serves live PCNT sample data
- If stopMonitoringRequested:
    - stopMonitoringRequested = false
    - transition to IDLE
```

### 6.5 Web Trigger Integration

```cpp
if (g_triggerUploadFlag) {
    g_triggerUploadFlag = false;
    // Force immediate upload — skip inactivity check
    currentState = UploadState::ACQUIRING;
    stateEnteredAt = millis();
    uploadCycleHadTimeout = false;
}
```

// Web "Monitor SD Activity" button
if (g_monitorActivityFlag) {
    g_monitorActivityFlag = false;
    monitoringRequested = true;
}

// Web "Stop Monitor" button
if (g_stopMonitorFlag) {
    g_stopMonitorFlag = false;
    stopMonitoringRequested = true;
}
```

Other web triggers (reset state) remain unchanged —
they operate independently of the FSM.

### 6.6 State Logging

Each state transition should log:
```
LOG("[FSM] IDLE → LISTENING (fresh data pending, smart mode)");
LOG("[FSM] LISTENING → ACQUIRING (300s of bus silence confirmed)");
LOG("[FSM] UPLOADING → RELEASING (5min exclusive access expired, 12 files uploaded)");
```

### 6.7 Web Status Endpoint

Update `/status` to include:
```json
{
  "fsm_state": "UPLOADING",
  "state_duration_seconds": 142,
  "idle_duration_seconds": 0,
  "upload_mode": "smart",
  "in_upload_window": true,
  "fresh_data_pending": true,
  "old_data_pending": false,
  "exclusive_timer_remaining_seconds": 158
}
```

### 6.8 SD Activity Monitor Endpoint

New endpoint `GET /api/sd-activity` (only active during MONITORING state):
```json
{
  "monitoring": true,
  "monitoring_duration_seconds": 313,
  "current_idle_ms": 4200,
  "longest_idle_ms": 18500,
  "total_active_samples": 47,
  "total_idle_samples": 266,
  "inactivity_threshold_seconds": 300,
  "would_trigger_upload": false,
  "samples": [
    {"time": 1707700325, "pulse_count": 1247, "active": true},
    {"time": 1707700326, "pulse_count": 0, "active": false},
    ...
  ]
}
```

### 6.9 Test

- Full FSM cycle: IDLE → LISTENING → ACQUIRING → UPLOADING → RELEASING → COOLDOWN → LISTENING
- Smart mode continuous loop: COMPLETE → RELEASING → COOLDOWN → LISTENING (never IDLE)
- Smart mode new data: COMPLETE → loop → next cycle discovers new DATALOG folder
- Scheduled mode: COMPLETE → IDLE, verify no uploads until next day
- Scheduled mode re-discovery: window opens → IDLE → LISTENING even if all files complete
- Window boundary: verify transition to IDLE when window closes during COOLDOWN (scheduled only)
- Web trigger: verify immediate ACQUIRING from any state
- MONITORING: verify entering from IDLE stops uploads, PCNT data flows to web UI
- MONITORING: verify entering from UPLOADING finishes file + root/SETTINGS first
- MONITORING: verify "Stop Monitor" returns to IDLE

---

## Phase 7: SD Activity Monitor Web UI

**Goal**: Web button to stop uploads and display live PCNT bus activity for threshold tuning.

### 7.1 TrafficMonitor: Add Sample Buffer

Add to `TrafficMonitor`:
```cpp
struct ActivitySample {
    uint32_t timestamp;    // seconds since boot (millis()/1000)
    uint16_t pulseCount;   // PCNT count for this 1-second window
    bool active;           // pulseCount > 0
};

static const int MAX_SAMPLES = 300;  // 5 minutes of 1-second samples
ActivitySample sampleBuffer[MAX_SAMPLES];
int sampleHead = 0;
int sampleCount = 0;
uint32_t longestIdleMs = 0;
uint32_t totalActiveSamples = 0;
uint32_t totalIdleSamples = 0;
```

`update()` already runs every loop iteration. When in MONITORING state (or always,
for minimal overhead), aggregate 100ms PCNT samples into 1-second windows and push
to the circular buffer.

Add `getSamplesJSON(int maxSamples)` method to serialize recent samples for the web API.

### 7.2 Web Endpoints

**TestWebServer.cpp** additions:

1. `POST /api/monitor/start` → sets `g_monitorActivityFlag = true`
2. `POST /api/monitor/stop` → sets `g_stopMonitorFlag = true`
3. `GET /api/sd-activity` → returns JSON with current PCNT data:
   - `monitoring`: bool
   - `monitoring_duration_seconds`: int
   - `current_idle_ms`: current consecutive idle streak
   - `longest_idle_ms`: longest observed idle streak
   - `total_active_samples` / `total_idle_samples`: counts
   - `inactivity_threshold_seconds`: Z value from config
   - `would_trigger_upload`: current_idle >= Z
   - `samples`: array of recent {time, pulse_count, active}

### 7.3 Web UI Page

Add a monitor section to the existing web interface (or a dedicated `/monitor` page):
- "Monitor SD Activity" button → POST /api/monitor/start
- Auto-updating table of activity samples (poll GET /api/sd-activity every 1 second)
- Visual bar chart of pulse counts (simple HTML/CSS bars)
- Running statistics: consecutive idle, longest idle, active/idle ratio
- Threshold indicator: "Would trigger upload: Yes/No"
- "Stop Monitoring" button → POST /api/monitor/stop

Implementation: embedded HTML in TestWebServer.cpp (same pattern as existing pages),
using JavaScript fetch() for polling.

### 7.4 Test

- Start monitor → verify uploads stop, PCNT data appears in web UI
- Observe CPAP therapy start → verify activity spikes in UI
- Observe CPAP therapy end → verify idle streak grows
- Stop monitor → verify normal FSM resumes
- Memory: verify 300-sample buffer doesn't cause issues

---

## File Change Summary

| File | Action | Phase |
|---|---|---|
| `include/pins_config.h` | Modify: CS_SENSE 32→33 | 1 |
| `include/TrafficMonitor.h` | **Create** | 1, 7 |
| `src/TrafficMonitor.cpp` | **Create** | 1, 7 |
| `include/Config.h` | Modify: add new fields + getters | 2 |
| `src/Config.cpp` | Modify: parse new fields, deprecation | 2 |
| `include/ScheduleManager.h` | Modify: rewrite interface | 3 |
| `src/ScheduleManager.cpp` | Modify: rewrite implementation | 3 |
| `include/FileUploader.h` | Modify: new enums, new method, remove periodic release | 4 |
| `src/FileUploader.cpp` | Modify: major refactor, mandatory root/SETTINGS | 4 |
| `include/SDCardManager.h` | No changes | 5 |
| `src/SDCardManager.cpp` | Modify: remove digitalRead check | 5 |
| `src/main.cpp` | Modify: major rewrite of loop(), MONITORING state | 6 |
| `src/TestWebServer.cpp` | Modify: FSM state in /status, monitor endpoints + UI | 6, 7 |
| `include/CPAPMonitor.h` | Modify: integrate TrafficMonitor or remove stub | 6 |

---

## Risk Assessment

| Risk | Mitigation |
|---|---|
| GPIO 33 doesn't work as expected | Phase 1 tests this first. Can fall back to GPIO 32 if needed. |
| PCNT misses activity (false idle) | Glitch filter tuning. Z default (125s) is 2× therapy write interval (~58s). See 01-FINDINGS.md §6. |
| CPAP confused by long SD card absence | X-minute limit (default 5 min). CPAP tolerates brief removals. |
| Upload too slow with exclusive access | Should be FASTER — no 1.5s release overhead per file. |
| Breaking config.json backward compat | Explicit migration logic + deprecation warnings. |
| Memory usage increase | TrafficMonitor uses hardware PCNT — negligible RAM. FSM state is a single enum. |
| Watchdog timeout during long uploads | Existing `yield()` calls in upload loops. Web server handling in between files. |

---

## Implementation Order

Phases are designed to be tested independently:

1. **Phase 1** (TrafficMonitor) — can be tested in isolation with logging
2. **Phase 2** (Config) — backward compat testable with existing firmware
3. **Phase 3** (ScheduleManager) — unit testable
4. **Phase 4** (FileUploader) — testable with web trigger, mandatory root/SETTINGS
5. **Phase 5** (SDCardManager) — minimal change, low risk
6. **Phase 6** (Main loop FSM) — integration, ties everything together
7. **Phase 7** (SD Activity Monitor) — web UI for threshold tuning, can be developed in parallel with Phase 6

Estimated total: **~1000-1200 lines of new/modified code** across all phases.
