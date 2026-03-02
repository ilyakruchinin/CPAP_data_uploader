# SMB+CLOUD Dual-Backend Redesign

> **Status**: Proposal — no code changes yet  
> **Date**: 2026-03-02  
> **Context**: SMB-only and CLOUD-only uploads work perfectly. Mixed SMB+CLOUD mode
> fails due to heap fragmentation, broken backend cycling, and TLS/SMB socket conflicts.

---

## 1. Problem Statement

Ten log files from mixed SMB+CLOUD testing reveal **five independent failure modes**
that combine to make dual-backend uploads unreliable:

| # | Problem | Root Cause | Impact |
|---|---------|-----------|--------|
| 1 | **Backend cycling broken** | `selectActiveBackend()` called once in `begin()` during `setup()`. With `MINIMIZE_REBOOTS`, the backend never changes. SMB always wins (`smbTs <= cloudTs`). | CLOUD never gets a turn until SMB finishes ALL its work (many sessions later). |
| 2 | **TLS handshake fails at ma=38900** | Pre-compiled Arduino-ESP32 ignores `sdkconfig.defaults` for asymmetric mbedTLS buffers. Actual TLS buffers are symmetric 16KB IN + 16KB OUT = 32KB. Handshake needs ~50KB+ peak contiguous. After SD mount + task start, ma=38900. | Cloud uploads fail with "TLS connect failed" every time the pre-warmed connection dies. |
| 3 | **TLS socket poisons libsmb2** | Active `WiFiClientSecure` TCP socket causes `errno:9 (EBADF)` on every `smb2_connect_share()` through lwIP socket layer conflict. | SMB connections fail for 15+ seconds while TLS socket is alive. |
| 4 | **Task creation fails at ma=38900** | 16KB task stack + FreeRTOS TCB need contiguous allocation. At ma=38900 (after TLS pre-warm + SD mount), allocation occasionally fails. | Upload session skipped entirely: "Failed to create upload task (rc=-1)". |
| 5 | **Heap leak over sessions** | `fh` drops from ~176KB to ~100KB over multiple sessions within same boot. Small leaks from String objects, SMB context residue, and lwIP socket buffers accumulate. | Eventually triggers task creation failure or TLS connect failure. |

### Why SMB-only works (ma=90,100)

No TLS socket, no TLS buffers consuming heap. OTA manager doesn't allocate
`WiFiClientSecure`. SMB gets 90KB contiguous — plenty for libsmb2.

### Why CLOUD-only works (ma=38,900)

Pre-warm TLS at ma=73,716 before SD mount. Connection stays alive through the
session. No SMB socket conflict. `httpRequest()` reuses the live connection —
never needs a fresh handshake at low ma.

---

## 2. Current Heap Budget

```
Boot:           242,948 total / 110,580 max_alloc
                    ↓
WiFi + BT off:  ~210,000 / 110,580
                    ↓
Init (web/OTA): ~167,000 / 73,716  (cloud configured)
                ~176,000 / 90,100  (SMB-only)
                    ↓
TLS pre-warm:   ~115,000 / 57,332  (−44KB fh, −16KB ma)
                    ↓
SD mount:        ~87,000 / 38,900  (−28KB fh, DMA buffers)
                    ↓
Task (16KB):     ~71,000 / 38,900  (task stack from different heap region)
```

### Permanent residents (always allocated):

| Component | Heap (approx.) | Notes |
|-----------|---------------|-------|
| WiFi driver | ~40KB | Static RX/TX buffers, crypto |
| lwIP pbuf pool | ~51KB | 32 × 1.6KB (sdkconfig override) |
| Web server | ~8KB | Handlers, SSE client, response buffers |
| FileUploader objects | ~4KB | 2× UploadStateManager, ScheduleManager |
| SMB buffer | 2-8KB | `malloc()`'d, persists across sessions |
| SleepHQUploader | ~2KB | Config refs, String members |
| OTA manager | ~2KB | Handler + version string |
| Logger | 4KB | BSS (not heap) |

