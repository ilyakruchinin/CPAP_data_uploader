# Consolidated Issue Analysis and Recommended Fixes (v2)

This document covers four open issues, their root causes, and recommended approaches. **No code changes are included** — this is a plan-only document.

> **v2 update:** Incorporates owner decisions on approach for each issue, adds early-allocation feasibility analysis, and reflects agreed implementation plan.

---

## Feasibility Study: Early Pre-Allocation of TLS and SMB Resources

*This question was raised before diving into the individual issues: "Can TLS pre-warm AND SMB context be preloaded very early (at startup) and reused later?"*

### TLS (WiFiClientSecure + mbedTLS)

**Short answer: No — a boot-time connection cannot survive to upload time, and holding the buffers idle is wasteful.**

The expensive resources are allocated during `tlsClient->connect()`:

| Resource | Size | Lifetime |
|---|---|---|
| mbedTLS symmetric I/O buffers | 16KB IN + 16KB OUT = ~32KB | Tied to SSL session; freed on `stop()` |
| SSL context + cert chain parsing | ~4-6KB | Same |
| **Total contiguous** | **~36-38KB** | `connect()` → `stop()` |

The `WiFiClientSecure` object itself is tiny (~100 bytes). The heap cost is entirely in the active TLS session.

**Why boot-time pre-warm fails:**

1. **Connection dies before upload.** The device may sit in LISTENING for minutes to hours waiting for CPAP bus silence. SleepHQ's server idle timeout is likely 30-60s. The connection would be dead long before the upload task starts.

2. **~36KB held idle wastes scarce heap.** Keeping mbedTLS buffers allocated during the LISTENING phase (where SD scanning, web server, mDNS, and traffic monitoring all compete for heap) consumes memory that is critically needed elsewhere.

3. **Reconnection re-allocates anyway.** When the dead connection is detected, `stop()` frees all mbedTLS memory, and `connect()` must re-allocate from whatever heap is available at that point — defeating the purpose.

4. **No session resumption in Arduino framework.** `WiFiClientSecure` does not support TLS session tickets or resumption. Each `connect()` performs a full handshake with full buffer allocation.

5. **Custom mbedTLS allocator is impractical.** mbedTLS supports `mbedtls_platform_set_calloc_free()` for a custom memory pool, but the hybrid compile constraint (cannot change mbedTLS struct sizes without ABI mismatch against the pre-compiled Arduino WiFiClientSecure layer) makes this too risky.

**What DOES work: Pre-warm within the upload task, before SD mount.** This is the approach from commit `51d756d`. At upload-task start, `max_alloc` is ~98KB. Performing the TLS handshake at this point succeeds easily. SD mount and pre-flight scanning happen *after*, fragmenting heap to ~55KB — but the TLS buffers are already allocated in a contiguous block from the clean 98KB region. The connection can survive the 30-60s of pre-flight scanning if the server's keep-alive allows it.

### SMB (libsmb2 context)

**Short answer: Pre-allocating the context is trivially cheap (~2KB) but doesn't solve the real problem.**

| Resource | Size | When allocated |
|---|---|---|
| `smb2_context` struct | ~1-2KB | `smb2_init_context()` |
| TCP socket (lwIP PCB) | ~200 bytes | `smb2_connect_share()` |
| SMB negotiate/session buffers | ~1-2KB | During protocol negotiation |
| PDU queue entries | ~256 bytes each | During active operations |

The context itself is cheap. The TCP connection is the expensive/fragile part, and it has the same survival problems as TLS:

- SMB servers have idle timeouts (Windows default: 15 minutes)
- The server may not be reachable at boot time (sleeping NAS, network not ready)
- Holding a socket FD open during LISTENING wastes lwIP resources

Pre-allocating the `smb2_context` at boot saves ~2KB of fragmentation but doesn't address the 38900-byte contiguous-heap floor, which is dominated by lwIP + SD_MMC + concurrent I/O buffers.

### Conclusion

| Strategy | Feasible? | Benefit | Risk |
|---|---|---|---|
| TLS connect at boot | No | N/A — connection dies | Wastes 36KB for hours |
| TLS pre-warm in upload task (before SD mount) | **Yes** | Handshake at ~98KB ma | Connection may die during scan |
| SMB context at boot | Technically yes | Saves ~2KB fragmentation | Not worth the complexity |
| Custom mbedTLS memory pool | No | Would guarantee allocations | ABI mismatch risk with hybrid compile |

