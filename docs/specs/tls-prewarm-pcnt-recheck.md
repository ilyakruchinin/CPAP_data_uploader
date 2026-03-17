# TLS Pre-Warm + PCNT Safety Re-Check

## Overview

The upload task pre-warms the TLS connection to SleepHQ's cloud API **before**
mounting the SD card.  This gives mbedTLS first pick of the largest contiguous
heap block, avoiding the `ma=38900` allocation failures observed in mixed
SMB+CLOUD runs where SD DMA buffers fragment memory before TLS can allocate.

After the handshake completes (~2-4 seconds), a **PCNT silence re-check**
confirms the CPAP machine hasn't started using the SD card during that window.
If silence is broken, the upload cycle aborts cleanly without ever touching
the SD bus.

## Problem Statement

### Heap Fragmentation During TLS Handshake

mbedTLS requires ~36 KB of contiguous heap for the TLS handshake:

| Allocation          | Size   |
|---------------------|--------|
| IN buffer           | 16 KB  |
| OUT buffer          | 16 KB  |
| SSL context + misc  | ~4 KB  |

When TLS connects **after** `SD_MMC.begin()`, the SD DMA buffers and temporary
scan allocations carve up the heap.  The remaining largest contiguous block can
drop to ~38,900 bytes — barely above the 36 KB minimum, causing intermittent
TLS handshake failures.

### SD Card Hold Duration

The previous flow held the SD card during the entire TLS handshake (2-4 s)
plus the pre-flight scan.  Moving TLS before SD mount shaves that time off
the card-hold window, reducing the risk of CPAP interference.

## Design

### Sequence (inside `uploadTaskFunction`, Core 0)

```
┌─────────────────────────────────────────────────────────────┐
│ 1. TLS Pre-Warm (cloud-only)                                │
│    • Skip if SMB-only session (no cloud backend configured)  │
│    • Failure is non-fatal — TLS will retry on-demand later   │
│    • WDT fed after handshake                                 │
│    • Heap logged before/after for diagnostics                │
├─────────────────────────────────────────────────────────────┤
│ 2. PCNT Silence Re-Check                                     │
│    • Read trafficMonitor.getConsecutiveIdleMs()               │
│    • Compare against config.getInactivitySeconds() threshold  │
│    • If below threshold → CPAP may have resumed:             │
│      – Release pre-warmed TLS (resetConnection)              │
│      – Return NOTHING_TO_DO → cooldown → listening           │
│    • If at/above threshold → safe to proceed                 │
├─────────────────────────────────────────────────────────────┤
│ 3. SD Mount (SD_MMC.begin via sdManager->takeControl())      │
│    • PCNT counter becomes invalid after this point            │
│    • ESP's own SD bus activity resets the counter             │
├─────────────────────────────────────────────────────────────┤
│ 4. Pre-flight scan + phased upload (CLOUD → SMB)             │
│ 5. SD Release (SD_MMC.end via sdManager->releaseControl())   │
└─────────────────────────────────────────────────────────────┘
```

### Heap Layout Comparison

**Before (TLS after SD mount):**
```
[██ SD DMA ██][temp allocs...][████ TLS 36KB ████]  ← TLS may FAIL
                               ^ must find 36KB after SD+temps
```

**After (TLS before SD mount):**
```
[████ TLS 36KB ████][██ SD DMA ██]  ← TLS gets first pick
 ^ cleanest contiguous heap
```

### PCNT Cross-Core Safety

The PCNT re-check reads `_consecutiveIdleMs` from Core 0 (upload task) while
Core 1 (main loop) writes it via `trafficMonitor.update()`.  This is safe
because:

- `_consecutiveIdleMs` is a `uint32_t` — 32-bit reads are atomic on ESP32
- PCNT stays active during UPLOADING state (not a low-power state)
- The main loop continues calling `trafficMonitor.update()` while the upload
  task runs, keeping the idle counter current

## Edge Cases

| Scenario | Handling |
|----------|----------|
| **SMB-only** (no cloud backend) | TLS pre-warm skipped; PCNT re-check still runs |
| **TLS pre-warm fails** | Non-fatal. Logged as warning. Upload proceeds; cloud phase connects on-demand |
| **PCNT re-check fails** | TLS cleaned up, upload aborted with `NOTHING_TO_DO` → cooldown → retry next cycle |
| **SD mount fails after TLS** | TLS cleaned up (`resetConnection`), returns `ERROR` |
| **DUAL backend: no cloud work found** | Safety `resetConnection()` in `runFullSession` before SMB phase releases the lingering TLS socket to prevent errno:9 conflicts with libsmb2 |
| **Manual Force Upload** | Same flow — TLS pre-warm + PCNT re-check still apply |

## Files Modified

| File | Change |
|------|--------|
| `src/main.cpp` | `uploadTaskFunction`: TLS pre-warm + PCNT re-check before `takeControl()` |
| `src/main.cpp` | `handleAcquiring`: Updated comment documenting new lifecycle |
| `src/FileUploader.cpp` | `runFullSession`: Safety TLS cleanup before SMB phase |
| `include/FileUploader.h` | Updated `runFullSession` declaration comment |

## Configuration

No new configuration keys.  The feature uses existing settings:

- **`INACTIVITY_SECONDS`**: Silence threshold (default 62s) — used for both
  the initial detection in `handleListening()` and the re-check
- **Cloud endpoint**: Pre-warm only runs when `hasCloudBackend()` returns true

## Log Messages

| Level | Message | Meaning |
|-------|---------|---------|
| INFO | `[Upload] TLS pre-warm: heap before fh=X ma=Y` | Pre-warm starting |
| INFO | `[Upload] TLS pre-warm succeeded (fh=X ma=Y)` | Handshake complete |
| WARN | `[Upload] TLS pre-warm failed (non-fatal, will retry on-demand)` | Handshake failed, proceeding |
| INFO | `[Upload] PCNT re-check OK: idle=Xms >= threshold=Yms` | Safe to mount SD |
| INFO | `[Upload] PCNT re-check FAILED: idle=Xms < threshold=Yms` | CPAP resumed, aborting |
| WARN | `[Upload] Aborting upload cycle to avoid SD card conflict` | Cycle aborted |
| INFO | `[Upload] Released pre-warmed TLS after PCNT abort` | Cleanup on abort |
| INFO | `[FileUploader] Releasing pre-warmed TLS before SMB phase` | Safety cleanup in DUAL mode |

## Testing Checklist

- [ ] Cloud-only: TLS pre-warm succeeds, upload proceeds normally
- [ ] SMB-only: TLS pre-warm skipped, PCNT re-check still runs
- [ ] DUAL backend: TLS pre-warm + cloud phase + TLS cleanup + SMB phase
- [ ] DUAL backend, no cloud work: safety cleanup releases TLS before SMB
- [ ] PCNT re-check failure: cycle aborts, returns to listening next cycle
- [ ] TLS pre-warm failure: upload proceeds with on-demand TLS connect
- [ ] Heap metrics: verify ma is higher when TLS connects before SD mount