### Transient (during upload session):

| Component | Heap (approx.) | Lifetime |
|-----------|---------------|----------|
| SD_MMC DMA | ~24KB | While SD mounted |
| TLS connection | ~40-45KB | While WiFiClientSecure connected |
| Upload task stack | 16KB | While task running |
| libsmb2 context | ~8-12KB | While SMB connected |

### The critical constraint

TLS handshake needs ~50KB+ contiguous at peak. After SD mount, ma ≈ 38,900 with
TLS already connected, or ma ≈ 65,524 without TLS. **A fresh TLS handshake cannot
succeed after SD mount** unless the task stack hasn't been allocated yet (ma ≈ 65,524)
or TLS was pre-warmed before SD mount.

---

## 3. Proposed Architecture: Phased Single-Session Upload

### Core Idea

Instead of cycling backends across sessions (which requires reboots or complex state
management), run **both backends sequentially within a single upload session**:

```
Phase 0: PREPARE     — create task at high ma, pre-warm TLS, mount SD
Phase 1: CLOUD       — upload using pre-warmed TLS (connection alive)
Phase 2: TRANSITION  — tear down TLS, verify heap recovery
Phase 3: SMB         — upload with clean socket table, more heap
Phase 4: CLEANUP     — unmount SD, report results
```

### Why CLOUD first, SMB second

1. **TLS handshake needs high ma** — must happen before SD mount fragments heap
2. **Pre-warmed TLS dies after ~15s idle** — must be used immediately after pre-warm
3. **TLS teardown recovers heap** — ma goes from 38,900 → 57,332 after teardown
4. **SMB benefits from freed TLS heap** — libsmb2 gets 57KB contiguous vs 49KB today

### Key insight: move SD mount into the upload task

Currently:
```
Core 1 (handleAcquiring):  preWarmTLS() → sdManager.takeControl()
Core 0 (uploadTask):       uploadWithExclusiveAccess()
```

The task stack (16KB) is allocated at ma=38,900 (after TLS + SD), which is marginal
and occasionally fails.

Proposed:
```
Core 1 (handleAcquiring):  [nothing heavy — just transition to UPLOADING]
Core 0 (uploadTask):       preWarmTLS() → sdManager.takeControl() → upload → release
```

Benefits:
- **Task created at ma=73,716** (before any transient allocations) — always succeeds
- **TLS pre-warm at ma=57,716** (after 16KB task stack) — reliable
- **SD mount happens last** (ma drops but TLS is already connected)
- **Single core owns the full sequence** — no cross-core timing issues

### Detailed flow

```
handleAcquiring():
    transitionTo(UPLOADING)    // no SD mount, no TLS — just start

handleUploading():
    if !uploadTaskRunning:
        // Task created at ma≈73,716 — very reliable
        createUploadTask(uploadOrchestrator, 12288)  // 12KB stack
        
uploadOrchestrator():
    // ── Phase 0: PREPARE ──────────────────────────────────
    if hasCloud:
        preWarmTLS()                    // ma≈57,716 → TLS connected
    
    sdManager.takeControl()             // mount SD, ma drops
    loadState()                         // read LittleFS summaries
    preflight()                         // scan for work
    
    // ── Phase 1: CLOUD ────────────────────────────────────
    if hasCloud && cloudHasWork:
        cloudBudget = hasSMB ? maxMs/2 : maxMs
        runCloudPhase(cloudBudget)      // pre-warmed TLS alive
        sleephqUploader->resetConnection()  // free ~40KB
        delay(100)                      // lwIP cleanup
        LOG("Phase 1 complete: ma=%u", ESP.getMaxAllocHeap())
    
    // ── Phase 2: SMB ──────────────────────────────────────
    if hasSMB && smbHasWork:
        smbBudget = hasCloud ? maxMs/2 : maxMs
        runSmbPhase(smbBudget)          // ma≈57,332, clean sockets
    
    // ── Phase 3: CLEANUP ──────────────────────────────────
    writeSessionSummaries()
    sdManager.releaseControl()
```

