# System Architecture: Smart Upload with Bus Activity Detection

## 1. Upload Modes

The firmware supports two upload modes, selected via `UPLOAD_MODE` in config.txt.

### 1.1 Scheduled Mode (`"scheduled"`)

All uploads happen **only within the upload window** defined by `UPLOAD_START_HOUR` and
`UPLOAD_END_HOUR`. Even in scheduled mode, bus inactivity (Z seconds of silence) must be
confirmed before taking SD card control.

When the upload window opens, the FSM transitions from IDLE to LISTENING — **even if all
known files are marked complete** — to discover any new data the CPAP may have written.
Once a scan confirms no new data exists, the day is marked as completed → IDLE until
the next day's window.

### 1.2 Smart Mode (`"smart"`)

Data is split into two categories with different scheduling rules:

| Category | Definition | When it can upload |
|---|---|---|
| **Fresh data** | DATALOG folders within last B days + root/SETTINGS files | **Anytime** (24/7), as long as bus is idle |
| **Old data** | DATALOG folders older than B days | **Only within upload window** [START, END] |

Smart mode operates as a **continuous loop**: LISTENING → ACQUIRING → UPLOADING →
RELEASING → COOLDOWN → LISTENING. The FSM **never enters IDLE** in smart mode.
After every upload cycle (whether complete or timed out), the system cools down and
returns to LISTENING to wait for bus inactivity before the next cycle. This ensures
new data written by the CPAP between upload cycles is always discovered on the next
scan, without relying on stale state information.

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

### 2.1 Smart Mode FSM (continuous loop — no IDLE)

```
              ┌──────────┐     bus activity detected
     ┌───────►│LISTENING │────────────────┐
     │        │(Z secs)  │◄───────────────┘
     │        └────┬─────┘     (reset silence counter)
     │             │
     │    Z seconds of silence
     │             │
     │             ▼
     │        ┌──────────┐
     │        │ACQUIRING │─── SD init failed ──┐
     │        └────┬─────┘                     │
     │             │                           │
     │        SD mounted OK                    │
     │             │                           │
     │             ▼                           │
     │        ┌──────────┐                     │
     │        │UPLOADING │                     │
     │        │(max X min)│                    │
     │        └──┬────┬──┘                     │
     │           │    │                        │
     │  all done │    │ X min expired          │
     │  (COMPLETE)│   │ (finish current file)  │
     │           │    │                        │
     │           │    ▼                        │
     │           │ ┌──────────┐                │
     │           └►│RELEASING │◄───────────────┘
     │             └────┬─────┘
     │                  │
     │                  ▼
     │             ┌──────────┐
     └─────────────│ COOLDOWN │
                   │ (Y min)  │
                   └──────────┘
```

Smart mode **always** returns to LISTENING after cooldown, regardless of whether the
last upload was complete, timed out, or had an error. This creates a continuous loop
that naturally discovers new data on each scan cycle.

### 2.2 Scheduled Mode FSM (IDLE between windows)

```
              ┌──────────┐
              │   IDLE   │◄──── window closed / day completed
              └────┬─────┘
                   │
          upload window open?
          (not yet completed today)
                   │
                   ▼
              ┌──────────┐     bus activity detected
              │LISTENING │────────────────┐
              │(Z secs)  │◄───────────────┘
              └────┬─────┘     (reset silence counter)
                   │
          Z seconds of silence
                   │
                   ▼
              ┌──────────┐
              │ACQUIRING │─── SD init failed ──┐
              └────┬─────┘                     │
                   │                           │
              SD mounted OK                    │
                   │                           │
                   ▼                           │
              ┌──────────┐                     │
              │UPLOADING │                     │
              │(max X min)│                    │
              └──┬────┬──┘                     │
                 │    │                        │
        all done │    │ X min expired          │
        (COMPLETE)│   │                        │
                 │    ▼                        │
                 │ ┌──────────┐                │
                 │ │RELEASING │◄───────────────┘
                 │ └────┬─────┘
                 │      │
                 │      ▼
                 │ ┌──────────┐
                 │ │ COOLDOWN │
                 │ │ (Y min)  │
                 │ └────┬─────┘
                 │      │
                 │      │ still in window? ──No──► IDLE
                 │      │
                 │      ▼
                 │    LISTENING (back to inactivity check)
                 │
                 ▼
              ┌──────────┐
              │ COMPLETE │──► mark day completed ──► IDLE
              └──────────┘
```

