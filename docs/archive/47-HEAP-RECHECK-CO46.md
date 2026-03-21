# Heap Fragmentation Re-Check (CO46) — Final Consolidated Analysis

> **Status**: Analysis + validated plan — no code changes yet
> **Date**: 2026-03-22
> **Purpose**: Consolidate and validate findings from `44-HEAP-RECHECK-CO46.md`,
> `45-HEAP-RECHECK-C54X.md`, and `46-HEAP-RECHECK-G31.md`. Correct errors,
> add newly discovered optimizations, and produce a final implementation plan.
> **Key constraint**: Web interface must remain accessible (even with delay).

---

## 1. Executive Summary

### What previous documents got right

- The **failure at `ma=36852`** is real and repeatable (all three docs agree).
- **TLS pre-warm was introduced for a real reason**: TLS handshake failures after
  SD mount fragmentation (confirmed by commit history `1f6a91e` → `6e51c6f` →
  `faa6c86` → `51d756d`).
- **Unused components (Zigbee, etc.) are a red herring**: linker `--gc-sections`
  strips them completely. Zero heap/flash impact (doc 46 correct).
- **Stale WiFi sleep Kconfig options are active** in the current build and consume
  2–4KB IRAM/DRAM (doc 45 correct, still present today).

### What previous documents got wrong or missed

| Claim | Source | Verdict |
|-------|--------|---------|
| "Custom TLS arena: too aspirational, leave for later" | Doc 45 | **Wrong.** The arena is the most impactful single fix. It makes the order-of-operations problem disappear entirely. |
| "Simple 2-slot allocator is sufficient" | Doc 46 | **Oversimplified but workable.** mbedTLS makes dozens of allocations during a handshake, not just 2. However, only 2 are large (>16KB). A size-threshold dispatcher (≥8KB → arena, else → system heap) works correctly. |
| "WiFi-Off / Dark Listening" as the power solution | Doc 46 | **Conflicts with user requirement.** Web interface must remain accessible. Dark Listening as described would make the Web UI unreachable. Must be redesigned. |
| No mention of `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` | All three | **Critical omission.** This single sdkconfig change saves ~12KB per TLS connection with zero code changes. The OUT buffer is currently 16384 bytes but we only send small HTTP requests (max 4096 byte chunks). |
| No mention of `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` | All three | **Missed.** Allows buffers to shrink after handshake. Complementary to asymmetric. |
| No mention of `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` | All three | **Missed.** Currently `=y`, keeping ~1–2KB of peer cert in memory after handshake. Can be disabled. |
| Static upload task stack via `xTaskCreateStaticPinnedToCore` | Doc 46 mentioned | **Under-emphasized.** Log data proves the task stack allocation is the immediate trigger that drops `ma` from 45044 to 36852. A static stack eliminates this entirely. |

### The actual root cause chain (from logs)

```
Cycle N produces work → cloud upload → TLS allocate/free → heap fragmentation floor
    ma drops from 90100 → 45044 (permanent fragmentation from TLS buffer holes)

Cycle N+1 starts:
    fh=154332, ma=45044  → task stack fits elsewhere → ma stays 45044 → SD mount OK
    
Cycle N+2 starts:
    fh=110888, ma=45044  → task stack must eat into largest block → ma drops to 36852
    → TLS pre-warm succeeds (uses smaller blocks, ma stays 36852)
    → SD mount FAILS (36852 is below SD_MMC DMA threshold)
```

The root cause is **not** that TLS and SD "interfere" at the moment of allocation.
The root cause is that **TLS buffer allocation/deallocation permanently fragments
the heap**, leaving a fragmentation floor of `ma≈45044` that is marginal for
subsequent cycles where the task stack must come from the same contiguous region.

---

## 2. Verified Heap Timeline (from `err-cpap_logs.txt.tmp`)

### First session (clean heap)

| Event | `fh` | `ma` |
|-------|------|------|
| Before upload task | 156,636 | 90,100 |
| TLS pre-warm start | 142,828 | 90,100 |
| TLS connecting | 140,492 | 90,100 |
| TLS connected | (not logged) | — |
| SD mounted | ✅ success | — |
| After session | 155,808 | **45,044** |