### Time budget splitting

| Scenario | CLOUD budget | SMB budget |
|----------|-------------|-----------|
| Both have work | 50% of maxMs | 50% of maxMs |
| Only CLOUD has work | 100% of maxMs | 0 |
| Only SMB has work | 0 | 100% of maxMs |
| Neither has work | Skip session | Skip session |

**Recommendation**: increase `EXCLUSIVE_ACCESS_MINUTES` from 1 to 2 when both
backends are configured. This gives each backend ~60 seconds — enough for 1-3
folders per phase. Auto-scale: `effectiveMinutes = baseMinutes * activeBackendCount`.

---

## 4. Backend Cycling: Remove It

The current cycling system (`selectActiveBackend()` with timestamp comparison) is
fundamentally broken with `MINIMIZE_REBOOTS`:

1. `selectActiveBackend()` is called once in `begin()` during `setup()`
2. `activeBackend` never changes within a boot
3. Summary timestamps are written to LittleFS but never re-read
4. Pre-flight redirect only triggers when the active backend finishes ALL its work

**Proposed replacement**: no cycling at all. Both backends run in every session.
The `UploadBackend` enum, `selectActiveBackend()`, `readBackendSummary()`,
`writeBackendSummary*()` methods, and the pre-flight redirect logic are all replaced
by the phased orchestrator.

Each backend maintains its own `UploadStateManager` (already exists: `smbStateManager`
and `cloudStateManager`). The orchestrator checks both for pending work and runs
whichever has files to upload.

### GUI implications

Currently: dashboard shows "Active: SMB" / "Next: CLOUD" — stale and confusing.

Proposed: dashboard shows both backends' progress since both run each session:
- **CLOUD**: 12/24 folders (uploading 20260301...)
- **SMB**: 18/24 folders (idle — waiting for CLOUD phase)

The `g_activeBackendStatus` / `g_inactiveBackendStatus` globals are replaced with
per-backend status structs that update independently.

---

## 5. Memory Optimizations

### 5.1 Reduce lwIP pbuf pool (saves ~26KB DRAM)

```
# sdkconfig.defaults — change from:
CONFIG_LWIP_PBUF_POOL_SIZE=32    # 32 × 1.6KB = ~51KB

# to:
CONFIG_LWIP_PBUF_POOL_SIZE=16    # 16 × 1.6KB = ~26KB
```

**Justification**: during upload, only 2-3 TCP sockets are active (one TLS or SMB +
one web server). 16 pbufs is sufficient. The 32-pbuf setting was added to prevent
`EAGAIN` under concurrent load, but the phased design ensures only one upload socket
is active at a time.

**Risk**: if the web server serves many concurrent requests during upload, pbufs
could exhaust. Mitigation: the web server already rate-limits SSE clients and the
upload task disables web server handling (`setWebServer(nullptr)`).

### 5.2 Reduce upload task stack (saves 4KB)

```cpp
// Change from:
xTaskCreatePinnedToCore(..., 16384, ...)   // 16KB

// to:
xTaskCreatePinnedToCore(..., 12288, ...)   // 12KB
```

**Justification**: the task stack holds local variables only. The largest
stack-allocated buffers are:
- `uint8_t buffer[4096]` in `httpMultipartUpload()`
- `StaticJsonDocument<2048>` in `createImport()`
- `char buf[512]` in `start_ssl_client()` (inside WiFiClientSecure)
- `char line[256]` in `UploadStateManager::loadState()`

Peak stack usage is ~8-10KB. A 12KB stack gives ~2-4KB margin.

**Validation**: add `uxTaskGetStackHighWaterMark()` logging at session end to
confirm actual usage. If high-water mark shows < 3KB remaining, revert to 14KB.

### 5.3 Reduce lwIP TCP mailboxes (saves ~2KB)