In scheduled mode, when the upload window opens, the FSM transitions from IDLE to
LISTENING **even if all known files are marked complete**. This ensures new data
written by the CPAP since the last upload is discovered during the scan. After the
scan confirms no new data exists, the day is marked completed → IDLE.

### 2.3 State Descriptions

| State | Description | Duration |
|---|---|---|
| **IDLE** | Scheduled mode only. Waiting for upload window to open. Checks periodically (every 60s). Not used in smart mode. | Until window opens (scheduled) |
| **LISTENING** | PCNT sampling bus activity. Tracking consecutive silence. | Until Z seconds of silence (default 125s — see 01-FINDINGS.md §6) |
| **ACQUIRING** | Taking SD card control, initializing SD_MMC. | ~500ms (brief transition) |
| **UPLOADING** | ESP has exclusive SD access. Upload runs as a **FreeRTOS task on Core 0**, keeping the web server responsive on Core 1. No periodic SD releases. Cloud imports are finalized per DATALOG folder. | Up to X minutes or until all files done |
| **RELEASING** | Finishing current file, unmounting SD, releasing mux to host. | ~100ms (brief transition) |
| **COOLDOWN** | Card released to CPAP. Non-blocking wait. Web server still responsive. | Y minutes |
| **COMPLETE** | All files uploaded for this cycle. | Transition state |
| **MONITORING** | Manual mode: all uploads stopped, live PCNT data displayed in web UI. | Until user clicks "Stop Monitor" |

---

## 3. Data Categorization

### 3.1 Data Tracking & Freshness Logic

The firmware uses different tracking strategies to balance reliability and performance:

- **Recent DATALOG folders** (within `RECENT_FOLDER_DAYS`):
  - **Tracking:** Individual files are tracked by **Size Only**.
  - **Storage:** RAM-only (transient). State is cleared on reboot to force a re-scan.
  - **Logic:** If file size increases (new data), it is re-uploaded.
  - **Limit:** Consumes slots in `MAX_FILE_ENTRIES` (250).

- **Old DATALOG folders** (older than `RECENT_FOLDER_DAYS`):
  - **Tracking:** Tracked by **Folder Name** only.
  - **Storage:** Persistent (saved to SD card).
  - **Logic:** Once a folder is marked complete, it is never scanned again.
  - **Limit:** Consumes slots in `MAX_COMPLETED_FOLDERS` (368 days of history).

- **Root Files** (`STR.edf`, `Identification.*`):
  - **Tracking:** None (Always Upload).
  - **Logic:** These files are **mandatory** for every cloud import cycle to ensure correct device association. They are small and uploaded every time data is sent.

- **Settings Files** (`/SETTINGS/*`):
  - **Tracking:** Tracked by **Size OR Checksum**.
  - **Storage:** Persistent.
  - **Logic:** Uploaded only if changed.
  - **Limit:** Consumes slots in `MAX_FILE_ENTRIES`.

- **MAX_DAYS cutoff**: Folders older than `MAX_DAYS` are completely ignored.

### 3.2 New: Fresh vs Old Data Split

The existing `isRecentFolder()` / `RECENT_FOLDER_DAYS` (B) concept maps directly to the
fresh/old data split:

| Category | Criteria | Scheduling |
|---|---|---|
| **Fresh** | `isRecentFolder(folderName) == true` (within B days) | Smart: anytime. Scheduled: window only. |
| **Per-import mandatory (cloud)** | Root/SETTINGS files | Uploaded during each cloud import finalization cycle |
| **Old** | `isRecentFolder(folderName) == false` AND within MAX_DAYS | Both modes: window only |
| **Ignored** | Older than MAX_DAYS | Never uploaded |

### 3.3 Upload Completeness Rule

**For cloud imports, root/SETTINGS files are MANDATORY per import cycle.**

Current behavior finalizes each DATALOG folder as a separate cloud import cycle:
1. Upload DATALOG files for one folder
2. Upload mandatory root files + SETTINGS files (`force=true`)
3. `processImport()`

**Strict Dependency:** Cloud imports are **only** created if at least one file in a DATALOG folder is successfully uploaded. If a scan finds no new/changed DATALOG files, **no import is created**, and therefore no Settings or Root files are uploaded. This ensures that valid imports always contain therapy data.

