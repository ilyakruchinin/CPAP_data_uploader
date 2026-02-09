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

### Test & Mock Fixes
- **`test/mocks/MockFS.h`** — Added `indexOf(const char*)`, `indexOf(const String&)`, and `lastIndexOf(char)` overloads to mock `String` class. `Config.cpp` uses string-search `indexOf` for endpoint type detection.
- **`test/mocks/ArduinoJson.h`** — Added `containsKey()` method to `JsonDocumentBase`. Required by `Config::censorConfigFileWithDoc()`.
- **`test/mocks/MockPreferences.h`** — Replaced static class member `globalStorage` with function-local static (Meyer's singleton) to fix static destruction order crash (SIGQUIT) on process exit.
- **`test/test_config/test_config.cpp`** — Fixed `test_config_load_with_defaults` expected `SESSION_DURATION_SECONDS` from 30 to 300 (matching actual default). Updated `test_config_sleephq_endpoint` to include required `CLOUD_CLIENT_ID` for cloud endpoint validation.