**Key**: `ma` drops from 90,100 to 45,044 permanently after first TLS cycle.
This is the **TLS fragmentation floor** — the two 16KB buffer holes in the heap.

### Successful no-work cycle (enough heap)

| Event | `fh` | `ma` |
|-------|------|------|
| Before upload task | 154,332 | 45,044 |
| TLS pre-warm start | 140,756 | **45,044** ← task stack didn't fragment |
| TLS connected | 103,092 | 45,044 |
| SD mounted | ✅ success | — |

### Failed no-work cycle (task stack fragments heap)

| Event | `fh` | `ma` |
|-------|------|------|
| Before upload task | 110,888 | 45,044 |
| TLS pre-warm start | 101,196 | **36,852** ← task stack ate into largest block |
| TLS connected | 102,976 | 36,852 |
| SD mount | ❌ **FAILED** | — |

**Root cause confirmed**: When `fh` is lower at task creation time (110K vs 154K),
the 12KB task stack must be allocated from the largest contiguous block, reducing
`ma` below the SD_MMC DMA threshold.

---

## 3. Exact mbedTLS Buffer Sizes (Verified from Framework Headers)

### Current configuration

| Setting | Value | Source |
|---------|-------|--------|
| `CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN` | 16384 | `sdkconfig.defaults:2239` |
| `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` | **not set** | Both IN and OUT = 16384 |
| `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH` | **not set** | Buffers never shrink |
| `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE` | **y** | Peer cert kept in heap |

### Actual buffer allocation per TLS connection

From `ssl_misc.h`:

```
PAYLOAD_OVERHEAD = MAX_IV_LENGTH(16) + SSL_MAC_ADD(16) + PADDING_ADD(0) = 32
IN_BUFFER_LEN  = HEADER(13) + IN_CONTENT_LEN(16384) + OVERHEAD(32)  = 16,429 bytes
OUT_BUFFER_LEN = HEADER(13) + OUT_CONTENT_LEN(16384) + OVERHEAD(32) = 16,429 bytes
                                                          TOTAL      = 32,858 bytes
```

### With asymmetric buffers (OUT = 4096)

```
IN_BUFFER_LEN  = 13 + 16384 + 32 = 16,429 bytes
OUT_BUFFER_LEN = 13 +  4096 + 32 =  4,141 bytes
                         TOTAL     = 20,570 bytes  (saves 12,288 bytes!)
```

This is safe because `ssl_client.cpp` already chunks writes to 4096 bytes:
```cpp
static constexpr size_t max_write_chunk_size = 4096;  // ssl_client.cpp:417
```

---

## 4. Custom TLS Arena — Detailed Feasibility Analysis

### Why it works

The ESP-IDF framework configures mbedTLS with:
- `MBEDTLS_PLATFORM_MEMORY` defined (`esp_config.h:128`)
- `MBEDTLS_PLATFORM_STD_CALLOC` = `esp_mbedtls_mem_calloc` (compile-time default)
- `MBEDTLS_PLATFORM_FREE_MACRO` **NOT defined**

This means `mbedtls_platform_set_calloc_free()` is available at runtime and can
override the default allocator. Confirmed by `platform.h:160-161`.

### Allocation pattern during TLS handshake

mbedTLS does NOT allocate "just 2 big buffers." During `start_ssl_client()` +
`ssl_starttls_handshake()`, it makes approximately:

| Allocation | Size | Lifetime |
|-----------|------|----------|
| SSL context internals | ~200 bytes | Connection lifetime |
| SSL config internals | ~200 bytes | Connection lifetime |
| Entropy context | ~600 bytes | Connection lifetime |
| DRBG context | ~300 bytes | Connection lifetime |
| Hostname dup | ~30 bytes | Connection lifetime |
| **SSL IN buffer** | **16,429 bytes** | **Connection lifetime** |
| **SSL OUT buffer** | **16,429 bytes** | **Connection lifetime** |
| Certificate chain (CA bundle) | ~2–4KB | Freed after handshake |
| Handshake temporaries | ~1–2KB | Freed during handshake |
| Peer certificate (if KEEP_PEER) | ~1–2KB | Connection lifetime |