This avoids partial imports and constrains per-import memory lifetime. The X-minute
timer still gates DATALOG folder traversal, but any active import is finalized before
ending the session.

### 3.4 Upload Priority Order

Within each upload session (UPLOADING state):

1. **Fresh DATALOG folders** (newest first) — timer X applies
2. **Old DATALOG folders** (newest first, only in window) — timer X applies
3. **For each uploaded folder**: finalize cloud import with mandatory root/SETTINGS,
   then process import

This per-folder finalization pattern is the current design for cloud reliability and
heap stability.

---

## 4. Heap Management & Recovery System

### 4.1 Memory Fragmentation Challenge
The ESP32's heap becomes fragmented during extended upload sessions, especially with mixed SMB+CLOUD operations. TLS connections, response parsing, and state management all contribute to reduced contiguous heap (`max_alloc`).

### 4.2 Automatic Recovery System
```cpp
const uint32_t SD_MOUNT_MIN_ALLOC = 45000;
if (ESP.getMaxAllocHeap() < SD_MOUNT_MIN_ALLOC) {
    LOG("[FSM] Heap fragmented — fast-reboot to restore heap");
    esp_restart();
}
```

**Recovery Process:**
1. **Detection**: Monitor `max_alloc` after each upload pass
2. **Threshold**: Reboot if `max_alloc < 45KB` (insufficient for SD mount)
3. **Fast-boot**: `esp_reset_reason() == ESP_RST_SW` skips stabilization delays
4. **Seamless**: Upload state preserved, user unnoticeable

### 4.3 Staged Backend Processing
To minimize heap pressure:
1. **SMB Pass**: Process while heap is fresh (~73KB max_alloc)
2. **Teardown**: Destroy SMB uploader, reclaim 8KB buffer
3. **Cloud Pass**: Process with optimized TLS handling
4. **Benefit**: Isolates memory pressure between backends

### 4.4 Pre-flight Scans
Before any network activity:
```cpp
bool hasWork = !scanDatalogFolders().empty() || mandatoryChanged;
if (!hasWork) {
    LOG("[Backend] nothing to upload — skipping");
}
```

**Benefits:**
- Avoids OAuth when no Cloud files exist
- Prevents SMB connection when nothing needs upload
- Saves heap for actual upload operations

---

## 5. Upload Session Flow (UPLOADING State Detail)

```
UPLOADING state entered
    │
    ├─ Start exclusive access timer (X minutes)
    │
    ├─ Phase 1: Fresh DATALOG folders (newest first)
    │   ├─ For each folder:
    │   │   ├─ Check timer: X minutes expired? → finalize active import, exit
    │   │   ├─ Upload changed files in folder
    │   │   ├─ Mark folder completed
    │   │   └─ Cloud: finalize import for this folder
    │   │       (mandatory root + SETTINGS upload, then processImport)
    │   └─ End phase
    │
    ├─ Phase 2: Old DATALOG folders (only if in upload window)
    │   ├─ Same per-folder logic as Phase 1
    │   └─ End phase
    │
    └─ Save state once per session boundary and return COMPLETE or TIMEOUT
```

### 4.1 Upload Efficiency Optimizations (Single-Pass)

1.  **Single-Pass Streaming**: The `SleepHQUploader` no longer reads the file twice (once for MD5, once for upload).
    - It calculates the MD5 hash **while streaming** the file content to the TLS socket.
    - It sends the `content_hash` field in the **multipart footer** (accepted by SleepHQ API).
    - *Impact*: Reduces SD card I/O by 50% per file and eliminates pre-upload delays.

2.  **TLS Connection Reuse**: The uploader implements a custom chunked transfer decoder to properly drain API responses.
    - This allows the `WiFiClientSecure` connection to remain clean and reusable (HTTP keep-alive).
    - *Impact*: Eliminates the 1-2 second TLS handshake overhead for every file after the first one.

