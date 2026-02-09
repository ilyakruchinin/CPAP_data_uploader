# Changelog

## SleepHQ Cloud Upload Implementation (Phase 1-6)

### Files Modified:
- `include/Config.h` — Added cloud config fields (cloudClientId, cloudClientSecret, cloudTeamId, cloudBaseUrl, cloudDeviceId, maxDays, uploadIntervalMinutes, cloudInsecureTls), getters, hasCloudEndpoint()/hasSmbEndpoint() helpers, PREFS_KEY_CLOUD_SECRET
- `src/Config.cpp` — Cloud config parsing in loadFromSD(), cloud credential secure storage/migration/censoring, validation supporting CLOUD endpoint type, all new getters
- `include/SleepHQUploader.h` — Full header: OAuth state, import lifecycle, multipart upload, content hash, TLS setup
- `src/SleepHQUploader.cpp` — Complete implementation: OAuth password grant auth, team_id discovery, import create/process, MD5 content hash, multipart file upload (in-memory for <48KB, streaming for larger), GTS Root R4 CA cert embedded, insecure TLS fallback
- `include/FileUploader.h` — No changes needed (already had conditional SleepHQUploader member)
- `src/FileUploader.cpp` — Multi-backend dispatch (all active backends per file), import lifecycle hooks in start/endUploadSession, MAX_DAYS filtering in scanDatalogFolders(), SETTINGS directory scan fix (Issue #28), updated delta/deep scan to use hasSmbEndpoint()
- `src/main.cpp` — UPLOAD_INTERVAL_MINUTES support via millis() timer, lastIntervalUploadTime tracking
- `src/TestWebServer.cpp` — Cloud status in /status endpoint, cloud config in /config endpoint
- `platformio.ini` — Updated ENABLE_SLEEPHQ_UPLOAD flag description

### Key Design Decisions:
- ENDPOINT_TYPE supports "SMB", "CLOUD", or "SMB,CLOUD" (comma-separated for both)
- MAX_DAYS is global (affects all backends)
- TLS: GTS Root R4 CA cert embedded by default, CLOUD_INSECURE_TLS=true as fallback
- OAuth token cached in memory with 60s safety margin before re-auth
- Import lifecycle: createImport at session start, processImport at session end
- content_hash = MD5(file_content + filename) for SleepHQ deduplication
- Build flag: ENABLE_SLEEPHQ_UPLOAD (enabled in pico32 env)

---

## Bug Fix Batch — Code Review Findings (2026-02-09)

All bugs found during a full code review of the SleepHQ cloud upload implementation.

### Bug 1 (HIGH) — Unsigned underflow in `ensureAccessToken()`
- **File:** `src/SleepHQUploader.cpp` line 147
- **Issue:** `tokenExpiresIn - 60` underflows to ~4 billion when `tokenExpiresIn < 60` (both `unsigned long`). This prevents token refresh, causing auth failures.
- **Fix:** Add guard: `if (tokenExpiresIn <= 60 || elapsed >= (tokenExpiresIn - 60))`.

### Bug 2 (HIGH) — Empty checksum in `uploadSingleFile()` causes perpetual re-uploads
- **File:** `src/FileUploader.cpp` lines 1055-1068
- **Issue:** An empty string was passed to `markFileUploaded()` instead of a real checksum. Since `hasFileChanged()` computes a fresh MD5 and compares against the stored (empty) value, root/SETTINGS files were re-uploaded every session.
- **Fix:** Call `stateManager->calculateChecksum()` to compute the real checksum before storing it.

### Bug 3 (MEDIUM) — OAuth scope space not URL-encoded
- **File:** `src/SleepHQUploader.cpp` line 103
- **Issue:** `scope=read write` has an unencoded space in form-urlencoded body. Strict OAuth servers may only see `scope=read`.
- **Fix:** Changed to `scope=read+write`.

### Bug 4 (MEDIUM) — Empty `ENDPOINT_TYPE` passes config validation but FileUploader can't initialize
- **File:** `src/Config.cpp` lines 649-652
- **Issue:** When `ENDPOINT_TYPE` is empty, the legacy fallback accepts any non-empty `ENDPOINT`, but `FileUploader::begin()` creates no backend (nothing matches). Config says valid, uploader fails.
- **Fix:** Default empty `ENDPOINT_TYPE` to `"SMB"` for backward compatibility when `ENDPOINT` is set.

### Bug 5 (MEDIUM) — `budgetManager` pause/resume called during scans without active session
- **File:** `src/FileUploader.cpp` lines 1117, 1241
- **Issue:** Delta/deep scans call `checkAndReleaseSD()`, which calls `budgetManager->pauseActiveTime()` / `resumeActiveTime()`. No session was started, so `activeTimeMs` accumulates garbage from `millis() - 0`, corrupting budget state for the next real upload.
- **Fix:** Guard pause/resume in `checkAndReleaseSD` to skip budget tracking when `sessionDurationMs == 0` (no active session).

### Bug 6 (LOW) — `pico32-ota` environment missing `ENABLE_SLEEPHQ_UPLOAD` flag
- **File:** `platformio.ini` line 108
- **Issue:** The OTA build target doesn't include `-DENABLE_SLEEPHQ_UPLOAD`, silently dropping cloud upload support.
- **Fix:** Added the flag to `pico32-ota` build_flags.

### Security Fix — WiFi/endpoint passwords exposed via `/config` in plain text mode
- **File:** `src/TestWebServer.cpp` lines 575-590
- **Issue:** When credentials are NOT in flash (plain text mode), WiFi password and endpoint password were served in cleartext via the unauthenticated `/config` HTTP endpoint. `cloud_client_secret` was already hidden.
- **Fix:** Always censor passwords in `/config` output regardless of storage mode.

### Performance Fix — O(n²) streaming response body read
- **File:** `src/SleepHQUploader.cpp` lines 639-642
- **Issue:** Character-by-character `String` concatenation in streaming upload response reader causes O(n²) reallocation.
- **Fix:** Use `tlsClient->readString()` for bulk read.

---

## SleepHQ Upload Reliability Fixes (2026-02-10)

Fixes two production issues observed during live CPAP data uploads to SleepHQ.

### Fix 7 (HIGH) — SleepHQ rejects imports as "corrupted" (hash mismatch)
- **Files:** `include/SleepHQUploader.h`, `src/SleepHQUploader.cpp`
- **Issue:** `computeContentHash()` reads the file once, then `httpMultipartUpload()` reads it again. Between these two reads, the CPAP machine appends data to the file (especially during an active sleep session). SleepHQ computes its own hash of the received data, finds it doesn't match the `content_hash` field, and marks the import as failed ("One of the files in this import was corrupted during upload").
- **Fix:** Size-locked reads. `computeContentHash()` now snapshots the file size and hashes exactly that many bytes, returning the `hashedSize`. `httpMultipartUpload()` accepts a `lockedFileSize` parameter and reads exactly that many bytes for both in-memory and streaming upload paths. This guarantees the hash always matches the uploaded content, even if the CPAP appends data between or during reads.

### Fix 8 (HIGH) — Interval uploads silently skip all files after first successful session
- **File:** `src/main.cpp`
- **Issue:** `main.cpp` handles interval timing independently, but `uploadNewFiles()` has an internal `shouldUpload()` check that returns false after `markUploadCompleted()` is called. Every subsequent interval session entered `uploadNewFiles()` and returned immediately without uploading anything.
- **Fix:** When the interval timer fires, pass `forceUpload=true` to `uploadNewFiles()` to bypass the internal daily schedule check.

### Fix 9 (HIGH) — Changed files in completed DATALOG folders never re-uploaded
- **Files:** `include/FileUploader.h`, `src/FileUploader.cpp`, `include/Config.h`, `src/Config.cpp`
- **Issue:** Once a DATALOG folder was marked "completed", it was permanently skipped by `scanDatalogFolders()`. Files that grew during an active CPAP session (today's BRP, PLD, SA2 data) were never re-uploaded, even on subsequent interval runs.
- **Fix:** Added configurable `RECENT_FOLDER_DAYS` (default: 2). Completed folders within the recent window are re-scanned on each interval upload. Per-file MD5 checksums are stored via `UploadStateManager::markFileUploaded()` (reusing existing infrastructure). Only files whose checksum has changed are re-uploaded, making re-scans efficient.

### Improvement — Lazy cloud import creation (avoids empty imports on SleepHQ)
- **Files:** `include/FileUploader.h`, `src/FileUploader.cpp`
- **Issue:** `startUploadSession()` eagerly created a SleepHQ import session. If no files had changed (all checksums match), an empty import was submitted, cluttering the SleepHQ dashboard with "0 files, 0 bytes" entries.
- **Fix:** Import creation deferred to `ensureCloudImport()`, called lazily on first actual file upload. If no files need uploading, no import is created.

### Test & Mock Fixes
- **`test/mocks/MockFS.h`** — Added `indexOf(const char*)`, `indexOf(const String&)`, and `lastIndexOf(char)` overloads to mock `String` class. `Config.cpp` uses string-search `indexOf` for endpoint type detection.
- **`test/mocks/ArduinoJson.h`** — Added `containsKey()` method to `JsonDocumentBase`. Required by `Config::censorConfigFileWithDoc()`.
- **`test/mocks/MockPreferences.h`** — Replaced static class member `globalStorage` with function-local static (Meyer's singleton) to fix static destruction order crash (SIGQUIT) on process exit.
- **`test/test_config/test_config.cpp`** — Fixed `test_config_load_with_defaults` expected `SESSION_DURATION_SECONDS` from 30 to 300 (matching actual default). Updated `test_config_sleephq_endpoint` to include required `CLOUD_CLIENT_ID` for cloud endpoint validation.

---

## Bug Fix Batch — Code Review Round 2 (2026-02-10)

Fixes found during second code review pass of the SleepHQ cloud upload implementation.

### Fix 10 (HIGH) — Budget-exhaustion retry silently skips upload
- **File:** `src/main.cpp`
- **Issue:** `budgetExhaustedRetry` was cleared to `false` before `forceThisUpload` was computed. Since `forceThisUpload` depended on `budgetExhaustedRetry`, it was always `false` for budget retries. This caused `uploadNewFiles()` to call `shouldUpload()` internally, which returns `false` after `markUploadCompleted()` — silently skipping the retry.
- **Fix:** Introduced `isBudgetRetry` local variable to capture the retry state before clearing the flag.

### Fix 11 (HIGH) — Cloud import failure cascades into aborting SMB uploads
- **Files:** `include/FileUploader.h`, `src/FileUploader.cpp`
- **Issue:** When `ensureCloudImport()` failed (e.g., network timeout, auth error), the SleepHQ `upload()` call would still execute with no active import, fail, set `uploadSuccess = false`, and abort the entire folder — including SMB uploads that would have succeeded.
- **Fix:** Added `cloudImportFailed` flag. When `ensureCloudImport()` fails, the flag is set and the SleepHQ backend is skipped for the rest of the session, allowing SMB/WebDAV to proceed independently.

### Fix 12 (MEDIUM) — WebDAV not updated for comma-separated ENDPOINT_TYPE
- **Files:** `include/Config.h`, `src/Config.cpp`, `src/FileUploader.cpp`
- **Issue:** WebDAV used exact string match (`== "WEBDAV"`) while SMB and Cloud used `indexOf`-based helpers supporting comma-separated values. Setting `ENDPOINT_TYPE=WEBDAV,CLOUD` would activate Cloud but not WebDAV.
- **Fix:** Added `hasWebdavEndpoint()` helper (matching `hasSmbEndpoint()`/`hasCloudEndpoint()` pattern) and updated all WebDAV checks in `FileUploader.cpp`. Added WebDAV validation in `loadFromSD()`.

### Fix 13 (MEDIUM) — Unchecked file.read() in in-memory upload path
- **File:** `src/SleepHQUploader.cpp`
- **Issue:** `file.read()` return value was ignored in the in-memory multipart upload path (files ≤48KB). A short read (SD error, file truncation between hash and upload) would send uninitialized buffer data, causing a content-hash mismatch on the SleepHQ server.
- **Fix:** Check `bytesRead != fileSize` and return `false` with error log if short.

### Fix 14 (MEDIUM) — Fragile URL parser for streaming upload
- **File:** `src/SleepHQUploader.cpp`
- **Issue:** The streaming upload URL parser found the port separator (`:`) before stripping the path, so a colon in a URL path could be misinterpreted as a port separator.
- **Fix:** Reordered to strip path first, then search for port separator.

### Improvement — Cache endpoint type flags to avoid per-call string allocation
- **Files:** `include/Config.h`, `src/Config.cpp`
- **Issue:** `hasCloudEndpoint()`, `hasSmbEndpoint()`, and `hasWebdavEndpoint()` each created a temporary `String`, converted to uppercase, and searched it — on every call. These are called in hot upload loops on a memory-constrained ESP32.
- **Fix:** Cached the parsed booleans (`_hasSmbEndpoint`, `_hasCloudEndpoint`, `_hasWebdavEndpoint`) during `loadFromSD()`. Getters now return the cached values directly.