**Recommended: TLS pre-warm within the upload task (before SD mount), only when `config.hasCloudEndpoint()`.** No boot-time pre-allocation.

---

## Issue 1: Log Display Order (ref: 39-LOG-RESEARCH-K25.md)

### Problem

The Web GUI Logs tab shows out-of-order timestamps where lines from a previous boot (e.g. `[21:10:38]`) appear interleaved with lines from the current boot (e.g. `[12:16:27]`). Downloaded logs are correct — the bug is purely in the client-side display.

### Root Cause

In `web_ui.h`, the `_appendLogs()` function's "genuinely new reboot" branch (lines 800–806) searches backwards from the boot banner for the `=== BOOT` separator and includes up to **10 lines before it** as "pre-reboot context":

```javascript
ctxStart = Math.max(0, j - 10);  // includes old-boot timestamps
newLines = lines.slice(ctxStart);
```

Those 10 context lines carry timestamps from the previous boot's timeline, which may be hours or days earlier. When appended to `clientLogBuf`, they appear to go backwards in time.

### Why the Downloaded File Is Correct

The server streams logs in correct chronological order: `syslog.3.txt` → `syslog.0.txt` → RAM buffer. Each file is internally chronological, and boot separators (`=== BOOT ...`) delimit boundaries. The download endpoint (`/api/logs/saved`) delivers this raw stream. The bug is only in `_appendLogs()`, which re-interprets the stream client-side.

### Chosen Fix: Option B — Clear and Re-fetch (Cleanest Long-Term)

On reboot detection, clear `clientLogBuf` entirely and re-fetch `/api/logs/full`. The server already provides correctly-ordered multi-boot history. The client just displays it as-is, eliminating the cross-boot stitching problem entirely.

This pattern is already implemented for the polling path (lines 855–861 in `fetchLogs()`):

```javascript
if (newBootDetected && backfillDone) {
    clientLogBuf = []; lastSeenLine = '';
    fetchBackfill();
}
```

The fix: make the "genuinely new reboot" branch in `_appendLogs()` simply set `newBootDetected = true` and return early, letting the existing `fetchLogs()` cleanup logic handle the rest. This avoids duplicating cross-boot stitching logic entirely.

#### Heap and Power Considerations

- **ESP32 heap:** Zero impact. The `/api/logs/full` handler streams directly from LittleFS files + RAM buffer to the HTTP response. No large intermediate buffers on the ESP32 side. All log buffering is in **browser memory** (`clientLogBuf` JS array).
- **Power draw:** One additional HTTP request (~50-100KB streamed) only on reboot detection, which is infrequent (once per boot cycle). Negligible compared to the file upload operations that follow. The streaming response uses chunked transfer, so no large response buffering on the ESP32.
- **Browser memory:** `clientLogBuf` is capped at `LOG_BUF_MAX = 2000` lines, which is already the existing limit. A full re-fetch replaces the buffer contents rather than growing it.

#### Why Not Option A?

Option A (just slice from `bootIdx`) would be fewer lines changed, but it leaves the stitching logic intact and creates a maintenance burden: any future changes to boot separators, log format, or persistence behavior would need to be reflected in the client-side stitching code. Option B delegates all ordering responsibility to the server, which already handles it correctly.

### Effort Estimate

~5 lines changed in `web_ui.h`. Low risk, no firmware-side changes needed.

---

## Issue 2: WiFi Password Lost After Full Flash (ref: 40-WIFI-PASSWORD-K25.md)

### Problem

After a full firmware flash (which erases NVS), the device fails to connect to WiFi. The `config.txt` on the SD card still contains `***STORED_IN_FLASH***` as the password, but the actual password is gone from NVS. `loadCredential()` silently returns an empty string, and the device attempts an open-network connection.

This is a recurring support issue because:

- development is rapid
- partition table changes require full flash
- users don't understand they need to re-enter passwords in `config.txt`

### Current Architecture

