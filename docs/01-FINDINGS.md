# Hardware Research Findings

## 1. CS_SENSE Pin Verification

### Background

The FYSETC SD-WIFI-PRO board uses a hardware multiplexer (controlled by GPIO 26) to share
the SD card bus between the ESP32 and the host device (CPAP machine). A separate GPIO is
routed from the host-side SD golden finger contacts to allow the ESP32 to **monitor bus
activity while the mux is in host mode**.

### The Discrepancy

| Source | CS_SENSE GPIO | Basis |
|---|---|---|
| FYSETC firmware (`SdWiFiBrowser/pins.h`) | 32 | Original code (likely a bug) |
| Our firmware (`pins_config.h`) | 32 | Copied from FYSETC |
| Reddit user IBNobody (reverse-engineered PCB) | **33** | Physical trace inspection |
| FYSETC schematic PDF (`SD-WIFI-PRO V1.0.pdf`) | **33** | Official schematic |

### Resolution: GPIO 33 is correct

Verified from the official FYSETC schematic PDF (from `github.com/FYSETC/SD-WIFI-PRO`):

- The schematic shows **GPIO 33** (`GPIO 33:ADC1_5`) connected via pull-up resistors to
  the host-side SD golden finger interface
- This is separate from the SDIO Slot 2 DAT3 pin (GPIO 13) used for ESP32's own SD access
- The FYSETC firmware has a bug: `#define CS_SENSE 32` should be `33`
- The bug was never caught because no firmware properly used CS_SENSE for meaningful detection

### Impact

Our firmware has been reading GPIO 32 (unused/floating) instead of GPIO 33 (actual host
bus activity). This explains:
- Why CS_SENSE never showed CPAP activity in our logs
- Why `ENABLE_CPAP_MONITOR` was disabled citing "CS_SENSE hardware issue"
- Why the CPAPMonitor stub says `<p>CPAP monitoring disabled (CS_SENSE hardware issue)</p>`

---

## 2. Why digitalRead() Fails for Bus Activity Detection

### The Problem

In SDIO 4-bit mode, the SD card's DAT3 line (which CS_SENSE monitors on the host side)
carries data at **MHz speeds**. `digitalRead()` samples at ~1 µs intervals, which is far
too slow to reliably catch bus activity. A busy bus can appear idle between samples.

Current code in `SDCardManager::takeControl()`:
```cpp
if (digitalRead(CS_SENSE) == LOW) {
    LOG("CPAP machine is using SD card, waiting...");
    return false;
}
```

This is fundamentally unreliable — it can miss activity that happens between polls.

### Solution: ESP32 PCNT Peripheral

The ESP32 has a hardware **Pulse Counter (PCNT)** peripheral that counts signal edges in
hardware, independent of CPU timing. By counting both rising and falling edges on GPIO 33,
we can detect any bus activity within a sampling window — even brief microsecond-level
pulses.

Key PCNT configuration:
- Count **both** rising and falling edges (catches any transition)
- Hardware glitch filter (~100ns) to ignore electrical noise
- Sample in windows (e.g., 100ms): if count > 0, bus is active
- Counter range: 0 to 32767 before overflow (more than sufficient)
- Zero CPU overhead during counting (hardware peripheral)

---

## 3. Verified Pin Summary

| Function | GPIO | Notes |
|---|---|---|
| **SD_SWITCH** | 26 | Mux control: HIGH = Host, LOW = ESP |
| **CS_SENSE** | **33** | Host-side bus activity monitor (CONFIRMED) |
| SD_CMD | 15 | SDIO Command (MOSI in SPI mode) |
| SD_CLK | 14 | SDIO Clock |
| SD_D0 | 2 | SDIO Data 0 (MISO in SPI mode) |
| SD_D1 | 4 | SDIO Data 1 |
| SD_D2 | 12 | SDIO Data 2 |
| SD_D3 | 13 | SDIO Data 3 / CS in SPI mode |
| SD_POWER | 27 | SD card power control (if available) |

