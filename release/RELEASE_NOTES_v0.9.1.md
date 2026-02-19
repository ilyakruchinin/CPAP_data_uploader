# CPAP Data Uploader v0.9.1 Release Notes

## Overview
Stability release that eliminates the persistent endless reboot loop affecting mixed SMB+Cloud configurations, and improves the web UI log viewer.

---

## Bug Fixes

### 🔁 Endless Reboot Loop — Fully Resolved

Three compounding root causes were diagnosed and fixed. Each caused the device to reboot on every boot cycle even when all data was already uploaded:

#### Root Cause 1 — Mandatory files counted as pre-flight work
**Symptom**: `smb_work=1 cloud_work=0` on every boot; session ran and uploaded nothing (0 files), then rebooted.

**Cause**: Files such as `STR.edf`, `Identification.json`, and SETTINGS are updated by the CPAP machine every time it regains SD card control. Including them in the pre-flight work check caused `hasWork=true` on every boot, even when all DATALOG folders were fully synced.

**Fix**: Mandatory root and settings files are no longer checked in `preflightFolderHasWork`. They are still uploaded during DATALOG-triggered sessions, but they cannot independently trigger a new session.

---

#### Root Cause 2 — Backend cycling freeze after redirect
**Symptom**: Every session showed `Cloud ts < SMB ts → Cloud selected`, immediately followed by `redirecting to SMB`. Timestamps never converged.

**Cause**: When the selected backend (CLOUD) had no work and was redirected to SMB, only SMB's session-start timestamp was written. CLOUD's timestamp remained frozen at its old value, so it was always selected as the "oldest" backend on every subsequent boot → always redirected → loop.

**Fix**: When a redirect occurs, a session-start summary is now written for **both** the redirected-to backend and the original (skipped) backend, keeping both timestamps in sync.

---

#### Root Cause 3 — Old incomplete folders counted as work outside upload window
**Symptom**: `smb_work=1` persisted at midnight (outside the 9:00–23:00 upload window) due to a folder that was uploaded to Cloud but not SMB. Session ran Phase 1 only (fresh folders, 0 files), then rebooted. Repeat indefinitely.

**Cause**: A folder from a previous date (`20260206`) had `completed=0, pending=0` for the SMB backend. `preflightFolderHasWork` treated it as genuine work. However, the actual upload session gates Phase 2 (old folders) with `scheduleManager->canUploadOldData()`, which returns false outside the upload window. Pre-flight detected work that the session could not perform → session started → 0 files uploaded → reboot.

**Fix**: `preflightFolderHasWork` now applies the same `canUploadOldData()` gate for non-recent incomplete folders. Old folders outside the upload window are silently skipped in the pre-flight. When the window opens, they are detected and uploaded normally.

---

### 🪵 Web UI Log Viewer — Deduplication & Reboot Detection

**Symptom**: Logs appeared duplicated on every poll cycle. Multiple `─── DEVICE REBOOTED ───` separators appeared after the first boot, making the log view unreadable.

**Root cause (duplicates)**: The `_appendLogs` function did not correctly track which lines had already been buffered. On each poll, the full server response was re-appended.

**Root cause (false reboots)**: The boot banner (`=== CPAP Data Auto-Uploader ===`) is always present in every server response (the ring buffer always starts from boot). The old reboot-detection logic triggered a new REBOOTED separator on every poll.

**Fixes**:
- `lastSeenLine` tracking: each poll only appends lines that appear after the last buffered line in the server response.
- Reboot detection now checks whether `lastSeenLine` appears *after* the boot banner in the new response. If it does, it's the same boot continuing — no separator inserted. Only a genuinely new boot (last seen line absent or before the banner) inserts the separator.

---

### 🪵 Log Tab — Trailing Empty Lines on Every Poll

**Symptom**: The "N lines buffered" counter incremented by 1 every 3 seconds even when the device was idle and no new log lines were produced.

**Cause**: Server responses always end with blank lines. `lastSeenLine` tracks the last *non-empty* line, so `slice(lastSeenPos+1)` returned those trailing empty strings as "new" on every poll.

**Fix**: Trailing empty/whitespace-only lines are now stripped from `newLines` before appending to `clientLogBuf`.

---

### 📋 Log Tab — Copy to Clipboard Button

Added a **"Copy to clipboard"** button in the Logs tab that exports the entire client-side log buffer as plain text (works with both modern Clipboard API and legacy `execCommand` fallback).

---

## Diagnostic Tooling Added

`preflightFolderHasWork` now emits per-folder diagnostic log lines when scanning:
```
[FileUploader] Pre-flight scan: folder=20260218 completed=1 pending=0 recent=1
[FileUploader] Pre-flight: WORK — folder 20260206 not completed/pending
[FileUploader] Pre-flight: WORK — file changed: /DATALOG/20260218/someFile.edf
```
These lines remain in the firmware to aid future diagnosis of upload scheduling issues.

---

## Upgrade Notes

- No configuration file changes required.
- Upload state files (`.backend_summary.smb`, `.backend_summary.cloud`, `.upload_state.*`) do not need to be reset — the new logic is compatible with existing state.
- If you have folders that were uploaded to Cloud but not SMB (or vice versa), they will be uploaded to the missing backend during the next upload window (9:00–23:00 by default).

---

## Commits in this Release

| Hash | Description |
|---|---|
| `9828a1c` | Fix endless reboot loop: advance both backend timestamps on redirect + fix log dupe separators |
| `0b9a1ca` | Fix endless loop: exclude mandatory files from pre-flight work check |
| `a609bd4` | Add pre-flight diagnostic logging to identify smb\_work=1 cause |
| `c5d2eae` | Fix endless loop: gate old-folder pre-flight work check with canUploadOldData() |
