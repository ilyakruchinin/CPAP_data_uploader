# Heap Fragmentation Re-Check — SD Mount Failures in v2.0i-beta2-dev+1

> **Status**: Analysis + Plan — no code changes yet
> **Date**: 2026-03-22
> **Log analysed**: `err-cpap_logs.txt.tmp` (v2.0i-beta2-dev+1, commit 4f89318)
> **Context**: Intermittent SD card mount failures during idle "nothing to upload"
> cycles in smart mode with MINIMIZE_REBOOTS enabled.

---

## 1. Observed Problem

After the initial successful upload session completes (~23:01), the FSM enters
a continuous LISTENING → ACQUIRING → UPLOADING → "no work" → COOLDOWN → LISTENING
loop every ~2 minutes.  **~30% of these cycles fail to mount the SD card.**

### Failure signature (100% consistent)

Every failure has `ma=36852` at the TLS pre-warm entry point.
Every success has `ma=45044` (or `ma=40948` later in the log).

| Time | `ma` at TLS pre-warm | SD Mount Result |
|------|----------------------|-----------------|
| 23:03 | 45044 | ✅ Success |
| 23:05 | **36852** | ❌ **Failed** |
| 23:07 | 45044 | ✅ Success |
| 23:09 | **36852** | ❌ **Failed** |
| 23:11 | 45044 | ✅ Success |
| 23:14 | 45044 | ✅ Success |
| 23:16 | 45044 | ✅ Success |
| 23:18 | 45044 | ✅ Success |
| 23:20 | **36852** | ❌ **Failed** |
| 23:22 | 45044 | ✅ Success |
| ... | (pattern continues) | ... |

Total failures in log: **9 out of ~30 cycles** (30% failure rate).

### The 130-second "mount duration" is a log bug

```
[23:05:35] SD card mount failed
[23:05:35] Releasing SD card. Total mount duration: 130589 ms
```

`controlAcquiredAt` is only set on **successful** mount (SDCardManager.cpp:84).
When mount fails, `releaseControl()` computes duration from the **previous**
successful mount's stale timestamp.  This is cosmetic only.

---

## 2. Root Cause: Task Stack Fragmenting the Largest Contiguous Block

### The fragmentation chain

```
Heap before upload task:    fh ≈ 114,500   ma = 45,044
    ↓ xTaskCreatePinnedToCore() allocates 12KB stack
TLS pre-warm entry:         fh ≈ 101,000   ma = 45,044  ← sometimes
                            fh ≈ 101,000   ma = 36,852  ← other times
```

The 12KB task stack is allocated by FreeRTOS from the heap.  Depending on **where**
in the heap the 12KB block lands, it may or may not split the largest contiguous
region:

- **Good placement**: 12KB carved from a non-largest region → ma stays at 45,044
- **Bad placement**: 12KB splits the 45KB region → two pieces, largest is 36,852

After TLS pre-warm succeeds (consuming ~40KB for mbedTLS buffers), the remaining
`ma` is what SD_MMC.begin() has to work with.  At `ma=36852`, the SDMMC host
driver's DMA buffer allocation fails.  At `ma=45044`, it succeeds.

### Why this is non-deterministic

FreeRTOS `pvPortMalloc()` uses a first-fit algorithm.  Over multiple cycles without
reboots (MINIMIZE_REBOOTS), small allocations from WiFi, lwIP, mDNS, web server
responses, and String temporaries create micro-holes in the heap.  The 12KB task
stack lands in different positions each cycle depending on the current hole map.

### The +101 bytes from NVS boot counter is NOT a factor

The 101 bytes is in static DRAM (`.bss`/`.data`), not on the heap.  It reduces total
heap by 101 bytes — negligible compared to the 8,192-byte swing between success
and failure (`45044 - 36852 = 8192`).

---

## 3. History of TLS Pre-Warm: Introduction, Removal, and Re-Introduction

### Timeline

