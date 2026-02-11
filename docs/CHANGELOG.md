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

---

## Bug Fix Batch — Hardware Testing Round (2026-02-10)

Fixes found during live hardware testing of the SleepHQ cloud upload.

### Fix 15 (HIGH) — "Incomplete folders remain" infinite retry loop
- **File:** `src/FileUploader.cpp`
- **Issue:** `RECENT_FOLDER_DAYS` causes completed folders to be re-included in `scanDatalogFolders()` for change detection. The `totalFoldersCount` formula was `datalogFolders.size() + completedCount + pendingCount`, which double-counted these recent completed folders — once in `datalogFolders.size()` and again in `completedCount`. This made `getIncompleteFoldersCount()` return phantom incomplete folders, causing an infinite "upload will retry" loop even when all files were successfully uploaded.
- **Fix:** Filter out already-completed and pending folders from the scan result before adding to the total count. Fixed in both `uploadNewFiles()` and `scanPendingFolders()`.

### Fix 16 (HIGH) — SleepHQ imports fail: root/SETTINGS files missing from cloud imports
- **Files:** `include/FileUploader.h`, `src/FileUploader.cpp`
- **Issue:** SleepHQ requires `STR.edf`, `Identification.json`, `Identification.crc`, and `/SETTINGS/` files alongside DATALOG data to process an import. These root/SETTINGS files were only uploaded if `hasFileChanged()` detected a change. If they had been previously uploaded to SMB, their checksums were already stored and they were silently skipped — never reaching the SleepHQ import. This caused SleepHQ to mark imports as "failed".
- **Fix:** When a cloud import is active (`cloudImportCreated = true`), Phase 2 now force-includes all root and SETTINGS files regardless of checksum state. Added `forceAll` parameter to `scanRootAndSettingsFiles()` and `forceUpload` parameter to `uploadSingleFile()`. SleepHQ's server-side content_hash deduplication prevents redundant data transfer.

### Fix 17 (HIGH) — SSL memory allocation failure after many cloud uploads
- **File:** `src/SleepHQUploader.cpp`
- **Issue:** After uploading ~30+ files in a single session, the ESP32 ran out of contiguous heap for TLS handshake buffers (~40-50KB). Root cause: `HTTPClient::end()` does not close the TLS connection when the server responds with `Connection: keep-alive` (HTTP/1.1 default). TLS session buffers stayed allocated between uploads, and repeated `malloc`/`free` cycles for in-memory upload buffers (up to 49KB) fragmented the heap until no contiguous block remained for TLS.
- **Fix:** Three-layer defense:
  1. **Force connection close**: Set `http.setReuse(false)` on all `HTTPClient` instances (both `httpRequest()` and `httpMultipartUpload()`) to ensure TLS buffers are freed after each request.
  2. **Heap monitoring with cleanup**: Before every TLS operation, check `ESP.getMaxAllocHeap()`. If the largest contiguous free block is below 55KB (`MIN_HEAP_FOR_TLS`), force `tlsClient->stop()` to reclaim TLS buffers before proceeding.
  3. **Graceful fallback to streaming**: If the in-memory upload path (≤48KB files) detects insufficient heap for the buffer + TLS, it falls through to the streaming path instead of failing.

### Fix 18 (HIGH) — SleepHQ import fails when only root files change (no DATALOG changes)
- **File:** `src/FileUploader.cpp`
- **Issue:** When only root files changed between scans (e.g., `STR.edf` updated by CPAP after therapy ends) but no DATALOG folder had new/changed files, Phase 2 scanned with `forceAll=false` (since no cloud import existed yet). Only the changed file was uploaded, creating a SleepHQ import with just `STR.edf`. SleepHQ rejected this import because it requires companion files (`Identification.json`, `Identification.crc`, `SETTINGS/`) alongside any data.
- **Fix:** After the initial Phase 2 scan, if changed root files are found and a cloud endpoint is configured but no import was created in Phase 1, re-scan with `forceAll=true` to include all companion files. This ensures every SleepHQ import has the minimum required file set.