- Default mode: `storePlainText = false` (secure mode)
- On first boot with plaintext credentials in `config.txt`, the system migrates them to NVS and replaces them with `***STORED_IN_FLASH***`
- On subsequent boots, the censored placeholder triggers NVS lookup
- If NVS is erased (full flash, partition change), the lookup silently fails

### Chosen Approach: Plaintext Default + Rename Config Key

#### Decision

1. **Default to plaintext credentials** (no NVS migration, no censoring)
2. **Rename config key** from `STORE_CREDENTIALS_PLAIN_TEXT` to `MASK_CREDENTIALS`
3. **`MASK_CREDENTIALS` defaults to `false`** (no masking = plaintext = safe default)
4. **No backwards compatibility** with the old `STORE_CREDENTIALS_PLAIN_TEXT` key
5. **No automatic NVS-to-plaintext de-export** — users with existing `***STORED_IN_FLASH***` in config.txt will be advised to re-enter their passwords manually

#### Why This Is the Right Call

1. **The threat model doesn't justify the complexity.** The SD card is physically inside the CPAP device. Anyone with physical access to the SD card already has access to the device itself.

2. **NVS is fragile across development cycles.** Partition table changes, full flashes, and NVS corruption all silently break WiFi.

3. **User experience is the priority.** Users should be able to edit `config.txt`, flash firmware, and have things work.

4. **Simpler semantics.** `MASK_CREDENTIALS = true` is a clear opt-in: "I want my passwords hidden from `config.txt`." Users who enable this understand the trade-off.

#### Implementation Plan

1. **Rename internal field:** `storePlainText` → `maskCredentials` (or equivalent), default `false`

2. **Rename config key:** `STORE_CREDENTIALS_PLAIN_TEXT` → `MASK_CREDENTIALS`
   - `MASK_CREDENTIALS = true` → migrate plaintext passwords to NVS, replace with `***STORED_IN_FLASH***` (existing behavior)
   - `MASK_CREDENTIALS = false` (default) → passwords stay in `config.txt` as plaintext

3. **Remove the old `STORE_CREDENTIALS_PLAIN_TEXT` parser branch.** No backwards compatibility mapping.

4. **When `MASK_CREDENTIALS = false` and config.txt contains `***STORED_IN_FLASH***`:**
   - Log a clear `LOG_ERROR`: `"WiFi password is '***STORED_IN_FLASH***' but MASK_CREDENTIALS is off — NVS data may be lost after full flash. Please re-enter your password in config.txt."`
   - Do NOT attempt to load from NVS (no de-export)
   - This makes the failure mode loud and actionable

5. **Documentation update:** Add a note to README / config reference:
   > `MASK_CREDENTIALS = true` hides sensitive credentials from `config.txt` by storing them in ESP32 flash memory (NVS). **Warning:** A full (non-OTA) firmware flash erases NVS. After such a flash, you must re-enter all passwords in `config.txt`. OTA updates are not affected.

#### What This Fixes

- Full flash → config.txt still has plaintext password → works immediately
- Partition table change → same
- New users → passwords stay in config.txt → no confusion
- Clear error message if `***STORED_IN_FLASH***` found without NVS backing

#### What This Doesn't Fix (Expected)

Users who previously had `STORE_CREDENTIALS_PLAIN_TEXT = false` (now removed) AND whose config.txt contains `***STORED_IN_FLASH***` will need to re-enter their passwords once. The loud error log will guide them.

### Effort Estimate

~25 lines changed across `Config.cpp` and `Config.h`. Low-medium risk.

---

## Issue 3: SMB Stall / Watchdog Resets (ref: 41-STALL-C54.md)

### Updated Understanding

My initial analysis in doc 41 recommended adding `smb2_set_timeout()`. After deeper investigation, I found that **it's already implemented**:

```cpp
#define SMB_COMMAND_TIMEOUT_SECONDS 15
smb2_set_timeout(smb2, SMB_COMMAND_TIMEOUT_SECONDS);
```

And the `libsmb2` sync API uses non-blocking sockets (`O_NONBLOCK`) with a `poll(..., 1000)` loop in `wait_for_reply()` that checks `smb2_timeout_pdus()` every second. So the protocol-level timeout mechanism is in place and should work.

### Why Watchdog Resets Still Occurred

The watchdog resets in `watchdog.txt.tmp` happened when the **SMB server was genuinely unavailable**. In that scenario:

