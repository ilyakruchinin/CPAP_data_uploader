# CPAP AutoSync v2.0i-alpha5

**Incremental patch release on top of v2.0i-alpha4.**
OTA update from any v2.0i-alpha is supported — no USB flash required.

---

## WDT Hardening: Per-Write Feeds & Reduced Socket Timeout

**Problem:** Despite alpha4's WDT hardening around TLS handshakes, a watchdog crash was observed during cloud upload of a small (896-byte) file. Root cause: consecutive `tlsClient->write()` calls in HTTP header/preamble/footer sections had no WDT feeds between them. With `SO_SNDTIMEO=20s`, if even 2 consecutive writes blocked on a saturated TCP send buffer (e.g. from concurrent web server log download), total blocking exceeded the 30s WDT.

**Fixes:**
- **SO_SNDTIMEO reduced 20s → 10s** — caps any single blocking `write()` so 2 consecutive blocked writes (20s) stays under 30s WDT
- **Per-write WDT feeds in `httpRequest()`** — 4 strategic `esp_task_wdt_reset()` calls interspersed among the 7+ header writes + flush (was: 0 feeds before flush)
- **Per-write WDT feeds in `httpMultipartUpload()` headers** — 3 feeds among 7 header writes (was: 1 feed after all 7)
- **Per-write WDT feeds in multipart preamble** — 2 feeds between 3 writes (was: 1 after all 3)
- **Per-write WDT feeds in footer + pre-flush** — feed between footer writes and before `flush()` (was: 1 feed before footer block)
- **WDT feed in WiFi reconnection wait loop** — response-header-timeout path now feeds both heartbeat and WDT

**Result:** No sequence of consecutive `tlsClient->write()` or `flush()` calls can exceed ~10s without a WDT feed. Worst-case gap between feeds is ~10s, well under the 30s WDT.

**Files:** `src/SleepHQUploader.cpp`

---

## Upload-Safe Log Loading (Tiered Protection)

**Problem:** Opening the Logs tab during an active upload triggered a `/api/logs/full` request streaming up to ~140 KB of NAND rotation files. This saturated lwIP's shared TCP send buffer, starving the upload task's TLS writes and causing watchdog timeouts — even with a single browser tab.

**Solution:** Five-tier defense system:

### Tier 1 — Server-side guard on `/api/logs/full`
During active upload, `handleApiLogsFull()` serves **only the circular buffer** (~12–16 KB) with zero NAND reads. Eliminates the primary source of TCP buffer contention.

### Tier 2 — Client-side upload awareness
When FSM state is UPLOADING/ACQUIRING, `_tryBackfill()` skips `/api/logs/full` entirely. The Logs tab falls back to `/api/logs` (circular buffer) + SSE for live streaming. Status shows "Upload active — showing live logs only".

### Tier 3 — Multi-tab blocks backfill
When multi-tab contention is detected (`_mtThrottled`), `_tryBackfill()` refuses to run. Status shows "Close other tabs to load full log history". Prevents any heavy log fetch while multiple clients compete for resources.

### Tier 4 — Reduced NAND log payload
`/api/logs/full` now serves **only `syslog.0.txt`** (latest rotation, ≤32 KB) + circular buffer (~48 KB total) instead of all 4 rotation files (~140 KB). Full history remains available via "Download All Logs" button (`/api/logs/saved`). `Logger::streamSavedLogs()` gains a `maxFiles` parameter.

### Tier 5 — Faster multi-tab decay
Multi-tab contention clears after **3 consecutive clean polls** (~15–30s) instead of 6 (~30–90s). Reduces time users must wait after closing duplicate tabs.

**Auto-resume:** Deferred backfill triggers automatically when upload finishes (state transition in `renderStatus()`) or multi-tab contention clears (`_mtSetDup(false)`).

**Files:** `src/CpapWebServer.cpp`, `include/web_ui.h`, `include/Logger.h`, `src/Logger.cpp`

---

## Files Changed

- `src/SleepHQUploader.cpp` — SO_SNDTIMEO 20s→10s, per-write WDT feeds in all TLS write sections
- `src/CpapWebServer.cpp` — `/api/logs/full` upload guard (circular buffer only during upload), reduced to latest rotation file only
- `include/web_ui.h` — `_tryBackfill()` gating (upload-aware + multi-tab aware), auto-resume on state change, faster multi-tab decay (6→3 polls)
- `include/Logger.h` — `streamSavedLogs(Print&, int maxFiles)` parameter added
- `src/Logger.cpp` — `streamSavedLogs()` respects `maxFiles` parameter for partial rotation file serving