| Date | Commit | Action | Rationale |
|------|--------|--------|-----------|
| 2026-03-02 | `1f6a91e` | **Introduced** TLS pre-warm in `handleAcquiring()` | Pre-warm TLS before SD mount so mbedTLS buffers are allocated at highest ma (~110KB). Eliminated HTTPClient, replaced with raw TLS I/O. |
| 2026-03-03 | `6e51c6f` | **Removed** TLS pre-warm | Believed asymmetric mbedTLS buffers (16KB IN / 4KB OUT) fit at post-SD-mount ma≈38900. TLS connects on-demand in cloud `begin()` after pre-flight confirms work. |
| 2026-03-03 | `faa6c86` | **Re-introduced** in phased orchestrator | Moved TLS pre-warm into `uploadTaskFunction()` before SD mount. Task created first at high ma, then TLS, then SD. |
| 2026-03-17 | `d4cc025` | **Removed** again during replatform | "Also remove TLS pre-warm before SD mount." Note mentioned asymmetric mbedTLS fits at post-SD heap levels. |
| 2026-03-17 | `51d756d` | **Re-introduced** again | "Restore TLS pre-warm before SD mount to prevent handshake failures. Pre-flight scanning fragments heap (ma drops from ~69KB to ~55KB). Symmetric mbedTLS buffers (16KB IN + 16KB OUT) need ~18KB contiguous." |
| Current | HEAD | TLS pre-warm **active** in `uploadTaskFunction()` Step 1 | Runs every cycle, even when there is nothing to upload. |

### The core tension

The TLS pre-warm was introduced because:
1. SD_MMC DMA buffers (~24KB) fragment the heap
2. After SD mount, ma drops too low for a TLS handshake (~36KB needed)
3. Pre-warming TLS before SD mount gives mbedTLS "first pick" of clean heap

It was removed because:
1. It wastes ~5s + ~40KB heap when there's nothing to upload
2. It holds a TLS connection that may time out before it's needed
3. It fragments the heap in a different way (TLS buffers + task stack compete)

**The fundamental problem**: TLS and SD_MMC both need large contiguous allocations,
and they fragment each other.  The order of allocation determines who wins.

---

## 4. Current Heap Budget (from err-cpap_logs.txt.tmp)

```
Boot:               227,388 / 110,580  (fh / ma)
    ↓ WiFi + mDNS + NTP + Web server + OTA + FileUploader
Post-init:          ~157,000 / ~73,000  (estimated from first successful upload)
    ↓ xTaskCreatePinnedToCore (12KB stack)
Task entry:         ~101,000 / 45,044   (good) or 36,852 (bad)
    ↓ TLS pre-warm (WiFiClientSecure + mbedTLS handshake)
Post-TLS:           ~103,000 / 45,044   (good) or 36,852 (bad)
    ↓ SD_MMC.begin() (SDMMC DMA buffers)
Post-SD mount:      succeeds (ma=45044) or FAILS (ma=36852)
```

### Permanent heap residents (always allocated after init)

| Component | Heap (approx.) | Notes |
|-----------|---------------|-------|
| WiFi driver | ~40KB | Static RX/TX buffers, crypto state |
| lwIP pbuf pool | ~19KB | 12 × 1.6KB (CONFIG_LWIP_PBUF_POOL_SIZE=12) |
| Web server | ~8KB | Handlers, SSE client, response buffers |
| mDNS responder | ~4KB | Task + multicast state (first 60s) |
| FileUploader objects | ~4KB | 2× UploadStateManager, ScheduleManager |
| SleepHQUploader | ~2KB | Config refs, String members (no TLS yet) |
| SMBUploader | ~1KB | Config refs (no buffer yet) |
| OTA manager | ~2KB | Handler + version string |
| Logger | 8KB | LOG_BUFFER_SIZE=8192 (on heap, not BSS) |
| ArduinoJson | ~1KB | Static config snapshot buffer |

### Transient (during upload cycle)