### Fix 19 (MEDIUM) — SleepHQ API conformance: path format, device/machine discovery, import naming
- **Files:** `src/SleepHQUploader.cpp`, `include/SleepHQUploader.h`, `docs/CONFIGURATION.md`
- **Issues found via live API exploration (Swagger + curl verification):**
  1. **Wrong file upload path format**: Sent `/DATALOG/20260210` but API expects `./DATALOG/20260210/` (`./ ` prefix + trailing `/`). SleepHQ normalizes paths server-side but correct format is required for clean imports.
  2. **Wrong device discovery endpoint**: Used `/api/v1/teams/{id}/devices` (doesn't exist). Correct endpoint is `/api/v1/devices/` (top-level, returns static device TYPE catalog, not per-user).
  3. **Missing machine discovery**: `/api/v1/teams/{id}/machines` returns the user's actual CPAP machines (model, serial, brand, name) — distinct from device types. Useful for import labeling and diagnostics.
  4. **No import name**: `name` parameter in import creation wasn't set.
- **Fixes:**
  1. Path format: `"." + dirPath + "/"` for all file uploads
  2. Device auto-detection from `/api/v1/devices/` — matches "ResMed" + "Series 11"
  3. New `discoverMachineInfo()` method logs all machine details
  4. Import name set to `"{machineName} Auto-Upload"` (e.g., "AirSense 11 AutoSet Auto-Upload")
- **Docs:** Complete device ID reference table added to CONFIGURATION.md (10 known device types from live API)

### Fix 23 (HIGH) — Batch SD reads and fix streaming path SD release
- **Files:** `include/SDCardManager.h`, `src/SDCardManager.cpp`, `include/SleepHQUploader.h`, `src/SleepHQUploader.cpp`, `include/FileUploader.h`, `src/FileUploader.cpp`, `include/UploadStateManager.h`, `src/UploadStateManager.cpp`
- **Issue:** Three SD card contention problems: (1) Streaming uploads (>48KB) used per-chunk SD release, but `SD_MMC.end()` in `releaseControl()` unmounts the filesystem, invalidating all open file handles — causing incomplete uploads and SSL errors. (2) `processImport()` held SD for 5+ seconds during a pure network call. (3) Each small file required a separate SD mount/unmount cycle (~600ms overhead each).
- **Root cause:** `releaseControl()` calls `SD_MMC.end()` which unmounts the filesystem. File handles become invalid after unmount, so per-chunk release in the streaming path silently broke file reads after the first release. The `processImport()` call in `endUploadSession()` didn't release SD before making the network API call.
- **Fixes:**
  1. **Fix streaming path** — Removed broken per-chunk SD release. New flow: release SD during TLS handshake (saves ~2-3s), retake SD for file streaming (unavoidable — file handle needs mounted FS), release SD after file close for response wait. Eliminates SSL errors from truncated file data.
  2. **Release SD before processImport()** — `endUploadSession()` now releases SD before the network call and retakes after. Eliminates the 5+ second hold at session end.
  3. **Batch small file uploads (cloud-only)** — New `uploadFromBuffer()` method reads multiple small files into a 48KB RAM buffer in a single SD hold, then releases SD and uploads all files from RAM. For 5 small files: ~600ms total SD overhead vs ~3000ms (5× improvement). Applied to both DATALOG folders (Phase 1) and root/SETTINGS files (Phase 2).
  4. **SD session statistics** — New `resetStatistics()`/`printStatistics()` on SDCardManager. Logs total hold time, CPAP access time, percentages, take/release counts, avg/longest/shortest hold durations at end of each upload session.
  5. **Buffer checksum computation** — New `calculateChecksumFromBuffer()` avoids re-reading files from SD for state tracking after batch uploads.
  6. **Zero-copy buffer uploads** — `uploadFromBuffer()` writes multipart parts directly to the TLS client from the batch buffer pointer. No intermediate combined buffer allocation, saving ~fileSize bytes of heap per upload. Critical on ESP32 where the 48KB batch buffer + TLS buffers leaves minimal free heap.
  7. **Deferred batch buffer allocation** — 48KB batch buffer is `malloc`'d lazily after cloud/TLS init succeeds, not before. Previously the premature allocation consumed heap needed for TLS handshake buffers, causing "heap too low" failures.
  8. **Release SD during cloud init** — `ensureCloudImport()` now releases SD before OAuth, team/device discovery, and import creation (~15s of pure network I/O). CPAP gets uninterrupted access during the entire cloud initialization phase.
  9. **Statistics fix** — `resetStatistics()` now handles the case where SD is already held when called, resetting `takeControlTime` and counting the current hold as take #1.
  10. **HTTP keep-alive for batch uploads** — `uploadFromBuffer()` now reuses the TLS connection across multiple files in a batch instead of stop/connect for each file. Eliminates heap fragmentation from repeated ~32KB TLS buffer alloc/free cycles that left contiguous heap below the 55KB TLS threshold after 4 uploads.
  11. **Release SD during NTP sync** — `FileUploader::begin()` now releases SD after state file load, before NTP sync. NTP sync is 100% network I/O (5s wait + up to 20 retries = 25s). SD hold during init drops from ~30s to ~200ms.
  12. **Phase 2 SD retake with retry** — Phase 2 (root/SETTINGS files) now ensures SD is mounted before scanning. If Phase 1 lost SD control (transient mount failure), retries with 1s delay. Previously, a transient SD failure after batch upload caused Phase 2 to scan an unmounted filesystem → 0 files found → import processed without mandatory companion files.
  13. **Companion file safety net** — `endUploadSession()` tracks whether companion files were uploaded via `companionFilesUploaded` flag. If cloud import was created but companion files are missing, attempts force-upload. If that also fails, skips `processImport()` to leave the import open rather than submitting an incomplete import that SleepHQ would reject.
  14. **Free batch buffer before SD remount** — All three DATALOG batch flush paths (mid-loop, budget-exit, end-of-folder) now `free(batchBuf)` before calling `takeControl()`. The 49KB batch buffer + 32KB TLS connection left insufficient heap for SD_MMC FAT mount buffers, causing `mount_to_vfs failed (0x101)` / `ESP_ERR_NO_MEM`. DATALOG state marking only needs `entry.localPath`/`entry.size`, not buffer data.
  15. **Free root batch buffer before SD remount** — Root batch path pre-calculates checksums from the buffer into a `std::vector<String>`, then frees the 49KB buffer before remounting SD. Root files need content checksums (unlike DATALOG's size-based tracking), so checksums are computed while buffer is still in memory.
  16. **Disconnect TLS between Phase 1 and Phase 2** — New `disconnectTls()` on SleepHQUploader frees the ~32KB TLS client between DATALOG and root/SETTINGS phases. Without this, the TLS keep-alive connection holds memory that prevents the 49KB root batch buffer from being allocated. Phase 2 uploads reconnect TLS on first use.
- **Impact:** Streaming uploads now complete successfully (no more SSL errors). Session-end SD hold drops from ~5s to ~0ms. Cloud init SD hold drops from ~15s to ~0ms. Init SD hold drops from ~30s to ~200ms. Batch uploads no longer cause heap fragmentation or SD mount failures. Root/SETTINGS files now use batch upload path (previously fell back to per-file due to heap pressure). Small file batch reduces SD mount overhead by ~5× for typical folders. Transient SD mount failures no longer cause incomplete imports. Session statistics enable ongoing SD contention diagnostics.

### Fix 22 (HIGH) — Single-read upload with size-based change detection
- **Files:** `include/UploadStateManager.h`, `src/UploadStateManager.cpp`, `include/SleepHQUploader.h`, `src/SleepHQUploader.cpp`, `src/FileUploader.cpp`
- **Issue:** Each file was read from SD card 3–4 times per upload: once for hash computation (`computeContentHash`), once for the actual upload (`httpMultipartUpload`), once for state tracking (`calculateChecksum`), and optionally once for change detection (`hasFileChanged`). This multiplied SD contention unnecessarily.
- **Root cause:** Hash computation, upload, and state tracking were separate passes over the file. The SleepHQ `content_hash` multipart field was placed before the file data, requiring the hash to be known before sending.
- **Fixes:**
  1. **Single-read upload with progressive hash** — `content_hash` moved after file data in the multipart body. Hash is computed chunk-by-chunk during the single file read. For in-memory uploads (≤48KB): file read into buffer, hash computed from buffer, then SD released for entire HTTP POST. For streaming (>48KB): hash computed progressively during file read+send. (Note: per-chunk SD release was later removed in Fix 23 — `SD_MMC.end()` invalidates file handles.)
  2. **Size-based change detection for DATALOG files** — DATALOG `.edf` files are append-only (new therapy data = file grows). `hasFileSizeChanged()` compares `file.size()` (metadata-only, no SPI data transfer) against stored size. Replaces full MD5 hash comparison that required reading the entire file.
  3. **Eliminated `computeContentHash()`** — No longer a separate function. Hash computation is merged into the upload stream.
  4. **Eliminated post-upload `calculateChecksum()` for DATALOG** — DATALOG files now store uploaded size instead of content hash. Zero SD reads for state tracking.
  5. **File size persisted in state JSON** — New `file_sizes` object in upload state file. Backward-compatible (missing = triggers re-upload).
- **Impact:** SD reads per file drop from 3–4 to exactly 1. For unchanged files in recent folder re-scans: 0 reads (was 1 full MD5 read). Streaming uploads interleave SD release per chunk (~4KB), giving CPAP access during each network send.
- **Design doc:** `docs/FIX22_SINGLE_READ_UPLOAD.md`

### Fix 21 (HIGH) — SD card contention starves CPAP of write access during therapy
- **Files:** `src/Config.cpp`, `include/SleepHQUploader.h`, `src/SleepHQUploader.cpp`, `include/FileUploader.h`, `src/FileUploader.cpp`
- **Issue:** The ESP32 held the SD card during network I/O (TLS handshake, HTTP upload, server response wait), starving the CPAP machine. With 20-minute interval uploads, therapy data for Feb 10-11 was reduced to empty 1KB header stubs — 7 fragmented sessions per night instead of normal continuous recording. BRP files were 1024 bytes vs normal 265KB-1.1MB.
- **Root cause:** SD card was held for the entire upload cycle including network-bound phases where the SPI bus wasn't needed.
- **Fixes:**
  1. **SD_RELEASE_WAIT_MS default 500→1500ms** — CPAP now gets ≥40% of SD time during periodic releases (was 20%)
  2. **Release SD during SleepHQ network I/O** — For in-memory uploads (≤48KB), SD is released after file is buffered in RAM, before HTTP POST. For streaming uploads (>48KB), SD is released after streaming completes, before waiting for server response. CPAP gets the entire network round-trip time.
  3. **Guaranteed CPAP window between every file** — New `ensureSdAndReleaseBetweenFiles()` gives CPAP a full SD_RELEASE_WAIT_MS window after each file upload, not just time-based periodic releases.
  4. **HTTP 200 vs 201 dedup logging** — SleepHQ returns 200 for files already on server (server-side dedup). Now logged as "Skipped (already on server)" to avoid redundant upload effort.
- **Impact:** For a typical 832-byte CSL.edf file: SD hold time drops from ~3 seconds (hash+upload+response) to ~200ms (hash+read only). CPAP gets ~95% of SD time during in-memory uploads.

### Fix 20 (HIGH) — SleepHQ import fails with "files missing" when budget exhausted before companion files
- **Files:** `src/FileUploader.cpp`, `src/SleepHQUploader.cpp`
- **Issue:** When the upload time budget was exhausted during Phase 1 (DATALOG folders), Phase 2 (root/SETTINGS files) was skipped entirely. The import was then processed with only DATALOG files, causing SleepHQ to reject it with "Some files were missing from your upload." SleepHQ requires companion files (`STR.edf`, `Identification.json`, `Identification.crc`, `SETTINGS/`) alongside DATALOG data.
- **Fix:** In `endUploadSession()`, if a cloud import is active but budget ran out before Phase 2, force-upload the mandatory companion files directly via `sleephqUploader->upload()` before calling `processImport()`. These files are small (~80KB total) so the overhead is minimal. Bypasses budget checks and multi-backend dispatch since these are mandatory for SleepHQ.
- **Also:** Simplified upload TLS handling — `tlsClient->stop()` before each file is sufficient (proven by import 12332755 completing successfully with 78 files). Removed unnecessary upload-time hash verification (SleepHQ echoes declared hash in 201 response regardless of data integrity).

---

## Diagnostic Logging Improvements (2026-02-11)

Enhanced logging to diagnose SD card contention between ESP32 and CPAP machine during overnight sessions.

### SD Card Contention Diagnostics
- **File:** `src/SDCardManager.cpp`
- **CS_SENSE pin state**: Logged at INFO level on every `takeControl()` call — shows whether CPAP is detected as using the SD card
- **Hold duration**: Logged in milliseconds on every `releaseControl()` — shows how long the ESP32 locked out the CPAP
- **CPAP waiting detection**: After releasing the card, checks CS_SENSE 50ms later — warns if CPAP immediately starts using the card (indicates it was waiting)

### Folder & File Detection Diagnostics
- **File:** `src/FileUploader.cpp`
- **Current date check**: Logs computed local date and explicitly checks whether today's and yesterday's DATALOG folders exist on the SD card
- **Raw /DATALOG listing**: Dumps all directory entries (no filters) with entry count
- **All files with sizes**: `scanFolderFiles()` now logs every file in each folder (not just `.edf`) with byte sizes
- **Folder scan summary**: Total dirs on card, skipped (old), skipped (completed non-recent), to process
- **Periodic SD release/retake**: Upgraded from DEBUG to INFO level for visibility