1. The device boots, waits 62s for bus silence, starts upload
2. SMB `connect()` or early operations fail/timeout after 15s
3. Retry logic kicks in, reconnect attempts, etc.
4. The cumulative time of connect attempts + timeouts + retries exceeds the 30s task-WDT window

The task-WDT is 30s. A single SMB operation times out at 15s. But `smb2_connect_share()` involves DNS resolution + TCP connect + SMB negotiate + session setup — each with its own potential 15s timeout window. Two back-to-back timeouts (connect + retry) = 30s, which is right at the WDT edge.

Additionally, there are paths in `SMBUploader.cpp` that include `delay(150)` for reconnect backoff and `delay(200 * reconnectAttempt)` for progressive backoff, adding to the total elapsed time without feeding the watchdog.

### User's Concern 1: What if a large file legitimately takes >30s?

This is a **valid concern**. Looking at the logs:

```
[17:09:49] Uploading file: 20260314_224926_BRP.edf (1495522 bytes)
[17:10:17] Uploaded: 20260314_224926_BRP.edf (1495522 bytes)
```

That's 28 seconds for a 1.5MB file. A slightly larger file or slower network could exceed 30s.

However, the upload loop in `SMBUploader::upload()` feeds the watchdog between chunks:

```cpp
feedUploadHeartbeat();  // resets both hw WDT and sw heartbeat
taskYIELD();
yield();
```

So a long-but-progressing transfer won't trigger the WDT. The WDT only fires when the code is stuck inside a single blocking call for >30s without returning.

The real risk is: what if a single `smb2_write()` call blocks for >30s? With non-blocking sockets and `poll()` with 1s timeout, this shouldn't happen in normal operation. But on ESP32's lwIP, `poll()` behavior under memory pressure or WiFi instability may not be as reliable as on a full Linux system.

**Verdict:** The current 30s task-WDT timeout is tight but adequate for normal operation. The problem is in error/retry paths that accumulate delays without feeding the WDT.

### User's Concern 2: Responsiveness in Blocking Mode

The web server runs on **Core 1**, the upload task on **Core 0**. So the web server's `handleClient()` is not directly blocked by SMB operations on the other core.

However, from previous investigation: "when upload hangs in SMB operation, test web server also becomes unresponsive until watchdog reboot." This suggests lwIP resource contention — both cores share the same TCP/IP stack and socket pool. A wedged SMB socket can exhaust file descriptors or lwIP PCBs, starving the web server.

Going async resolves this because the upload task yields to `poll()` between operations, giving lwIP more processing time on both cores.

### User's Concern 3: Are Cloud Uploads Also Blocking?

**Yes, but with better protection.** The `SleepHQUploader` uses `WiFiClientSecure` which:

- Sets `SO_SNDTIMEO` / `SO_RCVTIMEO` via `tlsClient->setTimeout(60)` — **60-second socket-level timeout**
- Has explicit application-level timeouts for response waiting (30s), header parsing (5s), body draining (5s)
- Feeds watchdog between chunks
- Has retry + reconnect logic with WiFi recovery

The key difference: `WiFiClientSecure` sets socket-level timeouts, while `libsmb2` relies only on its own `poll()` + protocol-timeout mechanism. The cloud path is more robust against TCP-level hangs.

However, the 60s `setTimeout` is concerning — it's **double** the 30s task-WDT. If a single `tlsClient->write()` or `tlsClient->connect()` blocks for the full 60s timeout, the task-WDT will fire first. This should be reduced to 15-20s regardless of whether SMB goes async.

### Chosen Approach: Async SMB (Direct to Phase 2)

#### Decision

Go directly to async SMB using `libsmb2`'s async API, combined with the immediate hardening fixes. The async approach is **heap-neutral** (see analysis below) and addresses all three concerns simultaneously.

#### Heap Impact Analysis: Sync vs Async

The `libsmb2` async API uses the **exact same data structures** as the sync API:

- Same `smb2_context` (~1-2KB)
- Same PDU structures (~256 bytes each)
- Same `smb2_write_async()` → same network buffers
- Same non-blocking socket with `O_NONBLOCK`

The difference is purely in control flow:

| Aspect | Sync (`smb2_write`) | Async (`smb2_write_async` + event loop) |
|---|---|---|
| Context memory | Same | Same |
| PDU buffers | Same | Same |
| Socket | Same (non-blocking) | Same (non-blocking) |
| Stack usage | Slightly higher (calloc for `sync_cb_data` in `wait_for_reply`) | Slightly lower (no `sync_cb_data` wrapper) |
| Control flow | `poll()` loop inside `wait_for_reply()` | `poll()` loop in caller |
| Additional heap | None | None |

**Conclusion: Async SMB is heap-neutral.** It may actually use marginally *less* heap because it eliminates the per-call `calloc(sizeof(sync_cb_data))` that the sync wrapper allocates and frees in `wait_for_reply()`.

#### Architecture

Replace all sync `smb2_*()` calls with their `_async` counterparts and a shared event-loop helper:

```c
// Shared event loop — replaces libsmb2's internal wait_for_reply()
int smb2_run_until_complete(struct smb2_context *smb2, bool *done_flag) {
    while (!*done_flag) {
        struct pollfd pfd = { smb2_get_fd(smb2), smb2_which_events(smb2), 0 };
        poll(&pfd, 1, 1000);  // 1s max wait — never blocks longer

        if (pfd.revents) {
            if (smb2_service(smb2, pfd.revents) < 0) return -1;
        }

        // Feed both watchdogs every iteration
        feedUploadHeartbeat();
        esp_task_wdt_reset();

        // Check abort/yield flags
        if (g_abortUploadFlag) return -ECANCELED;
        if (checkYieldNeeded()) return SMB_YIELD_NEEDED;

        // Timeout check (libsmb2 handles this internally via smb2_timeout_pdus,
        // but we add an outer safety net)
        if (smb2->timeout) smb2_timeout_pdus(smb2);
    }
    return 0;
}
```

Each SMB operation becomes:

```cpp
// Before (sync):
int rc = smb2_write(smb2, fh, buf, count);

// After (async):
bool write_done = false;
int write_status = 0;
smb2_write_async(smb2, fh, buf, count, write_cb, &cb_data);
int rc = smb2_run_until_complete(smb2, &write_done);
```

#### What This Solves

1. **Watchdog resets:** The event loop feeds WDT every 1s iteration. No single call can block >1s.
2. **Web responsiveness:** `poll(1000)` yields CPU between iterations, letting lwIP process web server traffic on the shared TCP/IP stack.
3. **CPAP yield support:** Abort/yield flags are checked every iteration, enabling graceful interruption.
4. **Cancel from Web UI:** `g_abortUploadFlag` is checked every iteration.

#### What Also Needs Hardening (Regardless of Async)

1. **Reduce TLS socket timeout** from 60s to 20s (affects cloud uploads, independent of SMB)
2. **Cap total SMB session retry time** — abort cleanly after N consecutive connection failures
3. **Feed WDT in reconnect backoff delays** in `SMBUploader.cpp`

#### Operations to Convert

| Operation | Sync | Async |
|---|---|---|
| Connect | `smb2_connect_share()` | `smb2_connect_share_async()` |
| Disconnect | `smb2_disconnect_share()` | `smb2_disconnect_share_async()` |
| Open file | `smb2_open()` | `smb2_open_async()` |
| Write | `smb2_write()` | `smb2_write_async()` |
| Close | `smb2_close()` | `smb2_close_async()` |
| Mkdir | `smb2_mkdir()` | `smb2_mkdir_async()` |
| Stat | `smb2_stat()` | `smb2_stat_async()` |
| Opendir/Readdir | `smb2_opendir()` / `smb2_readdir()` | Async equivalents |

### Effort Estimate

~200-300 lines of refactoring in `SMBUploader.cpp`. Medium-high risk — needs thorough testing of error paths and reconnect logic. The `smb2_run_until_complete()` helper centralizes the event loop, so each operation conversion is mechanical.

---

## Issue 4: Heap Fragmentation / max_alloc Drops to 38900 Bytes

### What the Logs Show

The `max_alloc` (largest contiguous free block) periodically drops to exactly **38900 bytes** during SMB file transfers, then recovers:

```
Uploading: 20260225_022314_PLD.edf (122056 bytes)  ma=42996
Uploaded:  20260225_022314_PLD.edf                  ma=38900  ← drop
Uploading: 20260225_022314_SA2.edf (54264 bytes)    ma=38900
Uploaded:  20260225_022314_SA2.edf                   ma=47092  ← recovery
```

The 38900 figure is suspiciously consistent across multiple log captures, suggesting it's a steady-state fragmentation floor rather than a leak.

### Why 38900 Bytes Appears Repeatedly

The ESP32 heap is managed by a multi-region allocator. The 38900-byte figure likely represents the largest contiguous block available after:

- lwIP TX/RX buffers are allocated (PBUF pool, TCP segments)
- SMB context + PDU buffers are active
- SD_MMC DMA buffers are allocated
- Web server + SSE client sockets are open
- Logger circular buffer (8KB BSS, not heap — not a factor)

The drop happens during active I/O (file read + network write simultaneously), and recovers when the network buffers are freed after the transfer completes.

### Is 38900 Bytes Safe?

#### For SMB Operations
Yes. `libsmb2` PDU allocations are small (~256 bytes per PDU). The SMB upload buffer is 4096 or 2048 bytes (adaptive). No single SMB operation needs anywhere near 38KB contiguous.

#### For TLS Handshake
**Marginal.** `WiFiClientSecure` allocates mbedTLS buffers during handshake:

- Default symmetric buffers: 16KB IN + 16KB OUT = ~32KB
- mbedTLS overhead (SSL context, certificates): ~4-6KB additional
- Total contiguous requirement: **~36-38KB**

The code already has a guard in `SleepHQUploader.cpp`:

```cpp
if (maxAlloc < 36000) {
    LOG_ERRORF("[SleepHQ] Insufficient contiguous heap for SSL (%u bytes), skipping request");
    return false;
}
```

At 38900 bytes, TLS handshake should succeed but with only **~2.9KB margin**. Any additional fragmentation from a concurrent allocation (web request, log flush, WiFi event) during the handshake window could push it below 36KB and fail.

#### For SMB + TLS Coexistence
Each session runs only one backend at a time (SMB OR Cloud, never both). TLS resources are released via `resetConnection()` before SMB starts, and vice versa. So the 38900 drop during SMB is not directly dangerous to TLS — by the time TLS is needed, SMB resources are freed and `max_alloc` should recover.

The risk is in **MINIMIZE_REBOOTS mode**, where the device skips the post-upload soft reboot and enters the next upload cycle with accumulated fragmentation. Over multiple cycles without reboot, the baseline `max_alloc` could drift lower.

### Recent Commits Tried to Address This

| Commit | Change | Effect |
|---|---|---|
| `15bc2da` | Reduce `LWIP_PBUF_POOL_SIZE` | Fewer pre-allocated network buffers → more free heap |
| `d8dbf82` | Revert WiFi sleep IRAM optimizations | Free ~2-4KB DRAM |
| `721fc33` | Memory heap improvements | Various |
| `51d756d` | Restore TLS pre-warm before SD mount | Handshake while heap is still unfragmented (~98KB ma) |
| `d4cc025` | Fix WiFi config key, remove TLS pre-warm | Reversed the pre-warm |
| `073a41a` | Fix brownout config key | Config fix |

### Chosen Approach

#### Tier 1: Accept 38900 as the Operational Floor (Agreed)

The data shows 38900 bytes is the natural steady-state during SMB transfers. It recovers after transfers complete. Since SMB and TLS don't run simultaneously, this floor is **not inherently dangerous**.

**Action:** Document 38900 as the expected operational floor during SMB transfers. Ensure all allocation guards use thresholds below this (the current 36000 guard is appropriate). Add a comment in the codebase near the 36000 threshold explaining this documented floor.

#### Tier 2: TLS Pre-Warm Within Upload Task (Contingent on Feasibility — See Above)

Per the feasibility study above, boot-time pre-allocation is **not viable**. The recommended approach is:

**Pre-warm TLS within the upload task, before SD mount, only when `config.hasCloudEndpoint()`:**

1. Upload task starts → `max_alloc` is ~98KB
2. Call `preWarmTLS()` → TLS handshake succeeds easily at 98KB
3. Mount SD → `max_alloc` drops to ~55KB (but TLS buffers already allocated)
4. Pre-flight scan → further fragmentation to ~38KB (TLS buffers safe)
5. Cloud uploads use the existing TLS connection