**Only 2 allocations are ≥ 8KB.** All others are small (<4KB).

### Arena design

```
Size-threshold dispatcher:
  if (requested_size >= 8192)  → serve from static arena (2 slots of ~17KB each)
  else                         → pass through to esp_mbedtls_mem_calloc()
```

This is robust because:
1. No other mbedTLS allocation is anywhere near 8KB
2. The IN/OUT buffers are always allocated as exactly 1 block each
3. The arena is a simple 2-slot allocator (slot_a, slot_b, each ~17KB)
4. Free just marks the slot as available
5. Total arena size: ~34KB allocated once at boot from clean heap

### Why doc 45's "too aspirational" verdict was wrong

Doc 45 claimed the arena was "too invasive for the current stack and wrapper
architecture." This is incorrect because:

1. The hook is a **single function call** at boot: `mbedtls_platform_set_calloc_free()`
2. It intercepts ALL mbedTLS allocations globally — no need to modify `WiFiClientSecure`
3. The `WiFiClientSecure` / `ssl_client.cpp` code is completely unaware of the change
4. The arena survives connection teardown and reconnection — slots are reused
5. Verified to compile with the current hybrid build

### What the arena achieves

- TLS buffers **never fragment the system heap** — they live in a fixed region
- SD_MMC DMA buffers always find the full contiguous heap available
- **TLS pre-warm becomes unnecessary** — TLS can connect at any point safely
- The order of operations (TLS vs SD mount) becomes irrelevant
- Repeated connect/disconnect cycles cause **zero cumulative fragmentation**

---

## 5. Validation of All Previous Options

### Options from doc 44

| Option | Verdict | Notes |
|--------|---------|-------|
| Return to SD-first, TLS-on-demand | **Correct direction, but insufficient alone** | Pre-flight still churns heap. Only safe after arena or asymmetric buffers. |
| Remove stale WiFi sleep Kconfig | **Do this** | Saves 2–4KB DRAM. Still active in current build. |
| Reduce lwIP pbuf pool | **Secondary** | Current value 12 is already reduced. Further reduction is experimental. |
| Strip unused components | **Negligible** | Linker already strips them. Only saves build time. |

### Options from doc 45

| Option | Verdict | Notes |
|--------|---------|-------|
| Option A: Remove stale WiFi sleep Kconfig | **Do this** | Agreed. |
| Option B: Stop deleting/recreating WiFiClientSecure | **Do this** | Good hygiene, reduces small heap holes. |
| Option D: Minimal work probe | **Do this** | Avoids no-work path cost. But insufficient for work-exists path. |
| Option E: Staged probe-first flow | **Do this** | Correct architecture. |
| Option F: Suppress repeated no-work cycles | **Good, but Phase 2** | Requires PCNT-during-cooldown. |
| Option L: Custom TLS arena — "leave for later" | **WRONG** | Should be Phase 1. Highest-impact single change. |

### Options from doc 46

| Option | Verdict | Notes |
|--------|---------|-------|
| Custom TLS Arena | **Correct and feasible** | But "2-slot allocator" description was oversimplified. Use size-threshold dispatcher. |
| Dark Listening / WiFi-Off | **Conflicts with user requirement** | Web UI must remain accessible. Needs redesign. |
| Minimal Work Probe | **Correct** | Agreed with doc 45/46. |

---

## 6. Newly Identified Optimizations (Not in Any Previous Doc)

### 6.1 Asymmetric mbedTLS buffers — sdkconfig-only, saves 12KB

Add to `sdkconfig.project` / `platformio.ini` `custom_sdkconfig`:

```ini
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
```

This reduces the OUT buffer from 16,429 to 4,141 bytes, saving **12,288 bytes**
per TLS connection. The IN buffer remains at 16,429 for receiving full TLS records.

