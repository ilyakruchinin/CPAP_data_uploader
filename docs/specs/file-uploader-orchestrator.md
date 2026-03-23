# File Uploader Orchestrator

## Overview
The File Uploader (`FileUploader.cpp/.h`) is the central orchestrator that coordinates all upload operations across multiple backends (SMB, Cloud, WebDAV). It manages upload state, performs pre-flight scans, handles the complete upload lifecycle, and selects which backend to run each session via timestamp-based cycling.

## Core Architecture

### Dual-Backend Phased Session Strategy
Each upload session runs **both backends sequentially** in a single SD card mount:
1. **Phase 1: CLOUD** — TLS pre-warmed by `uploadTaskFunction` before SD mount (cleanest heap); falls back to on-demand connect if pre-warm failed. OAuth + import creation + folder uploads + finalize.
2. **Phase 2: SMB** — TLS torn down after cloud phase; clean lwIP sockets and more heap for libsmb2. Mandatory files + DATALOG folder uploads.
3. **Reboot** — FSM reboots after releasing the SD card to restore heap (unless `MINIMIZE_REBOOTS` or `NOTHING_TO_DO`)

### Pre-flight Scans
Before writing the session-start timestamp (and before any network activity), performs SD-only scans across **all configured backends**. Uses a **dedicated `preflightFolderHasWork()`** instead of `scanDatalogFolders()` to avoid several critical false-positive conditions that cause endless reboot loops:

```
preflightFolderHasWork() rules (evaluated per folder):
  1. not completed AND not pending AND (recent OR canUploadOldData()) → WORK
  2. completed AND recent → scan files; WORK only if ≥1 file changed size
  3. Everything else (old completed, pending, or old incomplete outside window) → skip
```

**Critical design constraints** — three root causes of endless loops, each addressed:

| Root cause | Fix |
|---|---|
| `scanDatalogFolders()` always includes recently-completed folders, making `hasWork=true` on every boot | Pre-flight uses dedicated `preflightFolderHasWork()` that only returns true for genuinely changed recent files |
| Mandatory files (STR.edf, Identification.*, SETTINGS) grow every time the CPAP writes to the SD card; including them in the pre-flight check triggers a new upload session on every boot | Mandatory/settings files are **never** counted as pre-flight work — they are uploaded as a bonus during DATALOG-triggered sessions only |
| Old incomplete folders (e.g. uploaded to Cloud but not SMB) are detected as `smb_work=1` by pre-flight, but Phase 2 of the session is gated by `canUploadOldData()` (upload window). Outside the window, pre-flight detects work that the session cannot perform → reboot → repeat | Old (`!recent`) incomplete folders in pre-flight are gated by the same `canUploadOldData()` call used in Phase 2 |

```cpp
if (!smbWork && !cloudWork) {
    return UploadResult::NOTHING_TO_DO;  // → FSM applies Early Suppression & enters COOLDOWN, no reboot
}
```
- Returns `UploadResult::NOTHING_TO_DO` when every backend is fully synced (or only out-of-window old work remains)
- Returns `UploadResult::COMPLETE` when an upload successfully exhausts all pending folders
- The FSM translates both results into `g_noWorkSuppressed = true`, applying "Early Suppression" to halt further SD card probing until new CPAP activity occurs or the scheduled time window opens
- The session-start summary is NOT written for NOTHING_TO_DO — cycling pointer does not advance
- FSM responds by entering `COOLDOWN` directly without an `esp_restart()`, preventing endless reboot cycles

## Key Features

### Intelligent Folder Scanning
- **Recent completed folders**: Always rescanned — CPAP may extend/add files. Per-file size tracking (`hasFileChanged`) skips unchanged files
- **Old completed folders**: Skipped entirely
- **Pending folders**: Tracked for when they acquire content
- **Fresh vs Old data**: Different scheduling rules

### Backend Cycling
- `selectActiveBackend(sd)` compares `sessionStartTs` from `.backend_summary.smb` and `.backend_summary.cloud`
- Backend with **oldest timestamp** is selected; ties go to SMB
- Missing summary file → treated as ts=0 (oldest possible) so never-run backends are prioritized
- Written at session START so the pointer advances even on crashes
- **Redirect balancing**: when pre-flight detects that the selected backend has no work but the other backend does, it redirects to the working backend AND also writes a session-start summary for the original backend. Without this, the original backend's timestamp would stay frozen, causing it to be selected as "oldest" every boot → perpetual redirect → endless loop

### Uploaders
- **SMBUploader**: Network share uploads with transport resilience
- **SleepHQUploader**: Cloud uploads with OAuth and import sessions
- **WebDAVUploader**: Placeholder for future implementation

### Upload State Management
- **UploadStateManager**: Tracks file/folder completion status
- **Snapshot + Journal**: Efficient state persistence
- **Size-only tracking**: Optimized for recent DATALOG files
- **Checksum tracking**: For mandatory/SETTINGS files

