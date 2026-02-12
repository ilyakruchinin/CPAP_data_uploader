# System Architecture: Smart Upload with Bus Activity Detection

## 1. Upload Modes

The firmware supports two upload modes, selected via `UPLOAD_MODE` in config.json.

### 1.1 Scheduled Mode (`"scheduled"`)

All uploads happen **only within the upload window** defined by `UPLOAD_START_HOUR` and
`UPLOAD_END_HOUR`. Even in scheduled mode, bus inactivity (Z seconds of silence) must be
confirmed before taking SD card control.

Once all files are uploaded → done until next day's window.

### 1.2 Smart Mode (`"smart"`)

Data is split into two categories with different scheduling rules:

| Category | Definition | When it can upload |
|---|---|---|
| **Fresh data** | DATALOG folders within last B days + root/SETTINGS files | **Anytime** (24/7), as long as bus is idle |
| **Old data** | DATALOG folders older than B days | **Only within upload window** [START, END] |

After a complete upload cycle (all files done), the system enters a cooldown. After
cooldown + inactivity re-verified, it re-scans for new fresh data. If new files are
found (e.g., CPAP wrote new therapy data), a new upload cycle starts.

### 1.3 Upload Window

`UPLOAD_START_HOUR` and `UPLOAD_END_HOUR` define the **allowed upload window** (safe hours
when uploads are permitted for old/scheduled data).

Example: `UPLOAD_START_HOUR=8`, `UPLOAD_END_HOUR=22`
- Upload window: 08:00 → 22:00 (daytime, CPAP typically idle)
- Protected hours: 22:00 → 08:00 (nighttime, therapy likely in progress)

Cross-midnight support: if START > END, the window wraps. E.g., START=22, END=6 means
22:00 → 06:00 is the upload window.

---

## 2. Finite State Machine

```
                    ┌──────────────────────────────────────┐
                    │                                      │
                    ▼                                      │
              ┌──────────┐                                 │
              │   IDLE   │◄──── schedule window expired    │
              └────┬─────┘      or all done (scheduled)    │
                   │                                       │
          eligible to upload?                              │
          (mode + time + data)                             │
                   │                                       │
                   ▼                                       │
              ┌──────────┐     bus activity detected       │
              │LISTENING │────────────────┐                │
              │(Z secs)  │◄───────────────┘                │
              └────┬─────┘     (reset silence counter)     │
                   │                                       │
          Z seconds of silence                             │
                   │                                       │
                   ▼                                       │
              ┌──────────┐                                 │
              │ACQUIRING │─── SD init failed ──► RELEASING │
              └────┬─────┘                                 │
                   │                                       │
              SD mounted OK                                │
                   │                                       │
                   ▼                                       │
              ┌──────────┐                                 │
              │UPLOADING │                                 │
              │(max X min)│                                │
              └──┬────┬──┘                                 │
                 │    │                                    │
        all done │    │ X min expired                      │
                 │    │ (finish current file)              │
                 │    │                                    │
                 │    ▼                                    │
                 │ ┌──────────┐                            │
                 │ │RELEASING │                            │
                 │ └────┬─────┘                            │
                 │      │                                  │
                 │      ▼                                  │
                 │ ┌──────────┐                            │
                 │ │ COOLDOWN │                            │
                 │ │ (Y min)  │                            │
                 │ └────┬─────┘                            │
                 │      │                                  │
                 │      │  still eligible? ──No──► IDLE    │
                 │      │  (time window + data left)       │
                 │      │                                  │
                 │      ▼                                  │
                 │    LISTENING (back to inactivity check) │
                 │                                         │
                 ▼                                         │
              ┌──────────┐                                 │
              │ COMPLETE │─────────────────────────────────┘
              └──────────┘
                 │
                 │ smart mode only:
                 │ after cooldown + Z inactivity,
                 │ re-scan for new fresh data
                 │ → if found: back to LISTENING
                 │ → if none: IDLE
```

### State Descriptions