---

## 4. Key Hardware Constraints

1. **Mux blindness**: When ESP has SD control (GPIO 26 = LOW), it **cannot** monitor host
   activity. The mux physically disconnects the host from the bus. This means we cannot
   detect if the CPAP tries to access the card during an upload.

2. **No concurrent access**: Only one side (ESP or host) can access the SD card at a time.
   The mux is a hard either/or switch.

3. **Host detects removal**: When ESP takes control, the CPAP sees the card as "removed."
   When released, it sees "insertion." This triggers card re-enumeration on the CPAP side.

4. **Card re-initialization**: After each mux switch, the accessing side must re-initialize
   the SD card (`SD_MMC.begin()`). This adds ~500ms overhead per switch.

5. **Power from host**: The SD-WIFI-PRO is powered by the CPAP's SD card slot. WiFi
   transmission draws significant current but has been stable in testing.

---

## 5. Implications for Architecture

Given these constraints, the **"Listen then Commit"** approach is the only viable strategy:

1. **Listen** (PCNT on GPIO 33): Monitor bus activity while mux is in host mode.
   Wait for sustained silence (configurable duration).

2. **Commit** (exclusive access): Take full control of the SD card. Upload as fast as
   possible without any periodic releases. The CPAP cannot access the card during this time.

3. **Release**: Give the card back. Wait a cooldown period. Then listen again.

Key insight: **We cannot detect CPAP activity during upload.** Therefore:
- We must be confident the CPAP is idle BEFORE taking control
- We should limit how long we hold the card (configurable max time)
- We should provide adequate cooldown between upload sessions
- The periodic SD release every 1.5 seconds (current implementation) is eliminated —
  it was both slow AND ineffective (the CPAP re-enumerates on each switch anyway)

---

## 6. Preliminary SD Activity Observations

> **Status**: Preliminary data (Feb 2026). Limited observation window. Values should be
> refined with more data collection over time.

Using the SD Activity Monitor (`/monitor` web UI) with the corrected GPIO 33 PCNT
detection, the following bus activity patterns were observed on an **AirSense 11 AutoSet**:

### 6.1 During Active Therapy

The CPAP machine writes to the SD card approximately **every 60 seconds** (slightly
less per timer). Between writes, the bus is completely silent.

| Metric | Observed Value |
|---|---|
| Write interval | ~55–60 seconds |
| Longest idle between writes | ~57.9 seconds |
| Consecutive idle during therapy | Never exceeds ~58 seconds |
| Pulse count per write burst | Variable (hundreds to thousands) |

### 6.2 Outside Therapy (Machine Idle / Standby)

When the CPAP is not running a therapy session, SD card access is **much less frequent**
— at least 3× longer intervals than during therapy. Idle periods of 3+ minutes are
typical.

| Metric | Observed Value |
|---|---|
| Active samples | 11 (over ~6 min observation) |
| Idle samples | 364 (over ~6 min observation) |
| Longest idle streak | 187.4 seconds (3+ minutes) |
| Typical idle gap | >180 seconds |

### 6.3 Implications for Inactivity Threshold (Z)

The inactivity threshold (Z) must be long enough to distinguish "CPAP is idle between
therapy writes" from "CPAP is truly done with the card."

| Scenario | Max Observed Idle | Recommended Z |
|---|---|---|
| During therapy | ~58 seconds | Must be **above** 58s to avoid false triggers |
| Outside therapy | 187+ seconds | Z should be **below** 187s to detect idle quickly |
| **Safe default** | — | **125 seconds** (2× therapy interval, well below standby idle) |

A default of **Z = 125 seconds** provides a comfortable 2× margin above the longest
observed therapy idle gap (~58s), while being well below the observed standby idle
periods (~187s). This ensures the FSM will not attempt to take the SD card during
active therapy, while still detecting non-therapy idle states promptly.