3.  **Memory Stability**: All uploads use the streaming path with small fixed buffers (4KB).
    - The RAM-heavy in-memory path for small files was removed.
    - Upload state uses v2 bounded structures + incremental persistence:
      - fixed-size in-memory arrays for folders/retry/file fingerprints
      - separate state files per backend: `.upload_state.v2.smb`/`.cloud` + `.log`
      - append-only journal with periodic snapshot compaction
      - recent DATALOG uses size-only tracking
      - per-file immediate state saves were removed from `uploadSingleFile()`
    - *Impact*: predictable RAM footprint and lower heap fragmentation risk during TLS-heavy sessions.

**Key rules**:
- No `checkAndReleaseSD()` calls during upload. The ESP holds exclusive access for the
  entire session. This eliminates the ~1.5 second penalty per release cycle.
- **For cloud uploads, root/SETTINGS are mandatory for each finalized import cycle.**
  They are small, fast, and required for import validity.
- The X-minute timer controls DATALOG folder traversal. If timer expires, the uploader
  finalizes any active cloud import, saves state, and exits the session cleanly.

---

## 5. Smart Mode Continuous Loop

Smart mode does **not** use a separate re-scan step. Instead, the continuous loop
(LISTENING → ACQUIRING → UPLOADING → RELEASING → COOLDOWN → LISTENING) naturally
handles new data discovery:

```
COMPLETE (all known files uploaded)
    │
    ├─ RELEASING (release SD card)
    ├─ COOLDOWN (Y minutes — CPAP gets uninterrupted access)
    ├─ LISTENING (wait for Z seconds of bus silence)
    ├─ ACQUIRING (take SD card)
    ├─ UPLOADING (scan SD card — discovers any new folders/files written since last cycle)
    │   ├─ New data found → upload it → RELEASING → COOLDOWN → loop continues
    │   └─ No new data → COMPLETE (nothing uploaded) → RELEASING → COOLDOWN → loop continues
    │
    └─ (loop never exits — always returns to LISTENING after cooldown)
```

This handles all scenarios where the CPAP writes new data between upload cycles (e.g.,
therapy summary files written after mask-off, second therapy session starting later).

The cooldown period (Y minutes, default 10) ensures the CPAP gets adequate
uninterrupted SD card access between cycles. The inactivity check (Z seconds, default
125) ensures the CPAP is not actively writing when the ESP takes the card.

---

## 6. Progressive Web App Interface

### 6.1 PWA Architecture
The web interface is implemented as a Progressive Web App with pre-allocated buffers to prevent heap fragmentation during uploads.

### 6.2 Buffer Management
```cpp
// All HTML/JS content generated at startup
const char* getMainPageHtml() {
    return R"(<!DOCTYPE html><html>...</html>)";
}

// Pre-allocated JSON buffer for status
char statusBuffer[1024];
snprintf(statusBuffer, sizeof(statusBuffer), "{\"state\":\"%s\"...}", state);
```

**Benefits:**
- No runtime memory allocation during uploads
- Consistent memory usage patterns
- Reliable operation under heap pressure

### 6.3 Real-time Features
- **Auto-refresh**: 5-second page reload for dashboard
- **Live status**: JSON API for custom monitoring
- **Progress tracking**: Real-time upload progress bars
- **Manual controls**: Upload triggers, state reset, soft reboots

### 6.4 Rate Limiting
```cpp
bool isUploadUiRateLimited(slot, uploadInProgress, minInterval) {
    // Prevent server overload during uploads
    return uploadInProgress && (now - lastServed[slot]) < minInterval;
}
```

---

## 7. SD Activity Monitor (Web UI Feature)

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
- If currently UPLOADING → finish current file + mandatory root/SETTINGS files, release SD card, then enter MONITORING
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
| `Config` | Uses FSM timing params only: UPLOAD_MODE, START/END_HOUR, INACTIVITY_SECONDS, EXCLUSIVE_ACCESS_MINUTES, COOLDOWN_MINUTES. Legacy timing keys are unsupported. |
| `ScheduleManager` | Rewrite: support upload window (two hours), upload mode enum, `isInUploadWindow()` replacing `isUploadTime()`. |
| `SDCardManager` | Remove `digitalRead(CS_SENSE)` check from `takeControl()`. Activity detection moves to TrafficMonitor (called from main FSM). |
| `FileUploader` | Remove `checkAndReleaseSD()`. Add phase-based upload with per-folder cloud finalization and session-level state save cadence. |
| `UploadStateManager` | Add size-first change detection, size-only tracking for recent DATALOG re-scan, and pruning of legacy DATALOG checksum entries to limit state size/heap churn. |
| `CPAPMonitor` | Replace stub with TrafficMonitor integration. |