```
# sdkconfig.defaults — change from:
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64
CONFIG_LWIP_TCP_RECVMBOX_SIZE=12

# to:
CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32    # default, sufficient for 2-3 sockets
CONFIG_LWIP_TCP_RECVMBOX_SIZE=8       # default
```

### 5.4 Reduce WiFi static RX buffers (saves ~3KB)

```
# Add to sdkconfig.defaults:
CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=6   # default 8, saves 2 × 1.6KB
```

### 5.5 Dynamic SMB buffer sizing

Currently: SMB buffer is allocated once in `begin()` based on `maxAlloc` at init
time and persists across all sessions.

Proposed: free SMB buffer before cloud phase, reallocate before SMB phase at
optimal size for current heap state.

```cpp
// Before cloud phase:
smbUploader->freeBuffer();

// Before SMB phase:
uint32_t ma = ESP.getMaxAllocHeap();
size_t smbSize = (ma > 60000) ? 8192 : (ma > 40000) ? 4096 : 2048;
smbUploader->allocateBuffer(smbSize);
```

### 5.6 Summary of memory savings

| Optimization | Savings | Risk |
|-------------|---------|------|
| Reduce pbufs 32→16 | ~26KB | Low (phased upload = fewer concurrent sockets) |
| Reduce task stack 16→12KB | ~4KB | Low (verify with high-water mark) |
| Reduce TCP mailboxes | ~2KB | Low |
| Reduce WiFi RX buffers | ~3KB | Low |
| Dynamic SMB buffer | ~4-8KB (during cloud phase) | None |
| **Total** | **~39-43KB** | |

With these savings, the heap budget becomes:

```
Boot:           242,948 / 110,580
Init:           ~195,000 / ~100,000   (+28KB from pbuf + mailbox savings)
Task created:   ~183,000 / ~88,000    (12KB stack vs 16KB)
TLS pre-warm:   ~139,000 / ~72,000    
SD mount:       ~115,000 / ~53,000    
                   ↓
CLOUD phase:    ma ≈ 53,000 (comfortable for TLS operations)
                   ↓ teardown TLS
SMB phase:      ma ≈ 72,000 (excellent for libsmb2)
```

This gives **~14KB more headroom** at every stage compared to today's architecture.

---

## 6. Compile-Time Optimizations (Longer-Term)

### 6.1 Custom ESP-IDF build for asymmetric mbedTLS

The `sdkconfig.defaults` settings for asymmetric TLS buffers (4KB OUT instead of
16KB) are **ignored** by the pre-compiled Arduino-ESP32 framework `.a` libraries.
The actual buffers remain symmetric 16KB IN + 16KB OUT = 32KB.

To make these effective, the project would need to build with ESP-IDF from source
(either via `platform_packages` in platformio.ini or a custom Arduino-ESP32 build).
This would save **12KB per TLS connection** (OUT shrinks from 16KB to 4KB).

**Effort**: Medium. Requires changing the PlatformIO build to use ESP-IDF framework
instead of Arduino, or creating a custom Arduino-ESP32 build with modified mbedTLS
config.

**Recommendation**: defer until Phase 1 (orchestration) is validated. The runtime
optimizations from Section 5 should provide sufficient margin.

### 6.2 Disable unused TLS features

Already done: ChaCha20-Poly1305, AES-256 disabled. Additional candidates:

```
# Disable TLS session tickets (we don't resume sessions)
CONFIG_MBEDTLS_SSL_SESSION_TICKETS=n

# Disable TLS renegotiation (server-initiated, uncommon)
CONFIG_MBEDTLS_SSL_RENEGOTIATION=n

# Disable DTLS (we only use TLS over TCP)
CONFIG_MBEDTLS_SSL_DTLS_HELLO_VERIFY=n
```

Savings: minimal (~1-2KB code, negligible heap) but reduces attack surface.

### 6.3 Disable mDNS if unused

If the device doesn't need `hostname.local` resolution:
```
CONFIG_MDNS_ENABLED=n
```
Saves ~4KB heap from mDNS responder task.

---

## 7. Implementation Plan

### Phase 1: Core Orchestration (highest priority)