| State | Description | Duration |
|---|---|---|
| **IDLE** | Not eligible to upload. Checking periodically (every 60s). | Until schedule/mode triggers |
| **LISTENING** | PCNT sampling bus activity. Tracking consecutive silence. | Until Z seconds of silence |
| **ACQUIRING** | Taking SD card control, initializing SD_MMC. | ~500ms (brief transition) |
| **UPLOADING** | ESP has exclusive SD access. Uploading files as fast as possible. No periodic releases. | Up to X minutes or until all files done |
| **RELEASING** | Finishing current file, unmounting SD, releasing mux to host. | ~100ms (brief transition) |
| **COOLDOWN** | Card released to CPAP. Non-blocking wait. Web server still responsive. | Y minutes |
| **COMPLETE** | All files uploaded for this cycle. | Transition state |
| **MONITORING** | Manual mode: all uploads stopped, live PCNT data displayed in web UI. | Until user clicks "Stop Monitor" |

---

## 3. Data Categorization

### 3.1 Existing Implementation (Retained)

The current firmware already categorizes data well:

- **DATALOG folders** (`/DATALOG/YYYYMMDD/`): Therapy data, one folder per day.
  Sorted newest-first. Tracked by `UploadStateManager` (completed, pending, incomplete).
- **Root/SETTINGS files** (`/` and `/SETTINGS/`): Device configuration, identification.
  Tracked by per-file checksums.
- **Recent folders** (`isRecentFolder()`): DATALOG folders within `RECENT_FOLDER_DAYS` (B).
  These are re-scanned even when marked complete, to detect changed files.
- **MAX_DAYS cutoff**: Folders older than MAX_DAYS are completely ignored.

### 3.2 New: Fresh vs Old Data Split

The existing `isRecentFolder()` / `RECENT_FOLDER_DAYS` (B) concept maps directly to the
fresh/old data split:

| Category | Criteria | Scheduling |
|---|---|---|
| **Fresh** | `isRecentFolder(folderName) == true` (within B days) | Smart: anytime. Scheduled: window only. |
| **Fresh** | Root/SETTINGS files | Same as fresh DATALOG |
| **Old** | `isRecentFolder(folderName) == false` AND within MAX_DAYS | Both modes: window only |
| **Ignored** | Older than MAX_DAYS | Never uploaded |

### 3.3 Upload Completeness Rule

**Root/SETTINGS files are MANDATORY for a valid upload.** An upload of DATALOG folders
without the accompanying root and SETTINGS files (e.g., `Identification.json`,
`STR.edf`, machine settings) is not considered a valid import by SleepHQ or other
backends. These files are small and must always be included to finalize the upload,
even if the X-minute timer has expired.

This means the X-minute timer only gates **DATALOG folder processing**. Once DATALOG
folders are done (or timer expired mid-DATALOG), root/SETTINGS files are always
uploaded before transitioning to RELEASING.

### 3.4 Upload Priority Order

Within each upload session (UPLOADING state):

1. **Fresh DATALOG folders** (newest first) — timer X applies per-folder
2. **Old DATALOG folders** (newest first, only in window) — timer X applies per-folder
3. **Root/SETTINGS files** (MANDATORY) — always uploaded, timer does NOT skip these

Root/SETTINGS are uploaded last because they finalize the import. This matches the
SleepHQ import lifecycle: upload data files first, then metadata/config files, then
process the import.

---

## 4. Upload Session Flow (UPLOADING State Detail)

