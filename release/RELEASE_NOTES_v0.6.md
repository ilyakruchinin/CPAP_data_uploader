# v0.6-Cloud-beta — SleepHQ Cloud Upload with Cooperative SD Card Access

**HUGE progress and promising results.** SleepHQ cloud uploads are working, including during active CPAP therapy sessions.

## What's New

### SleepHQ Cloud Upload
- Direct upload to [SleepHQ](https://sleephq.com) via REST API with OAuth authentication
- Automatic **Team ID** and **Device ID** detection — these are now optional fields, only needed for multi-device setups
- Interval-based uploads (`UPLOAD_INTERVAL_MINUTES`) for near-real-time data sync
- Lazy import creation — no empty "0 files" imports cluttering your SleepHQ dashboard
- Server-side deduplication: SleepHQ rejects files it already has (HTTP 200), saving bandwidth

### Cooperative SD Card Access (the hard part)
Uploading to SleepHQ isn't difficult. **Minimising SD card access to allow simultaneous uploads while the CPAP is recording therapy data is INSANE.**

When the ESP32 uses the SD card, the CPAP cannot write to it. Early testing with 20-minute interval uploads caused therapy data corruption — 7 fragmented sessions per night instead of continuous recording, with data files reduced to empty 1KB stubs.

The entire upload architecture was redesigned to give the CPAP priority SD access:

- **Batch reads** — Small files (typically 5-6 per DATALOG folder) are read into a 48KB RAM buffer in a single SD hold (~600ms), then the SD card is immediately released. All files are uploaded from RAM while the CPAP has full SD access. 5x reduction in SD mount overhead.
- **Single-read uploads** — Each file is read from SD exactly once (was 3-4 times). The content hash is computed progressively during the read/upload stream, not as a separate pass.
- **Size-based change detection** — DATALOG `.edf` files are append-only. Instead of reading and hashing the entire file to detect changes, we just compare file sizes (metadata-only, zero SPI data transfer).
- **TLS connection reuse** — Batch uploads reuse a single TLS connection across all files. Creating a new TLS connection per file fragments the ESP32's limited heap until allocation fails.
- **Heap-aware memory management** — The 48KB batch buffer, 32KB TLS connection, and SD card FAT mount buffers can't all coexist on the ESP32's ~320KB RAM. Buffers are freed before SD remount, TLS is disconnected between upload phases, and batch buffers are allocated lazily after TLS init succeeds.
- **SD release during network I/O** — OAuth authentication, NTP sync, cloud init (~15s), TLS handshake, HTTP response wait — all release the SD card. The CPAP gets uninterrupted access during every network-bound operation.
- **Guaranteed CPAP access windows** — Between every file upload, the SD card is released for a configurable window (default 1.5s). Session statistics show the CPAP gets ~76% of SD time during active uploads.

### Session Statistics
Each upload session now logs SD card contention metrics:
```
ESP held SD: 90605 ms (24%) across 105 takes
CPAP had SD: 263336 ms (76%) across 104 releases
Avg hold: 862 ms, Longest: 9795 ms, Shortest: 404 ms
```

## Current Status

- **Uploads work 100% outside of CPAP usage** — e.g., morning scheduled upload after therapy ends
- **Uploads are relatively stable with shared use** — "upload changed files every N minutes" while CPAP is recording. This mode needs further testing (it is REALLY tricky — see 23 bug fixes below)
- **23 critical fixes** from live deployment testing, covering TLS memory exhaustion, SD mount failures from heap pressure, import failures from missing companion files, infinite retry loops, and more. See [CHANGELOG.md](docs/CHANGELOG.md) for the full details.

## Flashing

Three binary variants are provided:

| File | Use Case |
|------|----------|
| `cpap-uploader-v0.6-cloud-beta.bin` | **Standard build** — Flash via USB. 3MB app partition, no OTA. |
| `cpap-uploader-v0.6-cloud-beta-ota.bin` | **OTA build** — Flash via USB. Dual 1.5MB partitions, supports web-based firmware updates. |
| `cpap-uploader-v0.6-cloud-beta-ota-update.bin` | **OTA update only** — Upload via device web UI (for devices already running the OTA build). |

Flash via USB (all combined images flash at offset 0x0):
```bash
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 cpap-uploader-v0.6-cloud-beta.bin
```

## Configuration

SleepHQ requires these fields in `config.json`:
```json
{
  "ENDPOINT_TYPE": "CLOUD",
  "CLOUD_CLIENT_ID": "your-client-id",
  "CLOUD_CLIENT_SECRET": "your-client-secret",
  "UPLOAD_INTERVAL_MINUTES": 20
}
```

`CLOUD_TEAM_ID` and `CLOUD_DEVICE_ID` are auto-detected. See [CONFIGURATION.md](docs/CONFIGURATION.md) for all options.

## Known Limitations

- **Large file SD holds** — Files >48KB (e.g., 3MB BRP.edf) must be streamed from SD, holding the card for the full read+upload duration (~10s for 3MB). Per-chunk SD release isn't possible because `SD_MMC.end()` invalidates open file handles.
- **Session time budget** — Default 5-minute session may not complete all files in a large backlog. The device automatically retries with increasing budgets. Consider `SESSION_DURATION_SECONDS=600` for initial uploads.
- **Beta** — This is a pre-release. The cooperative SD access has been tested on a ResMed AirSense 11 but may behave differently on other machines.
