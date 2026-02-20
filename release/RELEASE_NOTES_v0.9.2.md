# CPAP Data Uploader v0.9.2 Release Notes

## Overview

Reliability release fixing three user-reported upload failures and improving boot
behaviour. Smart mode now correctly detects and uploads fresh therapy data after
every session. Scheduled mode no longer reboots the device when a manual upload is
triggered outside the upload window. A new `DEBUG` config option is available for
verbose diagnostics without re-flashing.

---

## Bug Fixes

### 🩺 Smart Mode: Fresh Therapy Data Not Uploaded — Fully Resolved

**Symptom:** After a therapy session the device started an upload, reported
"no work for any backend — skipping session", and uploaded nothing — even though
new EDF files were present on the SD card. The issue was consistent and
reproducible across multiple reboots.

Two root causes were diagnosed and fixed in `preflightFolderHasWork()`:

#### Root Cause 1 — Path mismatch in file-change detection for recently-completed folders

**Cause:** `scanFolderFiles()` returns bare filenames (e.g.
`20260220_094713_EVE.edf`), but `hasFileChanged()` expects full paths (e.g.
`/DATALOG/20260219/20260220_094713_EVE.edf`) — it hashes the path to look up
stored state. The bare-filename hash never matched any stored entry, so
`findFileIndex()` returned -1. The function then attempted
`sd.open("20260220_094713_EVE.edf")` at the filesystem root (which doesn't
exist), returning `false` (not changed). Result: every recently-completed folder
always reported "no file changes" regardless of how many new EDF files the CPAP
had written since the last upload.

**Fix:** Each bare filename is now prepended with the full folder path before
calling `hasFileChanged()`.

---

#### Root Cause 2 — Pending folders silently skipped by pre-flight scan

**Cause:** Folders in `pending` state (seen as empty in a previous scan) were
matched by neither the `!completed && !pending` nor the `completed && recent`
branch in the pre-flight scan. If the CPAP subsequently wrote therapy files into
such a folder, the pre-flight never detected them and no upload was triggered.

**Fix:** An explicit `!completed && pending` branch now scans for files. If any
are found, the same `canUploadOldData()` gate is applied and the folder counts
as upload work.

---

### 📅 Scheduled Mode: Manual Upload Trigger Outside Window Caused Reboot

**Symptom:** Clicking "Trigger Upload" in the web UI while in scheduled mode and
outside the configured upload window caused the device to acquire the SD card, do
nothing useful, then reboot — confusing and potentially disruptive.

**Fix:** `handleTriggerUpload()` now immediately rejects the request when in
scheduled mode and outside the upload window, returning a clear JSON status
response. The web UI shows an amber warning toast explaining the situation.
No SD card access, no reboot.

---

### ♻️ Empty Session Folders Stuck in Pending State Indefinitely

**Symptom:** CPAP session folders containing no therapy data (e.g. machine was
powered on but no mask was worn) remained in `pending` state indefinitely, even
after the 7-day timeout elapsed.

**Cause:** The `pending → completed` promotion only ran inside
`scanDatalogFolders()` / `handleFolderScan()`, which only execute during active
upload sessions triggered by *other* work. If the empty folder was the only
remaining item, no session was ever triggered, and the promotion code never ran.

**Fix:** The promotion now happens proactively inside `preflightFolderHasWork()`.
When a pending folder is still empty and its 7-day timeout has expired, it is
promoted to completed immediately — no upload session required, no network I/O.

---

## Improvements

### ⚙️ Boot Timing

- **Cold boot:** Electrical stabilisation delay increased from 2 s to 15 s to
  give the SD bus MUX time to fully settle before first SD access.
- **Smart mode activity detection:** Silence threshold increased from 3 s → 5 s;
  maximum wait increased from 20 s → 45 s. Reduces false-positive "SD is free"
  detection during CPAP shutdown sequences.
- **Soft-reboot:** The smart wait now also runs on soft-reboots; previously the
  improved thresholds were skipped entirely after a reboot.

---

### 🔍 DEBUG Config Option

A new `DEBUG = true` key can be added to `config.txt` to enable verbose
diagnostics at runtime without re-flashing:

- Per-folder pre-flight scan lines:
  `[FileUploader] Pre-flight scan: folder=20260219 completed=1 pending=0 recent=1`
- `[res fh= ma= fd=]` heap and resource suffix on every log line

Both are **silent by default** (`DEBUG = false`). Useful for diagnosing upload
scheduling issues without modifying firmware.

See `docs/CONFIG_REFERENCE.md` for the full key reference.

---

### 🗑️ BOOT_DELAY_SECONDS Removed

`BOOT_DELAY_SECONDS` has been removed from the configuration. It was parsed but
could never be applied — the delay runs before SD card access, which is required
to read the config file (chicken-and-egg). If present in `config.txt` it is now
silently ignored.

---

## Upgrade Notes

- No configuration file changes required.
- `DEBUG = false` by default — no change in log verbosity for existing setups.
- `BOOT_DELAY_SECONDS` in `config.txt` is silently ignored; no need to remove it.
- Upload state files do not need to be reset.

---

## Commits in this Release

| Hash | Description |
|---|---|
| `590f074` | Promote expired pending folders to completed during pre-flight |
| `bb7a9f5` | Rename TestWebServer → CpapWebServer (avoid ESP32 class collision) |
| `246cf43` | Fix pre-flight scan: path mismatch + pending folder skipping (Q4) |
| `58a8846` | Add DEBUG config option — runtime verbose log control |
| `6eea701` | Rename TestWebServer → CpapWebServer/WebServer throughout codebase |
| `ebe0551` | Boot timing, BOOT_DELAY_SECONDS removal, scheduled trigger guard |