```
UPLOADING state entered
    │
    ├─ Start exclusive access timer (X minutes)
    │
    ├─ Phase 1: Fresh DATALOG folders (newest first)
    │   ├─ For each folder:
    │   │   ├─ Check timer: X minutes expired? → finish current file, exit phase
    │   │   ├─ For each file in folder:
    │   │   │   ├─ Check if already uploaded (checksum match) → skip
    │   │   │   ├─ Upload to all active backends (SMB/WebDAV/Cloud)
    │   │   │   ├─ Record checksum, update state
    │   │   │   └─ Handle web requests between files (ENABLE_TEST_WEBSERVER)
    │   │   └─ Mark folder completed
    │   └─ All fresh folders done (or timer expired → exit phase)
    │
    ├─ Phase 2: Old DATALOG folders (only if in upload window)
    │   ├─ Check timer: X minutes expired? → exit phase
    │   ├─ For each folder (newest first):
    │   │   ├─ Same file-by-file upload logic as Phase 1
    │   │   └─ Mark folder completed
    │   └─ All old folders done (or timer expired → exit phase)
    │
    ├─ Phase 3: Root/SETTINGS files *** MANDATORY — NEVER SKIPPED ***
    │   ├─ Timer does NOT gate this phase
    │   ├─ Upload all changed files (checksum comparison)
    │   ├─ These files are small and fast
    │   └─ Required to finalize a valid import
    │
    └─ All files uploaded → COMPLETE
       OR DATALOG timer expired but root/SETTINGS done → RELEASING
```

**Key rules**:
- No `checkAndReleaseSD()` calls during upload. The ESP holds exclusive access for the
  entire session. This eliminates the ~1.5 second penalty per release cycle.
- **Root/SETTINGS files are never skipped by the X-minute timer.** They are small, fast
  to upload, and required for the upload to be considered valid by backends.
- The X-minute timer only controls how many DATALOG folders are processed per session.
  If the timer expires mid-DATALOG, the current file finishes, then root/SETTINGS are
  uploaded, then the session ends with RELEASING (to resume DATALOG next cycle).

---

## 5. Smart Mode Re-scan Logic

After a complete upload cycle in smart mode:

```
COMPLETE
    │
    ├─ Release SD card (RELEASING)
    ├─ Cooldown (Y minutes)
    ├─ Listen for inactivity (Z seconds)
    │
    ├─ Take SD card briefly to re-scan
    │   ├─ Check fresh DATALOG folders for new/changed files
    │   │   (using isRecentFolder() + hasFileChanged())
    │   ├─ If new files found → start new upload cycle
    │   └─ If no new files → IDLE
    │
    └─ IDLE (wait for next trigger)
```

This handles the scenario where the CPAP writes new data between upload cycles (e.g.,
therapy summary files written after mask-off).

---

## 6. SD Activity Monitor (Web UI Feature)

### Purpose

A **"Monitor SD Activity"** button in the web interface that:
1. **Stops all upload activity** (FSM transitions to a dedicated MONITORING state)
2. **Displays live PCNT bus activity data** over time in the web UI
3. Allows the user to observe when the CPAP is active/idle and **fine-tune thresholds**
   (e.g., inactivity seconds, cooldown timing)

### FSM Integration

New state: `MONITORING` — entered via web button, exited via web "Stop Monitor" button.

```
Any state ──(web "Monitor SD Activity" button)──► MONITORING
                                                       │
MONITORING ──(web "Stop Monitor" button)──────────► IDLE
```

