# Upload Flow Diagrams

Visual reference for the upload lifecycle, scheduling, and decision-making logic in the ESP32 CPAP Data Uploader.

---

## Table of Contents

- [System Startup](#system-startup)
- [Main Loop](#main-loop)
- [Upload Session](#upload-session)
- [File Scanning](#file-scanning)
- [Multi-Backend Upload](#multi-backend-upload)
- [SleepHQ Cloud Import Lifecycle](#sleephq-cloud-import-lifecycle)
- [Credential Migration](#credential-migration)
- [SD Card Sharing](#sd-card-sharing)

---

## System Startup

```
Power On
  │
  ├─ Initialize hardware (SD card, serial)
  ├─ Load config.json from SD card
  │    ├─ Parse all settings
  │    ├─ Validate required fields (WIFI_SSID, endpoint config)
  │    └─ Credential migration (see Credential Migration below)
  │
  ├─ Apply power management settings
  │    ├─ Set CPU frequency (CPU_SPEED_MHZ)
  │    └─ Configure WiFi power (TX power, power saving)
  │
  ├─ Connect to WiFi
  │    └─ Retry until connected
  │
  ├─ Synchronize time via NTP
  │    └─ Required for scheduled uploads and MAX_DAYS filtering
  │
  ├─ Initialize upload backends
  │    ├─ SMB uploader (if ENDPOINT_TYPE contains "SMB")
  │    ├─ WebDAV uploader (if ENDPOINT_TYPE contains "WEBDAV")
  │    └─ SleepHQ uploader (if ENDPOINT_TYPE contains "CLOUD")
  │
  ├─ Load upload state (.upload_state.json)
  │    └─ Tracks completed folders, file checksums, retry counts
  │
  ├─ Start web server (if enabled)
  │    └─ Endpoints: /, /status, /config, /logs, /upload, /scan, /ota
  │
  ├─ Wait BOOT_DELAY_SECONDS (default: 30s)
  │    └─ Allows CPAP machine to initialize
  │
  └─ Enter main loop
```

---

## Main Loop

```
┌─────────────────────────────────────────────────────┐
│                    Main Loop                         │
│                                                      │
│  ┌──── Service web server requests ◄──────────┐     │
│  │                                             │     │
│  ▼                                             │     │
│  Budget-exhaustion retry pending?              │     │
│  ├─ YES: Wait period elapsed?                  │     │
│  │    ├─ NO ──► return (non-blocking) ─────────┘     │
│  │    └─ YES ─► clear flag, set isBudgetRetry=true   │
│  │              proceed to upload ──────────────►─┐  │
│  │                                                │  │
│  └─ NO: Check upload schedule                     │  │
│       │                                           │  │
│       ├─ UPLOAD_INTERVAL_MINUTES > 0?             │  │
│       │    ├─ YES: Interval elapsed?              │  │
│       │    │    ├─ NO ──► return ──────────────────┘  │
│       │    │    └─ YES ─► proceed to upload ──►─┐ │  │
│       │    │                                    │ │  │
│       │    └─ NO: Daily schedule mode           │ │  │
│       │         shouldUpload()?                 │ │  │
│       │         ├─ NO ──► return ───────────────┘ │  │
│       │         └─ YES ─► proceed to upload ─►─┐  │  │
│       │                                        │  │  │
│  ┌────┼────────────────────────────────────────┘  │  │
│  │    │                                           │  │
│  ▼    │    === Upload Window Active ===            │  │
│  Take SD card control                             │  │
│  ├─ FAIL: CPAP using card ──► retry in 5s ───────┘  │
│  │                                                   │
│  └─ OK: Run upload session                           │
│         │                                            │
│         ├─ forceUpload = isBudgetRetry OR interval   │
│         ├─ uploadNewFiles(forceUpload)               │
│         │                                            │
│         ├─ Release SD card                           │
│         │                                            │
│         └─ Upload result:                            │
│              ├─ Budget exhausted (incomplete)         │
│              │    └─ Set retry timer = 2× session     │
│              │       duration, loop back ─────────────┘
│              └─ Complete or error
│                   └─ Record timestamp, loop back ─────┘
│                                                      │
└──────────────────────────────────────────────────────┘
```

---

## Upload Session

```
uploadNewFiles(forceUpload)
  │
  ├─ Start session: initialize time budget
  │    └─ Budget = SESSION_DURATION_SECONDS
  │
  ├─ Scan DATALOG folders (newest first)
  │    ├─ Filter by MAX_DAYS (if set and NTP available)
  │    ├─ Skip completed folders (unless within RECENT_FOLDER_DAYS)
  │    └─ Return sorted list: newest date first
  │
  ├─ For each DATALOG folder:
  │    │
  │    ├─ Check time budget remaining
  │    │    └─ Exhausted? ──► save state, return (budget_exhausted)
  │    │
  │    ├─ Check retry count
  │    │    └─ Exceeded MAX_RETRY_ATTEMPTS? ──► skip folder
  │    │
  │    ├─ Scan files in folder
  │    │    └─ Filter: skip already-uploaded unchanged files
  │    │
  │    ├─ Upload each file to all active backends
  │    │    ├─ Periodically release SD card (SD_RELEASE_INTERVAL_SECONDS)
  │    │    ├─ Check time budget before each file
  │    │    └─ On failure: increment retry count, save state, return
  │    │
  │    └─ All files uploaded? ──► Mark folder completed
  │
  ├─ Scan root files (/, /SETTINGS)
  │    ├─ Cloud import active? ──► force-include ALL files
  │    │    (SleepHQ requires STR.edf, Identification.*, SETTINGS/)
  │    ├─ No cloud import? ──► upload only new/changed files (by MD5)
  │    └─ Changed files found + cloud endpoint? ──► re-scan with
  │         forceAll=true (Fix 18: ensure companion files included)
  │
  ├─ End session
  │    ├─ Process cloud import (if active)
  │    ├─ Save upload state
  │    └─ Mark upload completed for the day (daily mode)
  │
  └─ Return success/failure
```

---

## File Scanning

```
scanDatalogFolders()
  │
  ├─ [DIAG] Log current local date, yesterday's date
  ├─ [DIAG] Check: today's folder EXISTS/NOT FOUND on SD card
  ├─ [DIAG] Check: yesterday's folder EXISTS/NOT FOUND on SD card
  ├─ [DIAG] Raw /DATALOG listing (all entries, no filters)
  │
  ├─ List /DATALOG directory
  │    └─ Each subfolder is named YYYYMMDD (e.g., 20260210)
  │
  ├─ For each folder:
  │    │
  │    ├─ MAX_DAYS filter (if MAX_DAYS > 0 and NTP synced):
  │    │    └─ Folder date < (today - MAX_DAYS)? ──► SKIP
  │    │
  │    ├─ Completed folder check:
  │    │    ├─ Not completed? ──► INCLUDE (logged as NEW)
  │    │    ├─ Completed AND within RECENT_FOLDER_DAYS? ──► INCLUDE (re-check)
  │    │    └─ Completed AND older? ──► SKIP
  │    │
  │    └─ Pending folder check:
  │         ├─ Empty folder? ──► Mark as pending (timestamp)
  │         ├─ Pending > 7 days still empty? ──► Mark completed (promote)
  │         └─ Pending with new files? ──► INCLUDE (promote to active)
  │
  ├─ [DIAG] Log summary: total dirs, skipped old, skipped completed, to process
  ├─ Sort: newest date first (prioritize recent data)
  │
  └─ Return folder list


scanFolderFiles(folderPath)
  │
  ├─ List all files in folder
  ├─ [DIAG] Log ALL files with sizes (not just .edf)
  │
  ├─ For each .edf file:
  │    ├─ Already uploaded with same checksum? ──► SKIP
  │    └─ New or checksum changed? ──► INCLUDE
  │
  └─ Return file list


scanRootAndSettingsFiles()
  │
  ├─ Scan / (root) for known CPAP files
  ├─ Scan /SETTINGS directory
  │
  ├─ For each file:
  │    ├─ Already uploaded with same checksum? ──► SKIP
  │    └─ New or checksum changed? ──► INCLUDE
  │
  └─ Return file list
```

---

## Multi-Backend Upload

```
uploadSingleFile(filePath, forceUpload)
  │
  ├─ Check if file has changed (MD5 checksum)
  │    ├─ forceUpload = true? ──► skip check (cloud needs companion files)
  │    └─ Unchanged and not forced? ──► skip upload, return success
  │
  ├─ Lazily create cloud import (ensureCloudImport)
  │    ├─ Already created? ──► continue
  │    ├─ Cloud import failed this session? ──► skip cloud
  │    └─ Create new import? ──► OAuth auth + createImport()
  │
  ├─ Upload to each active backend:
  │    │
  │    ├─ SMB (if ENDPOINT_TYPE contains "SMB"):
  │    │    ├─ Connect if needed
  │    │    ├─ Create remote directories
  │    │    └─ Upload file via SMB protocol
  │    │
  │    ├─ WebDAV (if ENDPOINT_TYPE contains "WEBDAV"):
  │    │    ├─ Connect if needed
  │    │    └─ Upload file via HTTP PUT
  │    │
  │    └─ SleepHQ (if ENDPOINT_TYPE contains "CLOUD"):
         ├─ Skip if cloudImportFailed = true
         ├─ Full TLS client destroy/recreate (Fix 20: fight heap fragmentation)
         ├─ Connect if needed (OAuth auth, setReuse=false)
         ├─ Compute content_hash = MD5(content + filename)
         ├─ Size-lock file (snapshot size at hash time)
         ├─ Multipart POST with content_hash + file data
         │    ├─ Path format: "./DATALOG/20260210/" (API spec)
         │    ├─ File ≤ 48KB AND heap sufficient: in-memory assembly
         │    ├─ File ≤ 48KB BUT heap tight: fall through to streaming
         │    └─ File > 48KB: streaming upload (Connection: close)
         │
         └─ NOTE: Upload-time hash verification is impossible
              (SleepHQ echoes declared hash in 201 response regardless
               of data integrity — corruption detected only server-side
               during import processing)
  │
  ├─ All backends succeeded?
  │    ├─ YES: Mark file uploaded (store checksum)
  │    └─ NO: Return failure (file will be retried)
  │
  └─ Return success/failure
```

---

## SleepHQ Cloud Import Lifecycle

```
Session Start
  │
  │  (import creation is deferred until first file needs uploading)
  │
  ▼
ensureCloudImport() ◄── called on first file upload
  │
  ├─ cloudImportCreated? ──► return true (already done)
  ├─ cloudImportFailed? ──► return false (skip cloud this session)
  │
  ├─ Authenticate (if token expired):
  │    ├─ POST /oauth/token
  │    │    body: client_id, client_secret, grant_type=password, scope=read write
  │    ├─ Parse access_token, expires_in
  │    └─ Cache token with 60s safety margin
  │
  ├─ Discover team_id (if not configured):
  │    ├─ GET /api/v1/me (with Bearer token)
  │    └─ Extract current_team_id from response
  │
  ├─ Discover machine info (Fix 19):
  │    ├─ GET /api/v1/teams/{team_id}/machines
  │    └─ Log model, brand, serial_number, name
  │
  ├─ Discover device_id (if not configured — Fix 19):
  │    ├─ GET /api/v1/devices (static device TYPE catalog)
  │    ├─ Auto-match: brand="ResMed" + name contains "Series 11"
  │    └─ Fall back: log all device types for manual config
  │
  ├─ Create import:
  │    ├─ POST /api/v1/teams/{team_id}/imports
  │    │    body: device_id, name="{machineName} Auto-Upload"
  │    ├─ Parse import_id from response
  │    └─ Set cloudImportCreated = true
  │
  └─ On any failure:
       ├─ Set cloudImportFailed = true
       └─ Log warning (cloud skipped, other backends continue)


For each DATALOG file (Fix 22):
  │
  ├─ Size-based change detection (metadata only, no SD read):
  │    ├─ file.size() vs stored size from last upload
  │    ├─ Size unchanged ──► skip (DATALOG files are append-only)
  │    └─ Size changed or new file ──► needs upload
  │
  ├─ Single-read upload with progressive hash (Fix 22):
  │    ├─ Open file, lock size
  │    │
  │    ├─ In-memory path (≤48KB — most CPAP files):
  │    │    ├─ Allocate payload buffer
  │    │    ├─ Read file into buffer (single SD read)
  │    │    ├─ Compute MD5(content + filename) from buffer
  │    │    ├─ Assemble multipart: name → path → file → content_hash
  │    │    ├─ Release SD card
  │    │    └─ HTTP POST (CPAP has SD during entire upload)
  │    │
  │    └─ Streaming path (>48KB — large BRP/PLD files):
  │         ├─ Connect TLS, send HTTP headers + preamble
  │         ├─ Loop per chunk (~4KB):
  │         │    ├─ Take SD → read chunk → release SD
  │         │    ├─ MD5Update with chunk (CPU only)
  │         │    └─ Send chunk over TLS (CPAP has SD)
  │         ├─ Finalize MD5(content + filename) → hash
  │         ├─ Send content_hash as final multipart field
  │         └─ Wait for response (CPAP has SD)
  │
  │    Multipart body structure (hash AFTER file):
  │      name, path, file content, content_hash
  │
  │    ├─ HTTP 201 ──► new file uploaded
  │    └─ HTTP 200 ──► file already on server (dedup, logged as skipped)
  │
  ├─ Store file size for next-session change detection
  │    (no post-upload checksum read needed for DATALOG)
  │
  └─ Release SD + CPAP window between files (Fix 21)


For each root/SETTINGS file:
  │
  ├─ Hash-based change detection (reads file for MD5):
  │    └─ Content may change in-place, size alone insufficient
  │
  └─ Same single-read upload as above


Session End
  │
  └─ processImport():
       ├─ POST /api/v1/imports/{import_id}/process_files
       └─ Server-side hash validation detects any corruption
```

---

## Credential Migration

```
loadFromSD()
  │
  ├─ Parse config.json
  │
  ├─ STORE_CREDENTIALS_PLAIN_TEXT = true?
  │    └─ YES: Use plain text from config.json, done
  │
  └─ NO (secure mode):
       │
       ├─ For each credential (WIFI_PASS, ENDPOINT_PASS, CLOUD_CLIENT_SECRET):
       │    │
       │    ├─ Value in config.json = "***STORED_IN_FLASH***"?
       │    │    └─ YES: Load from ESP32 NVS flash
       │    │
       │    └─ NO (plain text in config):
       │         ├─ New credential detected
       │         └─ Prioritize config.json value over flash
       │
       ├─ Any new (non-censored) credentials found?
       │    ├─ NO: All loaded from flash, done
       │    └─ YES: Migrate new credentials
       │         │
       │         ├─ Store in ESP32 NVS flash
       │         ├─ Verify by reading back
       │         ├─ Rewrite config.json with ***STORED_IN_FLASH***
       │         └─ Log migration success
       │
       └─ Done (credentials in memory for runtime use)
```

---

## SD Card Sharing

The device shares the SD card with the CPAP machine using cooperative time-slicing, batch reads, and aggressive release during network I/O. The goal is to minimize SPI bus hold time so the CPAP can write therapy data uninterrupted.

**Important:** `SD_MMC.end()` in `releaseControl()` unmounts the entire filesystem, invalidating all open file handles. SD can only be released when no files are open.

```
┌──────────────────────────────────────────────────────────┐
│     SD Card Timeline — Batch Mode (cloud-only, Fix 23)   │
│                                                          │
│  CPAP owns card ──► ESP takes control (mount ~600ms)     │
│                      │                                   │
│   ┌──────────────────┼──────────────────────────────┐    │
│   │  Batch Read      │                              │    │
│   │                  ▼                              │    │
│   │  ┌─ Read N small files into 48KB buffer ──┐    │    │
│   │  │  File 1: read → buffer[0..768]         │    │    │
│   │  │  File 2: read → buffer[769..772]       │    │    │
│   │  │  File 3: read → buffer[773..3166]      │    │    │
│   │  │  ... all small files in single SD hold │    │    │
│   │  └──────┬────────────────────────────────┘     │    │
│   │         │                                      │    │
│   │         ▼                                      │    │
│   │  ┌─ Release SD card ──────────────────────┐   │    │
│   │  │  Upload all N files from RAM buffer:   │   │    │
│   │  │  uploadFromBuffer() × N (no SD needed) │   │    │
│   │  │  CPAP has SD for entire upload phase   │   │    │
│   │  └──────┬─────────────────────────────────┘   │    │
│   │         │                                      │    │
│   │         ▼                                      │    │
│   │  ┌─ Retake SD for state save ────────────┐    │    │
│   │  │  Mark all N files uploaded, save state │    │    │
│   │  └──────┬────────────────────────────────┘    │    │
│   │         │                                      │    │
│   │         ▼                                      │    │
│   │  ┌─ CPAP window (Fix 21) ────────────────┐   │    │
│   │  │  Release SD for SD_RELEASE_WAIT_MS     │   │    │
│   │  └──────┬─────────────────────────────────┘   │    │
│   │         │                                      │    │
│   │         ▼                                      │    │
│   │  ... next batch or large file ...              │    │
│   └────────────────────────────────────────────────┘    │
│                                                          │
│     SD Card Timeline — Streaming (>48KB files)           │
│                                                          │
│   ┌────────────────────────────────────────────────┐    │
│   │  Release SD during TLS handshake (~2-3s)       │    │
│   │  Retake SD → open file → stream all data       │    │
│   │  (SD held during file I/O — unavoidable)       │    │
│   │  Close file → release SD                       │    │
│   │  Send hash + wait for response (SD released)   │    │
│   └────────────────────────────────────────────────┘    │
│                                                          │
│  Session end:                                            │
│  Release SD → processImport() (network) → retake SD     │
│  Save state → print statistics → release SD              │
│                                                          │
│  Timing (configurable):                                  │
│  SD_RELEASE_INTERVAL_SECONDS: periodic release (2s)      │
│  SD_RELEASE_WAIT_MS: CPAP window duration (1500ms)       │
│  SESSION_DURATION_SECONDS: total session budget (300s)   │
│                                                          │
│  Effective CPAP SD access during uploads:                │
│  • Batched small files: ~95% (SD held only for reads)    │
│  • Streaming files: ~60% (SD held during file stream)    │
│  • Between batches/files: 100% for 1500ms guaranteed     │
│  • Session end: 100% during processImport()              │
└──────────────────────────────────────────────────────────┘
```

---

## See Also

- [CONFIGURATION.md](CONFIGURATION.md) — Complete configuration reference
- [DEVELOPMENT.md](DEVELOPMENT.md) — Developer guide and architecture
- [FEATURE_FLAGS.md](FEATURE_FLAGS.md) — Build-time feature selection