### Memory Optimization
- **Sequential backends**: CLOUD then SMB — TLS is released before libsmb2 starts, preventing concurrent socket/heap pressure
- **FATFS 512-Byte Sectors**: A 3-tiered workaround (in `platformio.ini` and `sdkconfig.project`) forces `CONFIG_WL_SECTOR_SIZE=512`. This shrinks the `FATFS` struct, preventing an 18KB contiguous heap drop during SD mount.
- **TLS pre-warm before SD mount**: mbedTLS gets first pick of unfragmented heap (~36KB contiguous). See `docs/specs/tls-prewarm-pcnt-recheck.md`
- **Safety TLS cleanup**: Before SMB phase, unconditional `resetConnection()` ensures pre-warmed-but-unused TLS doesn't conflict with libsmb2 (errno:9)
- **Soft reboot between sessions**: Restores full contiguous heap via `esp_restart()` (fast-boot path skips delays)
- **Buffer management**: Dynamic SMB buffer sizing (8KB, 4KB, 2KB, 1KB) based on the highly-stable `ma=36852` safe fragmentation floor.
- **TLS reuse**: Persistent connections for cloud operations within a phase

### Mark-Complete Strategy
- **Recent folders**: Always marked complete (per-file size entries track changed/new files for next rescan)
- **Old folders**: Only marked complete when ALL files uploaded — failed old folders are retried whole next session

## Upload Process Flow

### 1. Initialization
```cpp
bool begin(fs::FS &sd) {
    // Create state managers for each backend
    smbStateManager = new UploadStateManager(...);
    cloudStateManager = new UploadStateManager(...);
    
    // Create uploaders
    smbUploader = new SMBUploader(...);
    sleephqUploader = new SleepHQUploader(...);
}
```

### 2. Upload Execution (called by uploadTaskFunction after TLS pre-warm + PCNT re-check + SD mount)
```cpp
UploadResult runFullSession(SDCardManager* sdManager, int maxMinutes, DataFilter filter) {
    // Pre-flight: check ALL backends (SD-only, no network)
    if (!smbWork && !cloudWork) return UploadResult::NOTHING_TO_DO;
    
    // Phase 1: CLOUD (TLS may already be connected via pre-warm)
    if (cloudWork) {
        sleephqUploader->begin();  // OAuth + team + import (uses pre-warmed TLS)
        for (folder : freshFolders + oldFolders) uploadDatalogFolderCloud(...);
        finalizeCloudImport();
        sleephqUploader->resetConnection();  // Release TLS for SMB
    }
    
    // Safety: release any pre-warmed TLS that wasn't used (no cloud work)
    if (smbWork && sleephqUploader->isConnected()) resetConnection();
    
    // Phase 2: SMB (clean sockets, more heap for libsmb2)
    if (smbWork) {
        uploadMandatoryFilesSmb(...);
        for (folder : freshFolders + oldFolders) uploadDatalogFolderSmb(...);
    }
    return UploadResult::COMPLETE;
}
```

### 3. File Upload Logic
- **Mandatory files**: Identification.*, STR.edf, SETTINGS/
- **DATALOG folders**: Date-named folders with therapy data
- **Change detection**: Size comparison for recent files, checksums for critical files
- **Progress tracking**: Real-time status updates via WebStatus

## Advanced Features

### Transport Resilience (SMB)
- **Recoverable errors**: EAGAIN, EWOULDBLOCK, "Wrong signature"
- **WiFi cycling**: Reclaim poisoned sockets
- **Retry logic**: Up to 3 connect attempts with backoff
- **Connection reuse**: Per-folder to avoid socket exhaustion

### Cloud Import Management
- **Lazy import creation**: Only when files exist
- **Session reuse**: OAuth + team + import in one TLS session
- **Streaming uploads**: Stack-allocated multipart buffers
- **Low-memory handling**: Graceful degradation when max_alloc < 40KB

### Empty Folder Handling
- **7-day waiting**: Before marking empty folders complete
- **Pending tracking**: Monitors folders that acquire content
- **Automatic promotion**: From pending to normal processing

## Configuration Integration
- **Schedule Manager**: Enforces upload windows for old data
- **Traffic Monitor**: Detects SD bus activity for smart mode
- **Web Server**: Real-time progress monitoring
- **WiFi Manager**: Network connectivity for cloud operations

## Error Handling
- **Graceful degradation**: Skip failed backends, continue with others
- **State preservation**: Always save progress even on failures
- **Retry mechanisms**: Built into each backend
- **Timeout protection**: Per-file and per-session timeouts

### Progress Metric — Excluding Empty Folders
- `foldersTotal` = `done + incomplete` only — pending (empty) folders are excluded from the total
- Empty folders are reported separately as `folders_pending` and shown as a note in the GUI `(N empty)` 
- Progress bar = `done / (done + incomplete)` so empty folders never prevent reaching 100%

## Performance Optimizations
- **Pre-flight gating**: No network if nothing to upload; no session-start written either
- **Bulk operations**: Directory creation, batch uploads
- **Memory awareness**: Buffer sizing based on available heap
- **Connection reuse**: Persistent sessions where possible

## Integration Points
- **UploadFSM**: Main state machine spawns uploadTaskFunction which calls runFullSession
- **SDCardManager**: Provides SD access control
- **Config**: Supplies all backend and timing parameters
- **WebStatus**: Real-time progress reporting
- **All Backend Uploaders**: Orchestrates their operations