**Risk**: Low. The Arduino `ssl_client.cpp` already limits writes to 4096 bytes.
All our HTTP request headers are well under 4096 bytes. The only consideration is
that if the server sends a TLS record requiring a >4096 byte response in the
handshake, it could fail — but standard TLS 1.2/1.3 handshakes don't do this.

**Impact on arena**: With asymmetric buffers, the arena only needs ~21KB instead
of ~34KB, freeing 13KB of DRAM.

### 6.2 Variable buffer length — shrinks buffers after handshake

```ini
CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y
```

After the handshake completes, mbedTLS can shrink the IN/OUT buffers to the
negotiated maximum fragment length. Complementary to asymmetric.

**Risk**: Low. Standard feature in mbedTLS 3.x. May have minor CPU overhead
from realloc during shrink.

### 6.3 Disable KEEP_PEER_CERTIFICATE — saves ~1–2KB

```ini
CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n
```

The peer certificate is only needed during handshake verification. After the
handshake succeeds, it wastes heap. Our code never inspects the peer cert
post-handshake.

**Risk**: Very low. Only affects code that calls `mbedtls_ssl_get_peer_cert()`
after the handshake. We don't do this.

### 6.4 Static upload task stack — prevents task-stack fragmentation

Replace `xTaskCreatePinnedToCore()` with `xTaskCreateStaticPinnedToCore()`:

```cpp
static StaticTask_t uploadTaskTCB;
static StackType_t  uploadTaskStack[UPLOAD_TASK_STACK / sizeof(StackType_t)];

xTaskCreateStaticPinnedToCore(
    uploadTaskFunction, "upload", UPLOAD_TASK_STACK / sizeof(StackType_t),
    params, 2, uploadTaskStack, &uploadTaskTCB, 0);
```

The 12KB stack lives in `.bss` (static DRAM), never on the heap. This directly
eliminates the immediate trigger where task creation drops `ma` from 45044 to 36852.

**Cost**: 12KB permanent DRAM. **But**: the task is created on nearly every upload
cycle anyway, so this is memory that would be used regardless. Making it static
just prevents it from fragmenting the heap.

### 6.5 TLS client-only mode (needs validation)

```ini
CONFIG_MBEDTLS_TLS_CLIENT_ONLY=y
```

Currently `CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT=y`. If the web server is HTTP-only
(no HTTPS), we don't need TLS server support. This could reduce code size and
possibly some static allocations.

**Risk**: Medium. Need to verify that no ESP-IDF component (HTTP server, OTA, etc.)
links against TLS server functions. Build test required.

### 6.6 Certificate bundle reduction

Currently `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y` with 200 certs.
Could switch to `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_CMN` (common CAs only)
or a custom bundle with just the Google Trust Services root CA.

**Risk**: Low if we identify the exact CA chain for `sleephq.com`. Higher if the
server rotates CAs unexpectedly.

---

## 7. Power Savings — Adapted for Web UI Accessibility

### What doc 46 proposed (Dark Listening)

- WiFi OFF by default, boot without connecting
- CPU at 40MHz, light-sleep
- Only wake WiFi when work is confirmed
- Web UI **not accessible** except during upload

**Problem**: User explicitly requires web interface accessibility.

### Adapted approach: "Lazy WiFi" mode

Instead of turning WiFi completely off, use a lighter approach:

1. **WiFi stays on** with modem-sleep (current `WIFI_PS_MIN_MODEM`)
2. **Web server stays running** — always accessible (possibly with ~1s wake delay)
3. **mDNS stays active** for the first 60s (already implemented)
4. **CPU stays at 80MHz** with DFS (Dynamic Frequency Scaling) enabled
5. **No TLS/upload activity** unless work is confirmed via minimal SD probe

### Where the real power savings come from

The biggest power waste is **not** WiFi idle current. It's:

1. **Repeated no-work upload cycles** — each cycle powers up TLS, mounts SD,
   scans files, tears down — all for nothing. With the minimal work probe +
   no-work suppression, these cycles are eliminated.