### 7.2 New Components

| Component | Purpose |
|---|---|
| **TrafficMonitor** | PCNT-based bus activity detection on GPIO 33. Provides `isBusy()`, `getConsecutiveIdleMs()`, rolling sample buffer for monitoring UI. |
| **UploadFSM** (in main.cpp or new class) | The state machine driving IDLE → LISTENING → UPLOADING → COOLDOWN → MONITORING cycle. |

### 7.3 Components Unchanged

- `WiFiManager` — no changes
- `SMBUploader` / `WebDAVUploader` — no architectural changes
- `Logger` — no changes
- `TestWebServer` — updates: expose FSM state, SD activity monitor page/endpoint, remove SD release status

`SleepHQUploader` and `UploadStateManager` received memory-stability changes and should
not be treated as unchanged.

---

## 8. Timing Example

Configuration: `Z=125s, X=5min, Y=10min, B=2, START=8, END=22, MODE=smart`

> Z=125s is based on preliminary observations (see 01-FINDINGS.md §6): CPAP writes
> every ~60s during therapy (max idle ~58s), but idles for 3+ minutes outside therapy.
> 125s provides a 2× safety margin above therapy writes.

```
Timeline (smart mode, morning after therapy):

07:00  CPAP therapy ends, mask off
07:00  CPAP writes final summary files
07:01  Bus goes silent (CPAP idle)
07:01  ESP in LISTENING state (fresh data eligible anytime)
07:03  ~125 seconds of silence confirmed (Z=125s)
07:03  → ACQUIRING → SD mounted
07:03  → UPLOADING: uploading last 2 days of DATALOG + root files
07:08  X=5min timer expires, current file finishes
07:08  → RELEASING → COOLDOWN
07:18  Y=10min cooldown complete
07:18  → LISTENING (smart mode always returns here)
07:20  Z=125s silence confirmed
07:20  → ACQUIRING → UPLOADING: resume remaining fresh files
07:23  All fresh files done → COMPLETE → RELEASING → COOLDOWN
07:33  Cooldown complete → LISTENING
07:35  Z=125s silence → ACQUIRING → UPLOADING: scan finds no new fresh data
07:35  → COMPLETE → RELEASING → COOLDOWN
07:45  Cooldown complete → LISTENING (loop continues...)
08:00  Upload window opens (START=8) — old data now eligible
08:02  Z=125s silence → ACQUIRING → UPLOADING: old DATALOG folders
08:07  X=5min timer → RELEASING → COOLDOWN
...    (cycle continues until all old data uploaded)
10:00  All files uploaded → COMPLETE → RELEASING → COOLDOWN
10:10  Cooldown → LISTENING → scan finds nothing → COMPLETE → COOLDOWN → ...

--- Second therapy session scenario ---
14:00  User starts second therapy session (mask on)
14:00  CPAP writes to SD card every ~60s
14:00  ESP in LISTENING but bus never stays silent for Z=125s (therapy writes)
       → FSM stays in LISTENING, cannot acquire (correct behavior)
16:00  User removes mask, CPAP writes final summary
16:01  Bus goes silent
16:03  Z=125s silence confirmed → ACQUIRING → UPLOADING
16:03  Scan discovers new DATALOG folder from second session → uploads it
16:05  → COMPLETE → RELEASING → COOLDOWN → LISTENING → ...
```

---

## 9. Current Decisions (Resolved)

1. `UPLOAD_START_HOUR`/`UPLOAD_END_HOUR` define the allowed upload window.
2. Cross-midnight windows are supported.
3. Smart mode remains a continuous loop (no dedicated re-scan state).
4. Web "Upload Now" is a manual override and may skip inactivity wait.
5. Legacy upload timing flags are not supported; use FSM keys (`UPLOAD_MODE`, `UPLOAD_START_HOUR`, `UPLOAD_END_HOUR`, `INACTIVITY_SECONDS`, `EXCLUSIVE_ACCESS_MINUTES`, `COOLDOWN_MINUTES`).
6. `RECENT_FOLDER_DAYS` defines fresh vs old; `MAX_DAYS` is hard cutoff.
7. Heap recovery is handled by allocation-churn reduction, not reboot orchestration.