This was commit `51d756d` and was reverted in `d4cc025`. The revert commit message says "Remove TLS pre-warm before SD mount" alongside a WiFi config key fix — the pre-warm removal may have been incidental to the config fix rather than a deliberate revert. Worth re-evaluating.

**Caveats:**
- Only pre-warm when cloud endpoint is configured (`config.hasCloudEndpoint()`)
- The connection may die during pre-flight scan (30-60s). If so, reconnect will happen at the lower `max_alloc` — but the existing 36000 guard handles this gracefully
- Pre-warming adds ~5-15s to upload task startup (TLS handshake time)
- Wastes power if no cloud work exists (but the config check minimizes this)

#### Tier 3: Add a Heap Safety Valve (Agreed)

In `MINIMIZE_REBOOTS` mode, the current check is:

```cpp
if (ma < 35000) {
    LOG_WARN("Heap fragmented — contiguous block below 35KB. Consider rebooting...");
}
```

This only warns. It should **force a reboot** if `max_alloc` drops below a critical threshold:

```cpp
if (ma < 32000) {
    LOG_WARN("Heap critically fragmented — forcing reboot to restore heap");
    setRebootReason("Heap safety valve (ma < 32KB)");
    Logger::getInstance().flushBeforeReboot();
    delay(200);
    esp_restart();
}
```

This ensures that even in `MINIMIZE_REBOOTS` mode, the device self-recovers before reaching a point where TLS or SMB operations would fail.

Additionally, consider adding a periodic `max_alloc` check in the main `loop()` (not just at state transitions) to catch gradual fragmentation drift.

#### Tier 4: Reduce Fragmentation Sources (Agreed — Where Applicable)

Several things contribute to heap fragmentation during upload:

1. **SMB reconnects** — each `smb2_init_context()` / `smb2_destroy_context()` cycle allocates and frees memory in different patterns
   - **Fix:** With async SMB (Issue 3), the context can be kept alive across files more reliably, reducing alloc/free churn
2. **UploadStateManager JSON serialization** — `DynamicJsonDocument` allocations during save/load
   - **Fix:** Use `StaticJsonDocument` with stack-allocated buffer where document size is bounded
3. **String operations** — Arduino `String` class uses heap realloc, which fragments
   - **Fix:** Continue replacing with stack buffers in hot paths (already partially done in SleepHQUploader)

### Is There Currently a Mechanism to Reboot on Critical Heap?

**Partially.** The default post-upload behavior is to soft-reboot (which restores full heap). In `MINIMIZE_REBOOTS` mode, there's a warning at 35KB but no forced reboot. There is no runtime heap monitor that checks `max_alloc` periodically during upload — the checks only happen at specific code points.

---

## Summary: Agreed Implementation Plan

| Priority | Issue | Fix | Effort | Risk |
|---|---|---|---|---|
| **1** | Log display order | Option B: clear buffer + re-fetch on reboot detect | ~5 lines | Low |
| **2** | WiFi password loss | Plaintext default + `MASK_CREDENTIALS` rename | ~25 lines | Low-Med |
| **3** | SMB stall + responsiveness | Async SMB rewrite + WDT hardening + reduce TLS timeout | ~250 lines | Med-High |
| **4a** | Heap: operational floor | Document 38900 as expected floor | ~5 lines (comments) | None |
| **4b** | Heap: safety valve | Force reboot below 32KB `max_alloc` in MINIMIZE_REBOOTS | ~10 lines | Low |
| **4c** | Heap: TLS pre-warm | Pre-warm in upload task before SD mount (cloud-only) | ~20 lines | Medium |
| **4d** | Heap: fragmentation | Reduce alloc/free churn (SMB context reuse, StaticJson, stack buffers) | Ongoing | Low |

**Execution order:** 1 → 2 → 4a+4b → 3 (async SMB, includes TLS timeout fix) → 4c → 4d (opportunistic, alongside other changes).

Fixes 1, 2, 4a, and 4b can be done in a single commit. Fix 3 (async SMB) is the largest change and should be a dedicated branch with thorough testing. Fix 4c depends on re-evaluating why the pre-warm was reverted.