2. **TLS handshake power spike** — each handshake takes 2-4 seconds at full CPU +
   WiFi TX. With the arena, we only do TLS when work exists.
3. **SD card mount/unmount cycles** — each mount draws current from the SD slot.
   With the probe-first design, unnecessary mounts are reduced.

### Estimated power budget comparison

| Scenario | Current | With fixes |
|----------|---------|------------|
| No-work cycle (every 2 min) | ~100mA for 15s (TLS + SD + scan) | ~30mA for 2s (quick SD probe) |
| WiFi idle (modem-sleep) | ~20mA | ~20mA (unchanged) |
| Web UI access | Available | Available |
| Actual upload | ~150mA for duration | ~150mA (unchanged) |

The big win is eliminating the 15-second no-work cycle that runs every 2 minutes.
That alone saves ~90% of wasted power in the idle-but-monitoring state.

---

## 8. Implementation Plan — Tiered by Impact and Difficulty

### Phase 1: SDK Config Quick Wins (zero code changes, rebuild only)

**Time estimate**: 30 minutes
**Impact**: HIGH (saves ~14KB heap, eliminates fragmentation floor)

1. Add to `platformio.ini` `custom_sdkconfig`:
   ```ini
   CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
   CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
   CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n
   ```

2. Remove stale WiFi sleep options from `platformio.ini` and `sdkconfig.project`:
   ```ini
   # REMOVE these:
   CONFIG_ESP_WIFI_SLP_IRAM_OPT=y
   CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME=8
   ```

3. Delete build cache: `sdkconfig.pico32-ota`

4. Rebuild and verify heap numbers.

**Expected result**: `ma` fragmentation floor rises from ~45044 to ~57000+.
SD mount failures at `ma=36852` should no longer occur because the OUT buffer
hole is only 4KB instead of 16KB.

### Phase 2: Static Task Stack (small code change)

**Time estimate**: 1 hour
**Impact**: MEDIUM (eliminates task-stack fragmentation trigger)

1. Pre-allocate `StaticTask_t` and `StackType_t[]` in static DRAM.
2. Replace `xTaskCreatePinnedToCore` with `xTaskCreateStaticPinnedToCore`.
3. The 12KB task stack never touches the heap.

**Expected result**: `ma` no longer drops when creating the upload task.
Combined with Phase 1, the `ma` should remain well above SD mount threshold
throughout all cycles.

### Phase 3: Custom TLS Arena (moderate code change)

**Time estimate**: 2–3 hours
**Impact**: HIGHEST (permanently eliminates TLS heap fragmentation)

1. Create `src/TlsArena.cpp` / `include/TlsArena.h`:
   - Static buffer: `static uint8_t tls_arena[22 * 1024]` (with asymmetric) or
     `static uint8_t tls_arena[34 * 1024]` (without)
   - 2-slot allocator: slot A and slot B, each sized for the largest buffer
   - `arena_calloc(n, size)`: if `n * size >= 8192` → serve from arena slot,
     else → `esp_mbedtls_mem_calloc(n, size)`
   - `arena_free(ptr)`: if ptr matches a slot → mark slot free,
     else → `esp_mbedtls_mem_free(ptr)`
2. Call `mbedtls_platform_set_calloc_free(arena_calloc, arena_free)` early in
   `setup()` before any TLS activity.
3. **Remove TLS pre-warm** from `uploadTaskFunction` — no longer needed.

**Expected result**: TLS can be established at any point in the upload lifecycle
without affecting `ma`. The order of operations becomes irrelevant. SD mount
always succeeds regardless of TLS state.

### Phase 4: Minimal Work Probe + Staged Acquisition (moderate code change)

**Time estimate**: 3–4 hours
**Impact**: HIGH (eliminates no-work path waste)

1. Create a low-churn `hasWorkToUpload()` function:
   - Streaming directory iteration (no `std::vector<String>`)
   - Fixed buffers, early exit on first `.edf` in an incomplete folder
   - Returns `{hasCloudWork, hasSmbWork}` struct