| Component | Heap (approx.) | Lifetime |
|-----------|---------------|----------|
| Upload task stack | 12KB | While task running |
| TLS connection | ~40KB | While WiFiClientSecure connected |
| SD_MMC DMA | ~24KB | While SD mounted |
| libsmb2 context | ~8-12KB | While SMB connected |
| SMB buffer | 1-8KB | Dynamic, during SMB phase |

---

## 5. What We Can Do About It

### 5A. Stop wasting TLS pre-warm on "nothing to upload" cycles (HIGH PRIORITY)

**Current flow** (every cycle, even when nothing to upload):
```
Task create → TLS pre-warm → PCNT check → SD mount → pre-flight → "no work" → release
```

**Proposed flow** — "SD-first, TLS-on-demand":
```
Task create → PCNT check → SD mount → pre-flight check
    → if work: TLS connect (on-demand) → upload → release
    → if no work: release (no TLS ever touched)
```

**Why this was abandoned before**: The commit history shows TLS was moved before SD
mount because:
1. Pre-flight scanning "permanently fragments the heap (ma drops from ~69KB to ~55KB)"
2. "Symmetric mbedTLS buffers (16KB IN + 16KB OUT) need ~18KB contiguous"
3. After SD mount + pre-flight, ma was too low for TLS handshake

**Why it can work now**: The pre-flight check in `runFullSession()` (lines 184-288)
opens `/DATALOG`, iterates directories, checks state manager for completion status,
and only opens individual files for `hasFileChanged()` checks on completed+recent
folders.  The heap impact of this scan is from:
- `File` objects (stack-allocated, ~100 bytes each)
- `String` objects for folder names (small, freed per iteration)
- `std::vector<String>` for `scanFolderFiles()` — temporary

The "permanent fragmentation" claim from commit `51d756d` (ma dropping from ~69KB
to ~55KB) likely came from the **old architecture** where pre-flight was more
expensive.  The current pre-flight is lighter and the Strings are freed before TLS
would connect.  **This needs verification with heap logging before and after
pre-flight.**

If the pre-flight does fragment the heap too much for TLS, there's a middle ground:
do a **lightweight pre-flight** (just check if any folder is not completed — no file
scanning) before TLS, then do the full scan after.

### 5B. sdkconfig inconsistency: WiFi sleep opts are still active (MEDIUM PRIORITY)

**Current `platformio.ini` (lines 48-53)**:
```ini
CONFIG_ESP_WIFI_SLP_IRAM_OPT=y
CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME=8
```

**Commit `d8dbf82`** (Mar 17) explicitly removed these with the message:
> "Remove CONFIG_ESP_WIFI_SLP_IRAM_OPT and CONFIG_ESP_WIFI_SLP_DEFAULT_MIN_ACTIVE_TIME
> to free ~2-4KB DRAM. These options were causing heap fragmentation that prevented
> TLS reconnects after initial handshake."

But **the current platformio.ini still has them**.  Either the removal was lost in a
merge, or a later commit re-added them.  The `sdkconfig.project` reference file also
still documents them as active (lines 88-94).

**Impact**: `CONFIG_ESP_WIFI_SLP_IRAM_OPT=y` sub-selects
`PM_SLP_DEFAULT_PARAMS_OPT` which adds sleep parameter tables to DRAM, reducing the
initial heap pool by ~2-4KB.  This directly reduces `ma` at all stages.

**Action**: Remove these two lines from `platformio.ini` and `sdkconfig.project`.
Delete the IDF build cache (`sdkconfig.pico32-ota`) to ensure the change takes effect.
This recovers ~2-4KB of DRAM → heap.

### 5C. Reduce lwIP pbuf pool further (LOW-MEDIUM PRIORITY)

Current: `CONFIG_LWIP_PBUF_POOL_SIZE=12` (12 × 1.6KB ≈ 19KB)

The phased upload design ensures only one network socket is active at a time (TLS
**or** SMB, never both).  The web server is also disabled during uploads
(`setWebServer(nullptr)`).  We could reduce to **8 pbufs** (~13KB), saving ~6KB.

