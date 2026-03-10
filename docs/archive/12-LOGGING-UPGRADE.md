# Logging System Upgrade

## Overview

This document describes the redesigned logging architecture that replaces the
previous ad-hoc multi-file scheme with a unified, always-on, chronologically
ordered log rotation system.

## Problems with the Previous Design

| Problem | Impact |
|---|---|
| Two-file rotation (`syslog.A.txt` / `syslog.B.txt`, 64 KB each) | Only ~128 KB total retention; rotation lost the entire older file |
| Separate `/last_reboot_log.txt` written before every `esp_restart()` | Duplicated content already in syslog files; inserted out-of-order in web UI |
| `handleApiLogsFull()` appended `last_reboot_log.txt` *after* current-boot syslog | Broke chronological ordering — old reboot context appeared after new boot logs |
| Logging gated by `PERSISTENT_LOGS=true` config key | Most users never enabled it; logs lost on unexpected crashes |
| 30-second flush interval | Up to 30 seconds of log data lost on unexpected crash/watchdog reboot |
| `/crash_log.txt` and `/debug_log.txt` as additional files | Fragmented log storage across many ad-hoc files |
| Complex client-side JS dedup (boot banner detection, `lastSeenLine`) | Frequent out-of-order display; difficult to maintain |

## New Architecture

### File Rotation

```
syslog.0.txt  ← active (append target)
syslog.1.txt  ← previous
syslog.2.txt  ← older
syslog.3.txt  ← oldest (deleted on next rotation)
```

- **4 files × 32 KB max = 128 KB** total NAND budget (same as before)
- When `syslog.0.txt` exceeds 32 KB:
  - Delete `syslog.3.txt`
  - Rename `2→3`, `1→2`, `0→1`
  - Next flush creates a fresh `syslog.0.txt`

### Always-On Persistence

Logging is **always enabled** — no longer gated by `PERSISTENT_LOGS` config.
The `enableLogSaving()` call happens unconditionally early in `setup()`.

On first boot after upgrade, legacy files are cleaned up:
- `/syslog.A.txt`, `/syslog.B.txt` — deleted
- `/last_reboot_log.txt` — deleted
- `/crash_log.txt`, `/debug_log.txt` — deleted

### Flush Interval: 10 Seconds

Changed from 30s to 10s. NAND wear analysis:

- Average log throughput: ~200 bytes/second during activity
- At 10s intervals: ~2 KB per flush (append-only)
- LittleFS only rewrites the last 4 KB page on append
- Page rewrites: ~6/minute during active logging
- NAND endurance: 10K–100K erase cycles per block
- With LittleFS wear leveling across the partition: **years** of effective lifetime
- During idle periods (LISTENING state): near-zero writes (no new logs)

### Pre-Reboot Flush

The old `dumpPreRebootLog()` (which wrote a separate `/last_reboot_log.txt`) is
replaced by `flushBeforeReboot()`, which simply calls `dumpSavedLogsPeriodic()`.

This means pre-reboot content goes into the same `syslog.0.txt` rotation —
**no separate file, no chronological ordering issues**.

### Boot Separator

Each boot writes a separator line to `syslog.0.txt`:

```
=== BOOT v0.12.1 (heap 245760/113792) ===
```

This makes boot boundaries clearly visible in downloaded logs without needing
a separate file.

## API Endpoints

| Endpoint | Purpose | What it serves |
|---|---|---|
| `GET /api/logs` | Polling (circular buffer only) | Current 8 KB RAM buffer |
| `GET /api/logs/full` | Web GUI backfill | `syslog.3→0` + circular buffer (chronological) |
| `GET /api/logs/saved` | Download button | Flush first, then `syslog.3→0` + circular buffer |
| `GET /api/logs/stream` | SSE live stream | New log lines as they arrive |

**Key change**: `/api/logs/full` and `/api/logs/saved` no longer insert
`last_reboot_log.txt` content. All data is strictly chronological.

The "Download Saved Logs" button is renamed to **"Download All Logs"** and now
includes both NAND-persisted content AND current circular buffer content,
giving users a complete snapshot for troubleshooting.

## Files Changed

### `include/Logger.h`
- Added `SYSLOG_MAX_FILES` (4) and `SYSLOG_MAX_FILE_SIZE` (32 KB) constants
- Added `streamSavedLogs(Print&)` for web endpoint streaming
- Replaced `dumpPreRebootLog()` with `flushBeforeReboot()`
- Kept `dumpSavedLogs()` as legacy compatibility redirect

### `src/Logger.cpp`
- `enableLogSaving()`: migrates legacy files, writes boot separator to `syslog.0.txt`
- `dumpSavedLogsPeriodic()`: multi-file rotation with `syslog.0–3.txt`
- `streamSavedLogs()`: streams files oldest-first for web endpoints
- `flushBeforeReboot()`: calls `dumpSavedLogsPeriodic()` (no separate file)
- Removed old `dumpPreRebootLog()` (wrote `/last_reboot_log.txt`)
- Removed old `dumpSavedLogs()` crash_log.txt writer

### `src/main.cpp`
- Logging always enabled (removed `config.getSaveLogs()` gate)
- Flush interval changed from 30s to 10s
- All `dumpSavedLogsPeriodic() + dumpPreRebootLog()` pairs → single `flushBeforeReboot()`

### `src/CpapWebServer.cpp`
- `handleApiLogsSaved()`: uses `streamSavedLogs()` + `printLogs()` for complete download
- `handleApiLogsFull()`: uses `streamSavedLogs()` + `printLogs()` (no reboot log insertion)

### `include/web_ui.h`
- "Download Saved Logs" → "Download All Logs"

## Migration

On first boot after the upgrade, `enableLogSaving()` automatically:
1. Deletes legacy files (`syslog.A/B.txt`, `last_reboot_log.txt`, etc.)
2. Starts fresh with `syslog.0.txt`
3. Writes a boot separator

No user action required. The `PERSISTENT_LOGS` config key is still accepted
but no longer needed — logging is always on.