> **Note**: These observations are preliminary and based on limited data from a single
> AirSense 11 AutoSet. Different CPAP models may have different write patterns. The
> threshold should be tuned using the SD Activity Monitor for each deployment.

---

## 7. Cloud API & Protocol Findings (SleepHQ)

### 7.1 Multipart Upload Efficiency
Extensive testing with the SleepHQ API (v1) revealed critical optimization opportunities:

1.  **Hash-in-Footer Support**: The API accepts the `content_hash` field in the multipart **footer** (after the file content).
    *   *Impact*: Allows calculating MD5 checksums *while* streaming the file upload.
    *   *Result*: Eliminates the pre-upload read pass. Uploads are now **Single-Pass** (Read → MD5+Stream → Footer).

2.  **Chunked Transfer Encoding**: The API (served via Cloudflare) often uses `Transfer-Encoding: chunked` for responses.
    *   *Issue*: Standard `WiFiClient` doesn't handle chunked decoding automatically.
    *   *Fix*: Implemented a custom chunked decoder to properly drain responses. This is **required** to keep the TLS connection clean for reuse (keep-alive).

3.  **Batching Not Supported**:
    *   Attempting to upload multiple files in a single multipart request returns `406 Not Acceptable` or validation errors.
    *   *Conclusion*: Files must be uploaded one request per file.

### 7.2 HTTP Status Codes
*   **201 Created**: File uploaded successfully (new data).
*   **200 OK**: File skipped/deduplicated by server (hash matches existing file).
    *   *Optimization*: The firmware treats both as success. Local state avoids redundant uploads before hitting the network:
        * Root/SETTINGS files: checksum + size tracking
        * Recent DATALOG re-scan: size-only tracking (no persisted DATALOG checksum entries)

### 7.3 Memory Management
*   **Streaming is Mandatory**: Attempting to buffer multipart payloads in RAM (for small files) causes heap fragmentation over time, leading to SSL allocation failures (`-32512`) during long sessions.
*   **Solution**: All uploads, regardless of size, must use the streaming path. This keeps heap usage flatter and avoids burst allocations.

### 7.4 Heap Fragmentation Root Cause & Fix (streamfix11)

Testing showed that TLS streaming alone was necessary but not sufficient in long sessions. A second pressure source was upload state churn in `UploadStateManager`:

1. **Root cause identified**
    * Historical per-file DATALOG checksum persistence caused large `file_checksums` maps.
    * Large maps inflated `/.upload_state.json` and `DynamicJsonDocument` allocation size during `loadState()`/`saveState()`.
    * Frequent state saves (including per-file root/SETTINGS saves) increased allocation churn, reducing contiguous heap available for TLS handshakes.

2. **Fixes implemented (no reboot workaround)**
    * Removed soft-reboot recovery path entirely (no `HEAP_EXHAUSTED`, no reboot counter flow).
    * Upload state moved to **v2 line-based snapshot+journal** (`/.upload_state.v2`, `/.upload_state.v2.log`).
    * In-memory state now uses bounded fixed-size arrays (deterministic RAM footprint).
    * Recent DATALOG re-scan uses **size-only** change tracking.
    * DATALOG checksums are no longer persisted; checksum tracking remains for root/SETTINGS files.
    * Persistence uses append-only journal writes with periodic compaction, instead of high-churn full JSON rewrites.
    * Per-file immediate state save in `uploadSingleFile()` was removed; persistence happens at folder/session boundaries.

3. **Observed outcome**
    * Device completed full multi-folder upload cycles without reboot-based recovery.
    * Logs showed legacy checksum pruning on boot and stable operation under lower contiguous-heap conditions where previous builds restarted.

4. **Reimplementation guardrails**
    * Do not reintroduce reboot-based heap recovery as a primary strategy.
    * Keep DATALOG state lightweight (size-first, bounded metadata).
    * Avoid high-frequency JSON save cycles for upload state.
    * Treat contiguous heap (`max_alloc`) as a first-class runtime signal for TLS stability.
