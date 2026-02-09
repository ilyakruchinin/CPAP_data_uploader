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
  │    └─ Upload new/changed files (by MD5 checksum)
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
  ├─ List /DATALOG directory
  │    └─ Each subfolder is named YYYYMMDD (e.g., 20260210)
  │
  ├─ For each folder:
  │    │
  │    ├─ MAX_DAYS filter (if MAX_DAYS > 0 and NTP synced):
  │    │    └─ Folder date < (today - MAX_DAYS)? ──► SKIP
  │    │
  │    ├─ Completed folder check:
  │    │    ├─ Not completed? ──► INCLUDE
  │    │    ├─ Completed AND within RECENT_FOLDER_DAYS? ──► INCLUDE (re-check)
  │    │    └─ Completed AND older? ──► SKIP
  │    │
  │    └─ Pending folder check:
  │         ├─ Empty folder? ──► Mark as pending (timestamp)
  │         ├─ Pending > 7 days still empty? ──► Mark completed (promote)
  │         └─ Pending with new files? ──► INCLUDE (promote to active)
  │
  ├─ Sort: newest date first (prioritize recent data)
  │
  └─ Return folder list


scanFolderFiles(folderPath)
  │
  ├─ List all files in folder
  │
  ├─ For each file:
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
uploadSingleFile(filePath)
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
  │         ├─ Skip if cloudImportFailed = true
  │         ├─ Connect if needed (OAuth auth)
  │         ├─ Compute content_hash = MD5(content + filename)
  │         ├─ Size-lock file (snapshot size at hash time)
  │         └─ Multipart POST with content_hash + file data
  │              ├─ File ≤ 48KB: in-memory assembly
  │              └─ File > 48KB: streaming upload
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
  │    └─ Extract team_id from response
  │
  ├─ Create import:
  │    ├─ POST /api/v1/team/{team_id}/imports
  │    │    body: device_id, programmatic=true
  │    ├─ Parse import_id from response
  │    └─ Set cloudImportCreated = true
  │
  └─ On any failure:
       ├─ Set cloudImportFailed = true
       └─ Log warning (cloud skipped, other backends continue)


For each file:
  │
  ├─ Compute content_hash:
  │    ├─ Open file, snapshot size
  │    ├─ MD5 hash exactly snapshotSize bytes
  │    ├─ Append filename to hash
  │    └─ Return hex digest + hashedSize
  │
  ├─ Upload file:
  │    POST /api/v1/team/{team_id}/imports/{import_id}/files
  │    Content-Type: multipart/form-data
  │    Parts: name, path, content_hash, file (size-locked to hashedSize)
  │
  └─ SleepHQ deduplication:
       └─ If content_hash already exists on server ──► file skipped (HTTP 200)


Session End
  │
  └─ processImport():
       ├─ PUT /api/v1/team/{team_id}/imports/{import_id}/process
       └─ Triggers server-side data processing
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

The device shares the SD card with the CPAP machine using a cooperative time-slicing approach.

```
┌──────────────────────────────────────────────────┐
│              SD Card Timeline                     │
│                                                   │
│  CPAP owns card ──► Device takes control          │
│                      │                            │
│   ┌──────────────────┼────────────────────────┐   │
│   │  Upload Session  │                        │   │
│   │                  ▼                        │   │
│   │  ┌─ Upload file(s) ─┐                    │   │
│   │  │  (2 sec default)  │                    │   │
│   │  └──────┬────────────┘                    │   │
│   │         │                                 │   │
│   │         ▼                                 │   │
│   │  ┌─ Release SD card ─┐                   │   │
│   │  │  (500 ms default)  │ ◄─ CPAP can      │   │
│   │  │                    │    read/write     │   │
│   │  └──────┬─────────────┘                   │   │
│   │         │                                 │   │
│   │         ▼                                 │   │
│   │  ┌─ Reclaim SD card ─┐                   │   │
│   │  │  Upload more files │                   │   │
│   │  └──────┬─────────────┘                   │   │
│   │         │                                 │   │
│   │         ▼                                 │   │
│   │  ... repeat until budget exhausted ...    │   │
│   │         │                                 │   │
│   │         ▼                                 │   │
│   │  Release SD card ──► CPAP owns card       │   │
│   └───────────────────────────────────────────┘   │
│                                                   │
│  Timing (configurable):                           │
│  SD_RELEASE_INTERVAL_SECONDS: how long to hold    │
│  SD_RELEASE_WAIT_MS: how long to release          │
│  SESSION_DURATION_SECONDS: total session budget   │
└──────────────────────────────────────────────────┘
```

---

## See Also

- [CONFIGURATION.md](CONFIGURATION.md) — Complete configuration reference
- [DEVELOPMENT.md](DEVELOPMENT.md) — Developer guide and architecture
- [FEATURE_FLAGS.md](FEATURE_FLAGS.md) — Build-time feature selection