**Goal**: both backends run in a single session, CLOUD first, SMB second.

| Step | Change | Files |
|------|--------|-------|
| 1a | Create `uploadOrchestrator()` function that runs both backends sequentially | `src/FileUploader.cpp` |
| 1b | Move SD mount from `handleAcquiring()` into upload task | `src/main.cpp`, `src/FileUploader.cpp` |
| 1c | Move TLS pre-warm into upload task (before SD mount) | `src/main.cpp`, `src/FileUploader.cpp` |
| 1d | Add time-budget splitting (50/50 when both have work) | `src/FileUploader.cpp` |
| 1e | Remove `selectActiveBackend()`, backend cycling, pre-flight redirect | `src/FileUploader.cpp`, `include/FileUploader.h` |
| 1f | Remove `g_activeBackendStatus` / `g_inactiveBackendStatus` cycling globals | `src/FileUploader.cpp`, `include/WebStatus.h` |
| 1g | Add per-backend status structs for GUI | `include/WebStatus.h`, `include/web_ui.h` |
| 1h | Add `uxTaskGetStackHighWaterMark()` logging at session end | `src/FileUploader.cpp` |

### Phase 2: Memory Optimizations

| Step | Change | Files |
|------|--------|-------|
| 2a | Reduce lwIP pbufs 32→16 | `sdkconfig.defaults` |
| 2b | Reduce task stack 16→12KB | `src/main.cpp` |
| 2c | Reduce TCP mailbox sizes | `sdkconfig.defaults` |
| 2d | Reduce WiFi static RX buffers 8→6 | `sdkconfig.defaults` |
| 2e | Add `SMBUploader::freeBuffer()` and dynamic re-allocation | `src/SMBUploader.cpp`, `include/SMBUploader.h` |

### Phase 3: Validation

| Step | What |
|------|------|
| 3a | Build verification: `pico32` + `pico32-ota` |
| 3b | Single-backend regression: SMB-only, CLOUD-only still work |
| 3c | Mixed-mode test: SMB+CLOUD, verify both backends upload |
| 3d | Long-run test: 10+ sessions without reboot, verify heap stability |
| 3e | Verify stack high-water marks, adjust stack size if needed |

### Phase 4: Polish (low priority)

| Step | Change |
|------|--------|
| 4a | GUI: show dual-backend progress |
| 4b | Auto-scale `EXCLUSIVE_ACCESS_MINUTES` for dual-backend |
| 4c | Consider custom ESP-IDF build for asymmetric mbedTLS |

---

## 8. Risk Analysis

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| SD mount from Core 0 causes issues | Low | High | SD_MMC driver is not core-pinned; test thoroughly |
| 12KB stack too small | Low | Medium | Log high-water mark; revert to 14KB if < 2KB margin |
| 16 pbufs insufficient under load | Low | Medium | Web server is disabled during upload; monitor for EAGAIN |
| Cloud phase takes too long, starves SMB | Medium | Low | Time budget ensures 50/50 split |
| Pre-warmed TLS dies before cloud begins | Low | Medium | Task owns the full sequence; only ~1s between pre-warm and use |
| Heap leak over many sessions still accumulates | Medium | Medium | Log heap at session start/end; reboot if fh drops below threshold |

### Graceful degradation

If heap drops below a safe threshold (e.g., ma < 30,000 after multiple sessions),
the orchestrator should request a soft reboot rather than attempting another session
that will fail. This is not the primary recovery mechanism — it's a safety net.

```cpp
if (ESP.getMaxAllocHeap() < 30000) {
    LOG_WARN("Heap critically low — requesting soft reboot for recovery");
    requestSoftReboot("heap_recovery");
    return;
}
```

---

## 9. What This Replaces

The following existing mechanisms become unnecessary with the phased orchestrator:

| Removed | Reason |
|---------|--------|
| `selectActiveBackend()` | Both backends run each session |
| `readBackendSummary()` / `writeBackendSummary*()` | No cycling needed |
| `getBackendSummaryPath()` | No summary files needed |
| Pre-flight redirect logic ("active backend has no work — redirecting") | Orchestrator checks both backends directly |
| `g_activeBackendStatus` / `g_inactiveBackendStatus` globals | Replaced by per-backend status |
| Conditional `preWarmTLS()` in `handleAcquiring()` | Pre-warm moves into upload task |
| Safety `resetConnection()` before SMB session | Orchestrator always runs CLOUD before SMB |
| `Released TLS resources before SMB session` log path | Same |

### What stays the same

| Kept | Reason |
|------|--------|
| `UploadStateManager` (per-backend) | Tracks folder/file completion independently |
| `SleepHQUploader` | Cloud upload logic unchanged |
| `SMBUploader` | SMB upload logic unchanged |
| `preWarmTLS()` | Still needed, just called from different place |
| `resetConnection()` | Still used for TLS teardown between phases |
| Per-folder SMB reconnect | Still the right granularity |
| `MINIMIZE_REBOOTS` | Works correctly with phased orchestrator |

---

## 10. Expected Outcome

After implementation:

| Metric | Current (SMB+CLOUD) | Proposed |
|--------|---------------------|----------|
| CLOUD gets a turn | Only after SMB finishes ALL work (many sessions) | Every session (Phase 1) |
| TLS handshake success | Fails at ma=38,900 | Succeeds at ma≈57,000 (before SD mount) |
| SMB `errno:9` | Every connect while TLS active | Never (TLS torn down before SMB) |
| Task creation failure | Occasional at ma=38,900 | Never (created at ma≈73,000+) |
| Reboots needed | Required for backend cycling | Not required (both run each session) |
| Heap per SMB phase | ma≈49,140 | ma≈57,332+ (TLS freed before SMB) |
| Sessions to full upload | ~30+ (SMB first, then CLOUD) | ~12-15 (both backends each session) |

---

## 11. Implementation Status

**Implemented** (2026-03-02):

| Change | File(s) | Details |
|--------|---------|---------|
| Phased orchestrator | `FileUploader.cpp` | `runFullSession()` replaces `uploadWithExclusiveAccess()`. CLOUD Phase 1 → TLS teardown → SMB Phase 2. |
| Remove backend cycling | `FileUploader.h/cpp` | Removed `selectActiveBackend()`, `BackendSummary`, summary read/write helpers. Both backends run every session. |
| Move SD+TLS into task | `main.cpp` | `uploadTaskFunction()` owns: TLS pre-warm → SD mount → upload → SD release. Task created at high `ma` before any heavy allocations. |
| Dynamic SMB buffer | `SMBUploader.h/cpp`, `FileUploader.cpp` | Added `freeBuffer()`. SMB buffer allocated at Phase 2 start (after TLS freed), freed at end. |
| Task stack 16→12KB | `main.cpp` | Stack HWM logged at session end for tuning verification. |
| lwIP pbufs 32→16 | `sdkconfig.defaults` | Saves ~26KB DRAM. Single-socket phased design needs fewer pbufs. |
| TCP mailbox reduction | `sdkconfig.defaults` | `TCPIP_RECVMBOX 64→32`, `TCP_RECVMBOX 12→8`, `UDP_RECVMBOX 8→6`. |
| WiFi RX buffers 8→6 | `sdkconfig.defaults` | Saves ~3KB. |
| Time budget splitting | `FileUploader.cpp` | When both backends have work, total time = 2× configured minutes, split evenly. |
| `begin()` no SD needed | `FileUploader.h/cpp`, `main.cpp` | State managers use LittleFS. No SD mount required during boot init. |
| GUI dual-backend | `web_ui.h`, `CpapWebServer.cpp` | Shows "DUAL" with green color. Live progress checks both session statuses. |
| Stack HWM logging | `FileUploader.cpp` | `uxTaskGetStackHighWaterMark()` logged at session end for stack tuning. |

**Verification**: Both `pico32` and `pico32-ota` build targets compile successfully.