When entering MONITORING:
- If currently UPLOADING → finish current file, release SD card, then enter MONITORING
- If currently ACQUIRING → release SD card, enter MONITORING
- All other states → enter MONITORING immediately
- TrafficMonitor continues sampling (it's always running in the main loop)

### Web UI: Activity Timeline

The web page shows a **progressive, auto-updating activity timeline**:

```
┌─────────────────────────────────────────────────────────┐
│  SD Card Activity Monitor                    [Stop]     │
├─────────────────────────────────────────────────────────┤
│                                                         │
│  Time         Activity    Pulse Count    Status         │
│  ─────────    ────────    ───────────    ──────         │
│  14:32:05     ████████    1247           ACTIVE         │
│  14:32:06     ████████    983            ACTIVE         │
│  14:32:07     ██          12             ACTIVE         │
│  14:32:08                 0              idle           │
│  14:32:09                 0              idle           │
│  14:32:10                 0              idle           │
│  14:32:11     ████████    1547           ACTIVE         │
│  ...                                                    │
│                                                         │
│  Consecutive idle: 0s    Longest idle: 3s               │
│  Total active samples: 47    Total idle samples: 12     │
│                                                         │
│  Inactivity threshold (Z): 300s                         │
│  Would trigger upload: No (need 300s idle)              │
├─────────────────────────────────────────────────────────┤
│  Monitoring since: 14:31:52 (duration: 00:05:13)        │
│  [Stop Monitoring & Resume Normal Operation]            │
└─────────────────────────────────────────────────────────┘
```

### Implementation Approach

- **Endpoint**: `GET /api/sd-activity` returns JSON with latest PCNT samples
- **Polling**: Web page polls every 1 second (or uses Server-Sent Events if feasible)
- **Data**: TrafficMonitor stores a rolling buffer of recent samples (e.g., last 300
  samples = 5 minutes at 1 sample/second)
- **Metrics**: consecutive idle duration, longest idle streak, total active/idle ratio,
  raw pulse counts per sample window
- **Sample rate**: During MONITORING, TrafficMonitor can sample more frequently (e.g.,
  every 1 second instead of 100ms windows, reporting aggregate counts per second)

### Use Cases

1. **Pre-deployment calibration**: Run monitor during therapy start → observe when bus
   activity begins. Run during therapy end → observe when it stops. Use to set Z.
2. **Debugging**: If uploads fail to trigger, check if PCNT is detecting activity at all.
3. **Threshold tuning**: Watch activity patterns to decide optimal Z, X, Y values.

---

## 7. Interaction with Existing Components

### 7.1 Components Retained (with modifications)

| Component | Changes |
|---|---|
| `Config` | New params: UPLOAD_MODE, START/END_HOUR, INACTIVITY_SECONDS, EXCLUSIVE_ACCESS_MINUTES, COOLDOWN_MINUTES. Deprecate: UPLOAD_HOUR, SD_RELEASE_INTERVAL_SECONDS, SD_RELEASE_WAIT_MS, UPLOAD_INTERVAL_MINUTES. |
| `ScheduleManager` | Rewrite: support upload window (two hours), upload mode enum, `isInUploadWindow()` replacing `isUploadTime()`. |
| `SDCardManager` | Remove `digitalRead(CS_SENSE)` check from `takeControl()`. Activity detection moves to TrafficMonitor (called from main FSM). |
| `FileUploader` | Remove `checkAndReleaseSD()`. Add phase-based upload with mandatory root/SETTINGS finalization. Accept data category filter. |
| `TimeBudgetManager` | Simplify: X-minute timer replaces complex session budget. Remove pause/resume (no periodic releases). |
| `UploadStateManager` | No changes needed. Already tracks per-folder and per-file state. |
| `CPAPMonitor` | Replace stub with TrafficMonitor integration. |

### 7.2 New Components

| Component | Purpose |
|---|---|
| **TrafficMonitor** | PCNT-based bus activity detection on GPIO 33. Provides `isBusy()`, `getConsecutiveIdleMs()`, rolling sample buffer for monitoring UI. |
| **UploadFSM** (in main.cpp or new class) | The state machine driving IDLE → LISTENING → UPLOADING → COOLDOWN → MONITORING cycle. |

### 7.3 Components Unchanged

- `WiFiManager` — no changes
- `SMBUploader` / `WebDAVUploader` / `SleepHQUploader` — no changes (upload backends)
- `Logger` — no changes
- `TestWebServer` — updates: expose FSM state, SD activity monitor page/endpoint, remove SD release status

---

## 8. Timing Example

Configuration: `Z=300s, X=5min, Y=10min, B=2, START=8, END=22, MODE=smart`

```
Timeline (smart mode, morning after therapy):

07:00  CPAP therapy ends, mask off
07:00  CPAP writes final summary files
07:01  Bus goes silent (CPAP idle)
07:01  ESP in LISTENING state (fresh data eligible anytime)
07:06  5 minutes of silence confirmed (Z=300s)
07:06  → ACQUIRING → SD mounted
07:06  → UPLOADING: uploading last 2 days of DATALOG + root files
07:11  X=5min timer expires, current file finishes
07:11  → RELEASING → COOLDOWN
07:21  Y=10min cooldown complete
07:21  → LISTENING (check for inactivity again)
07:26  Z=300s silence confirmed
07:26  → ACQUIRING → UPLOADING: resume remaining files
07:29  All fresh files done → COMPLETE
07:29  Re-scan: no new files → COOLDOWN → LISTENING → ...
08:00  Upload window opens (START=8)
08:00  Old data now eligible
08:05  Z=300s silence confirmed
08:05  → UPLOADING: old DATALOG folders
08:10  X=5min timer → RELEASING → COOLDOWN
...    (cycle continues until all old data uploaded)
10:30  All files uploaded → IDLE
10:30  Next activity: tomorrow morning (or when new fresh data detected)
```

---

## 9. Open Questions

> These questions should be answered before implementation begins.

### Q1: Upload Window Direction

I'm interpreting `UPLOAD_START_HOUR=8, UPLOAD_END_HOUR=22` as **"uploads allowed 8am to
10pm"** (the safe daytime hours). The protected therapy hours (10pm–8am) are **outside**
this window.

- In **scheduled mode**: ALL uploads only within 8am–10pm
- In **smart mode**: fresh data anytime, old data only within 8am–10pm

**Is this correct?** Or do start/end define the therapy window (inverted logic)?

### Q2: Cross-Midnight Window

If `START=22, END=6`, should the upload window wrap around midnight (22:00→06:00)?
This would be unusual (uploading during typical therapy hours) but should be supported
for flexibility.

### Q3: Smart Mode COMPLETE → Re-scan Timing

After all files are uploaded in smart mode, how aggressively should we re-scan?

- **Option A**: Cooldown (Y min) → inactivity check (Z sec) → re-scan → if nothing new, IDLE
- **Option B**: Cooldown (Y min) → inactivity check (Z sec) → re-scan → if nothing new,
  wait another Y minutes and re-scan again (persistent polling)
- **Option C**: After COMPLETE, only re-scan if PCNT detects new bus activity followed by
  silence (event-driven rather than polling)

I've designed for **Option A** above. Option C is more elegant but requires keeping PCNT
running during IDLE state.

### Q4: Web "Upload Now" Behavior

Should the web interface "Upload Now" button:
- (a) Skip inactivity check entirely (immediate SD takeover)
- (b) Use a shortened inactivity check (e.g., 5 seconds instead of Z)
- (c) Still require full Z-second inactivity check

Currently it does (a). Recommend keeping (a) since it's a manual override — the user
knows the CPAP is not in use.

### Q5: Backward Compatibility

Should old config params be supported as fallbacks?

| Old Param | New Equivalent | Fallback? |
|---|---|---|
| `UPLOAD_HOUR` | `UPLOAD_START_HOUR` (same value), `UPLOAD_END_HOUR` (+2 hours) | Suggested |
| `SESSION_DURATION_SECONDS` | `EXCLUSIVE_ACCESS_MINUTES` (÷60) | Suggested |
| `SD_RELEASE_INTERVAL_SECONDS` | Removed (no periodic release) | Ignored with warning |
| `SD_RELEASE_WAIT_MS` | Removed | Ignored with warning |
| `UPLOAD_INTERVAL_MINUTES` | `UPLOAD_MODE=smart` (if >0) | Suggested |

### Q6: MAX_DAYS vs RECENT_FOLDER_DAYS Interaction

Current behavior: `MAX_DAYS` is a hard cutoff (older folders completely ignored).
`RECENT_FOLDER_DAYS` (B) defines re-scan window for changed files.

Proposed: B now **also** defines the fresh/old scheduling split.
MAX_DAYS remains as the absolute oldest limit. Data between B and MAX_DAYS is "old"
(uploaded only in window). Data older than MAX_DAYS is ignored entirely.

```
|--- ignored ---|--- old data ---|--- fresh data ---|
              MAX_DAYS           B days ago         today
```

**Is this correct?**