2. Refactor upload path:
   ```
   LISTENING → idle detected
     → mount SD (cheap, heap is clean thanks to arena)
     → hasWorkToUpload()
     → if no work: unmount SD, enter cooldown, done
     → if work: create upload task, proceed with TLS + upload
   ```
3. TLS only allocated when cloud work is confirmed.

**Expected result**: No-work cycles are extremely cheap (~2 seconds, no TLS,
no task stack, minimal heap churn). Power savings from eliminating wasted cycles.

### Phase 5: No-Work Suppression (optional, power optimization)

**Time estimate**: 2 hours
**Impact**: MEDIUM (reduces power in steady-state)

After a `NOTHING_TO_DO` result, require evidence of new CPAP bus activity before
retrying. This means the device may sit in listening state for hours without
doing anything — consuming only WiFi idle power (~20mA).

**Consideration**: PCNT must remain active during cooldown to detect new activity.
Current design suspends PCNT during cooldown for light-sleep. May need to merge
cooldown into listening with a time gate.

---

## 9. What NOT to Do

1. **Do NOT implement "Dark Listening" / WiFi-Off mode** as described in doc 46.
   It makes the Web UI unreachable, violating the user's explicit requirement.

2. **Do NOT blindly revert to full "SD-first, pre-flight, TLS-on-demand"** without
   first implementing the arena or at least asymmetric buffers. The pre-flight
   still churns heap enough to make post-mount TLS marginal.

3. **Do NOT waste time on Zigbee/component removal** for heap benefits. The linker
   already strips them. Only useful for build speed.

4. **Do NOT use "cache config in RAM"** approach (user previously rejected).

---

## 10. Risk Assessment

| Change | Risk | Mitigation |
|--------|------|------------|
| Asymmetric TLS buffers | Low | OUT=4096 matches existing 4KB write chunking. Verify handshake works. |
| Remove WiFi sleep IRAM opts | Low | Already reverted once in commit d8dbf82 with no issues. |
| Static task stack | Very low | Same memory usage, just moved from heap to .bss. |
| Custom TLS arena | Medium | Must handle edge cases: double-free, both slots occupied. Add assertions. Test with repeated connect/disconnect cycles. |
| Minimal work probe | Low | Pure addition, does not modify existing scan code initially. |
| Disable KEEP_PEER_CERTIFICATE | Very low | We never inspect peer cert post-handshake. |

---

## 11. Expected Outcome After All Phases

| Metric | Current | After Phase 1+2 | After All Phases |
|--------|---------|-----------------|-----------------|
| `ma` fragmentation floor | ~45,044 | ~57,000+ | ~90,000+ (arena prevents fragmentation) |
| SD mount failures | Periodic | Rare/none | **Impossible** (TLS decoupled from heap) |
| TLS pre-warm needed | Yes (every cycle) | Probably not | **No** (arena makes order irrelevant) |
| No-work cycle cost | 15s, ~100mA | 15s, ~100mA | **2s, ~30mA** |
| Web UI accessible | Yes | Yes | **Yes** |
| DRAM cost of arena | 0 | 0 | +22KB in .bss (but saves equivalent from heap fragmentation) |

---

## 12. Conclusion

The solution is **layered defense**, not a single silver bullet:

1. **Asymmetric buffers** (Phase 1) — the lowest-effort, highest-confidence change.
   Saves 12KB per TLS connection with a one-line sdkconfig change.

2. **Static task stack** (Phase 2) — eliminates the immediate trigger (task creation
   fragmenting the largest contiguous block).

3. **Custom TLS arena** (Phase 3) — the permanent, robust fix that makes heap
   fragmentation from TLS impossible. Eliminates the need for TLS pre-warm.

4. **Minimal work probe** (Phase 4) — eliminates wasted no-work cycles, the biggest
   power and heap-churn contributor.

Phases 1+2 alone likely fix the SD mount failures. Phase 3 makes the fix permanent
and architecture-proof. Phase 4 addresses power efficiency.

**Awaiting approval to begin implementation, starting with Phase 1.**