**Risk**: Web server SSE + status polling during idle might need more than 8 pbufs
if the browser is aggressive.  Needs testing.

### 5D. Pre-allocate WiFiClientSecure once at boot, reuse forever (MEDIUM PRIORITY)

Currently, `setupTLS()` does `new WiFiClientSecure()` and `resetTLS()` does
`delete tlsClient; tlsClient = nullptr; ... setupTLS()`.  Each delete/new cycle
can place the ~200-byte WiFiClientSecure object at a different heap address,
creating micro-holes.

**Proposal**: Allocate `WiFiClientSecure` once during `FileUploader::begin()` (at
boot when heap is pristine) and keep it for the lifetime of the process.  Use
`tlsClient->stop()` to disconnect without freeing the object.  The mbedTLS internal
buffers (~32KB) will still be allocated/freed per connection, but the WiFiClientSecure
wrapper stays put.

This won't eliminate TLS buffer fragmentation, but it removes one source of
micro-holes.

### 5E. Managed components still being compiled (LOW PRIORITY, investigate)

`managed_components/` contains:
- `espressif__fb_gfx/` (frame buffer graphics — unused)
- `espressif__mdns/` (mDNS — used, but only for 60s after boot)
- `joltwallet__littlefs/` (LittleFS — used)

`fb_gfx` should be added to `custom_component_remove` if it's not already stripped
by the linker.  If it contributes to DRAM (static buffers, BSS), removing it frees
heap.

The `custom_component_remove` list in `platformio.ini` already strips 22 components
(zigbee, rainmaker, modbus, etc.), but `fb_gfx` was missed despite being listed as
a build dependency.

### 5F. Reserve/pre-allocate TLS buffers at a fixed heap address (ASPIRATIONAL)

The root cause of TLS fragmentation is that mbedTLS allocates 32KB (16KB IN + 16KB
OUT) through `mbedtls_calloc()` → `pvPortMalloc()`.  Each connect/disconnect cycle
may place these buffers at different addresses, creating permanent holes.

**Ideal solution**: Override `mbedtls_platform_set_calloc_free()` with a custom
allocator that serves the two 16KB buffers from a pre-reserved 32KB region allocated
once at boot.  All other mbedTLS allocations (handshake temps, cert chain) use the
normal heap.

**Complexity**: Medium-high.  Requires hooking mbedTLS's memory allocator, which
is possible but fragile across ESP-IDF versions.  The pioarduino hybrid compile
recompiles mbedTLS, so we have access to the build config.

**Simpler alternative**: Allocate a 32KB "TLS arena" at boot with `heap_caps_malloc
(32768, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`.  Pass this to mbedTLS via
`mbedtls_ssl_conf_recv/send` hooks.  Not straightforward — mbedTLS manages its own
buffer pointers internally.

This is aspirational and should only be pursued if 5A+5B+5C don't solve the problem.

### 5G. Asymmetric mbedTLS buffers — still ABI-forbidden (NO-GO, documented)

`CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` changes struct sizes in mbedTLS.  The hybrid
compile recompiles IDF libs (including mbedTLS) but **NOT** the Arduino framework
(`WiFiClientSecure`).  Changing struct sizes causes ABI mismatch → TLS crash.

This was already documented in `platformio.ini` (lines 38-43) and `sdkconfig.project`
(lines 76-83) as FORBIDDEN.  Would require building the Arduino framework from
source, which is a major effort.

---

## 6. Recommended Plan (ordered by impact and safety)

### Phase 1: Quick wins (no architectural change)

| Step | Change | Expected Impact | Risk |
|------|--------|----------------|------|
| 1a | Remove WiFi sleep opts from platformio.ini + sdkconfig.project | +2-4KB contiguous heap | None (they were already removed once) |
| 1b | Add `espressif__fb_gfx` to `custom_component_remove` | Small DRAM savings if it has static allocs | None |
| 1c | Fix the cosmetic `controlAcquiredAt` log bug in SDCardManager | Accurate failure diagnostics | None |
| 1d | Delete `sdkconfig.pico32-ota` build cache to force recompile | Ensures 1a takes effect | Slow first build |

