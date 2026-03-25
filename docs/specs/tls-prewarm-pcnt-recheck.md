# [DEPRECATED] TLS Pre-Warm + Current PCNT Safety Re-Check

> **Note:** The "TLS Pre-Warm" strategy was deprecated and removed from the codebase. The memory fragmentation issues it sought to solve were permanently fixed by compiling `libfatfs` with `CONFIG_FATFS_SECTOR_512=y` and limiting `max_files` to 2. This recovered ~18KB of contiguous heap during SD mounts.

## Overview of Current PCNT Re-Check

While pre-warming is gone, the **PCNT silence re-check** remains a critical safety feature. It executes **before** mounting the SD card to confirm the CPAP machine hasn't started writing data during the brief window between `LISTENING` detection and the start of the `UPLOADING` task.

If silence is broken during this handoff, the upload cycle aborts cleanly without ever touching the SD bus.

## Design

### Sequence (inside `uploadTaskFunction`, Core 0)

```
┌─────────────────────────────────────────────────────────────┐
│ 1. PCNT Silence Re-Check                                    │
│    • Read trafficMonitor.getConsecutiveIdleMs()               │
│    • Compare against config.getInactivitySeconds() threshold  │
│    • If below threshold → CPAP may have resumed:             │
│      – Return NOTHING_TO_DO → cooldown → listening           │
│    • If at/above threshold → safe to proceed                 │
├─────────────────────────────────────────────────────────────┤
│ 2. SD Mount (SD_MMC.begin via sdManager->takeControl())      │
│    • PCNT counter becomes invalid after this point            │
│    • ESP's own SD bus activity resets the counter             │
├─────────────────────────────────────────────────────────────┤
│ 3. Pre-flight scan + phased upload (CLOUD → SMB)             │
├─────────────────────────────────────────────────────────────┤
│ 4. SD Release (SD_MMC.end via sdManager->releaseControl())   │
└─────────────────────────────────────────────────────────────┘
```

### PCNT Cross-Core Safety

The PCNT re-check reads `_consecutiveIdleMs` from Core 0 (upload task) while
Core 1 (main loop) writes it via `trafficMonitor.update()`.  This is safe
because:

- `_consecutiveIdleMs` is a `uint32_t` — 32-bit reads are atomic on ESP32
- PCNT stays active during the transition to the UPLOADING state
- The main loop continues calling `trafficMonitor.update()` while the upload
  task runs, keeping the idle counter current

## Files Modified
*This feature relies on `src/main.cpp` focusing on `uploadTaskFunction`.*

## Log Messages

| Level | Message | Meaning |
|-------|---------|---------|
| INFO | `[Upload] PCNT re-check OK: idle=Xms >= threshold=Yms` | Safe to mount SD |
| INFO | `[Upload] PCNT re-check FAILED: idle=Xms < threshold=Yms` | CPAP resumed, aborting |
| WARN | `[Upload] Aborting upload cycle to avoid SD card conflict` | Cycle aborted |