### Phase 2: SD-first, TLS-on-demand (the real fix)

| Step | Change | Expected Impact | Risk |
|------|--------|----------------|------|
| 2a | Add heap logging before and after pre-flight scan in `runFullSession()` | Data to validate 5A feasibility | None |
| 2b | Move TLS pre-warm from Step 1 to **after pre-flight confirms work** | Eliminates TLS on "nothing to upload" cycles. Eliminates TLS-induced fragmentation that causes SD mount failures. | TLS may fail if pre-flight fragmented heap too much |
| 2c | If 2b fails: add **lightweight pre-flight** (folder completion check only, no file scanning) before SD mount, skip TLS + SD entirely when no work | Same benefit as 2b but with less heap impact during check | More code complexity |
| 2d | Pre-allocate WiFiClientSecure once in `begin()`, reuse | Fewer micro-holes from repeated new/delete | Low |

### Phase 3: Further heap recovery (if Phase 2 insufficient)

| Step | Change | Expected Impact | Risk |
|------|--------|----------------|------|
| 3a | Reduce lwIP pbufs 12→8 | +6KB contiguous | May starve web server during idle |
| 3b | Stop mDNS earlier (30s instead of 60s) or make configurable | +4KB after timeout | Slightly shorter discovery window |
| 3c | Investigate TLS arena pre-allocation (5F) | Eliminates TLS fragmentation entirely | Complex, fragile |

---

## 7. Expected Outcome After Phase 1+2

### "Nothing to upload" cycles (the common case after initial sync)

```
Current:  Task → TLS pre-warm (5s, 40KB) → SD mount → pre-flight → "no work" → release
          → SD mount sometimes fails due to ma=36852

Proposed: Task → PCNT check → SD mount → pre-flight → "no work" → release
          → No TLS touched → SD mount always succeeds (ma ≈ 45,044-53,000)
```

### Upload cycles (when work exists)

```
Current:  Task → TLS pre-warm → SD mount → pre-flight → upload → release
          → TLS succeeds at high ma, SD mount depends on task stack placement

Proposed: Task → PCNT check → SD mount → pre-flight → TLS connect → upload → release
          → SD mount at high ma (no TLS buffers yet)
          → TLS must succeed at post-SD-mount ma (needs verification)
```

The key risk is whether TLS can handshake at post-SD-mount `ma` values.  From
docs/14-CLOUD-HEAP-ADVICE.md, the first TLS handshake after SD mount historically
lands at `ma ≈ 49,140-51,188` (pre-replatform).  With the replatformed firmware and
the WiFi sleep opt removal (+2-4KB), this should be `ma ≈ 51,000-55,000` — well
above the ~36KB minimum for symmetric mbedTLS handshake.

If post-SD-mount `ma` is insufficient, fallback option 2c (lightweight pre-flight
without SD mount) ensures TLS pre-warm only runs when there's actually work to do,
which is a massive improvement over the current "pre-warm every 2 minutes" behavior.

---

## 8. Open Questions

1. **What is the actual `ma` after pre-flight scan?**  Need heap logging before/after
   `preflightFolderHasWork()` to validate that pre-flight doesn't permanently fragment
   below TLS threshold.  This is the key data point for Phase 2.

2. **Does `fb_gfx` contribute any DRAM?**  If it's only code (IRAM/flash), removing
   it won't help heap.  Check `.map` file or build output for `.bss`/`.data` symbols
   from fb_gfx.

3. **Is the WiFi sleep opt removal actually missing, or was it intentionally
   re-added?**  The commit history shows removal in `d8dbf82` but the current
   `platformio.ini` has them.  Need to verify whether a later commit re-added them
   deliberately for power savings, or if this is a rebase/merge artifact.
