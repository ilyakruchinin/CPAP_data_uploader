# Rapid Yield Feasibility Study

## Document Purpose

Full research review and feasibility analysis of the "rapid SD-bus yield" concept
proposed in `amanuense/CPAP_data_uploader` PR #45. This document builds on the
initial summary in doc 37 (C54) with deeper electrical, software, and timing
analysis against the **current** codebase architecture.

**Verdict: Do not implement.** The concept has reasonable intent but is not
viable as described, and the current architecture already mitigates the
underlying risk through a different (and better) strategy.

---

## 1. What PR #45 Proposes

### 1.1 Problem Statement

When the ESP32 holds the SD bus for uploading (GPIO26 LOW = MUX routed to ESP),
the CPAP machine may attempt to access the card (periodic EDF flushes, RTC
updates, post-therapy writes). Without yielding, the MUX blocks the CPAP and it
may report an "SD card not detected" error.

### 1.2 Proposed Mechanism

| Component | Detail |
|---|---|
| Trigger | `FALLING` edge ISR on GPIO33 (`CS_SENSE`) |
| Signal | `volatile bool g_cpapYieldRequest` set from ISR |
| Detection points | SMB chunk loop, some `FileUploader` folder loops |
| New result | `UploadResult::YIELD_NEEDED` |
| FSM handling | Release SD bus, enter cooldown, retry from saved state |

### 1.3 Claimed Timing

The PR claims the CPAP gets its SD response within one chunk cycle (~2ms at
8KB), well within the CPAP's 100-500ms SD host command timeout.

---

## 2. Hardware Topology Analysis

### 2.1 Pin Assignments (from `pins_config.h`)

| Pin | Function | Notes |
|---|---|---|
| GPIO26 | `SD_SWITCH_PIN` | MUX control. LOW = ESP owns bus, HIGH = CPAP owns bus |
| GPIO33 | `CS_SENSE` | Tapped **upstream** of MUX on CPAP side (verified from schematic) |
| GPIO14 | `SD_CLK_PIN` | SDIO clock |
| GPIO15 | `SD_CMD_PIN` | SDIO command |
| GPIO2/4/12/13 | `SD_D0-D3` | SDIO data lines |

### 2.2 Signal Visibility

When the MUX is routed to ESP (GPIO26 LOW):

- The CPAP's SD lines are **disconnected** from the physical card
- But GPIO33 is tapped **before** the MUX
- Therefore, CPAP CS assertions on GPIO33 **are visible** to the ESP even while
  the ESP owns the bus

This means the electrical premise of the PR is **valid**: if the CPAP tries to
access the card while ESP owns the bus, GPIO33 will show activity.

### 2.3 PCNT Coexistence Concern

The current codebase uses GPIO33 for `TrafficMonitor` via the IDF 5.x PCNT
peripheral driver (`pcnt_new_unit` / `pcnt_new_channel`).

The PR proposes adding `attachInterrupt(digitalPinToInterrupt(CS_SENSE),
cpapYieldISR, FALLING)` on the **same pin**.

On ESP32, these use separate hardware paths:

- **PCNT**: routes through the GPIO matrix to a dedicated pulse counter
  peripheral
- **GPIO ISR**: routes through the GPIO interrupt matrix to the CPU

They **can** coexist in principle because they are different peripheral
consumers of the same GPIO input signal. However:

- The IDF 5.x PCNT driver configures the GPIO via `gpio_hal`. Adding
  `attachInterrupt()` calls `gpio_isr_handler_add()` which reconfigures
  interrupt enable bits on the same GPIO.
- No testing has been done to confirm this does not interfere with PCNT edge
  counting or glitch filtering.
- The PCNT glitch filter (125ns) would not apply to the GPIO ISR path, so the
  ISR would fire on noise that PCNT correctly filters out.

**Risk: Medium.** Likely works but untested on this hardware with IDF 5.x, and
the ISR would see unfiltered edges that PCNT ignores.

---

## 3. Current SD Ownership Lifecycle

### 3.1 Takeover Sequence

The current architecture already mitigates the CPAP-blocking scenario through
**proactive silence detection**:

```text
TrafficMonitor (PCNT on GPIO33)
  -> samples every 100ms
  -> tracks consecutive idle time
  -> FSM waits for 5 seconds of continuous silence (SMART_WAIT_REQUIRED_MS)
  -> only then: SDCardManager::takeControl()
    -> setControlPin(true) + 300ms settle
    -> 500ms stabilization delay
    -> GPIO drive capability reduction
    -> SD_MMC.begin() mount
```

### 3.2 Release Sequence

```text
SDCardManager::releaseControl()
  -> SD_MMC.end()
  -> GPIO drive capability restore
  -> optional 4-bit compatibility remount (if 1-bit mode was used)
  -> setControlPin(false) + 300ms settle
```

The release sequence alone takes **300-800ms** depending on the 1-bit remount
path.

### 3.3 TrafficMonitor State During Upload

- PCNT is **not suspended** during upload (only suspended during IDLE/COOLDOWN)
- But `trafficMonitor.update()` is **not called** during the UPLOADING state
- The PCNT hardware continues counting edges, but nobody reads the counter
- This means the existing PCNT infrastructure could theoretically be polled
  mid-upload as an alternative to adding a raw GPIO ISR

---

## 4. Code Paths That Hold the SD Card

Every code path that runs between `takeControl()` and `releaseControl()` would
need yield awareness for this concept to work. The current paths are:

### 4.1 Pre-Flight Scanning

**Duration:** Variable, typically 1-5 seconds depending on folder count.

```text
FileUploader::runFullSession()
  -> preflightFolderHasWork() [per backend]
    -> sd.open("/DATALOG")
    -> iterate folders
    -> scanFolderFiles() for incomplete/pending folders
    -> hasFileChanged() for recent completed folders
```

**Yield safety:** Moderate. Can abort between folder iterations. State is saved
incrementally. But open `File` handles must be closed properly.

### 4.2 Cloud Upload (SleepHQ)

**Duration:** Seconds to minutes per file depending on size and network.

```text
SleepHQUploader::httpMultipartUpload()
  -> TLS connect (if not already connected)
  -> send HTTP headers
  -> stream file data in adaptive chunks (up to 4KB)
    -> SD read -> MD5 update -> TLS write -> retry loop
  -> send multipart footer with checksum
  -> read HTTP response
```

**Yield safety:** **Very low.** Once HTTP headers with `Content-Length` are sent,
the server expects exactly that many bytes. Aborting mid-stream leaves:

- An incomplete HTTP request on the wire
- The TLS connection in an unrecoverable state
- The server-side import session potentially corrupted
- No way to resume the same file upload (must restart the entire file)

The cloud uploader has its own retry logic (2 attempts with TLS reset), but
this assumes network failures, not deliberate mid-transfer aborts.

### 4.3 SMB Upload

**Duration:** Seconds to minutes per file.

```text
SMBUploader::upload()
  -> smb2_open() remote file
  -> while (localFile.available()):
    -> localFile.read(uploadBuffer, uploadBufferSize)
    -> smb2_write() with retry/backpressure handling
  -> smb2_close()
```

**Yield safety:** Low-Medium. The chunk loop has natural check points between
reads, but:

- `smb2_write()` is a **blocking synchronous call** with no configured timeout
- If the write blocks on socket backpressure, the yield flag check never runs
- An open remote file left unclosed may cause issues on the SMB server
- The file would need to be re-uploaded from scratch (no partial resume)

### 4.4 Folder Scanning (Full Scan)

**Duration:** 1-10 seconds depending on folder/file count.

```text
FileUploader::scanDatalogFolders()
  -> iterate /DATALOG
  -> per folder: check state, apply MAX_DAYS filter
  -> scanFolderFiles() for pending folders

FileUploader::scanFolderFiles()
  -> iterate folder contents
  -> build file list
```

**Yield safety:** Moderate. Can abort between folders. But vectors of folder
names and file paths are being built in memory and would need cleanup.

### 4.5 State Persistence

State saves happen at folder boundaries (`smbStateManager->save(stateFs)`,
`cloudStateManager->save(stateFs)`), not file boundaries. State is written to
**LittleFS** (internal flash), not the SD card, so state persistence itself
does not require SD access.

---

## 5. Timing Analysis

### 5.1 CPAP SD Timeout Budget

SD host controllers typically have these timeout budgets:

| Operation | Typical Timeout |
|---|---|
| CMD response | 64 clock cycles (~0.1ms at 25MHz) |
| Data read | 100ms (TAAC + NSAC) |
| Data write (busy) | 250ms |
| Card initialization | 1 second |

The CPAP is the **host** controller. If it asserts CS and gets no response from
the card (because the MUX is routing to ESP), the timeout depends on the
host's implementation. The PR assumes 100-500ms.

### 5.2 Realistic Yield Latency

Even in the best case, the yield latency chain is:

| Step | Estimated Time |
|---|---|
| GPIO33 falling edge -> ISR fires | < 1us |
| ISR sets flag | < 1us |
| Upload loop checks flag | **0 - 30,000ms** (see below) |
| Upload function returns | < 1ms |
| FSM calls releaseControl() | **300-800ms** |
| **Total** | **300ms - 31,000ms** |

The bottleneck is step 3: **when does the upload loop next check the flag?**

- **SMB chunk loop:** checks between `smb2_write()` calls. If a write blocks
  (socket backpressure with retry delays up to 5 seconds per retry, 10
  retries), the flag is not checked for up to **50 seconds**.
- **Cloud streaming:** the PR does **not** add checks inside the cloud upload
  streaming loop at all. The cloud streaming loop in
  `httpMultipartUpload()` can run for **minutes** on large files with no
  yield check points.
- **Folder-level loops:** checks between folders, not between files within a
  folder. A single folder with many files could take **minutes**.

### 5.3 Conclusion on Timing

The PR's claim of "~2ms yield latency" is **not achievable** with the current
codebase, even if the PR's patches were ported. The actual worst-case latency
is **minutes**, not milliseconds. This is far outside any reasonable SD host
timeout budget.

---

## 6. Resume Semantics Analysis

### 6.1 What the State Managers Track

`UploadStateManager` tracks:

- **Completed folders** (by date key)
- **Pending folders** (empty folders with timestamp)
- **Per-file upload status** (path hash, MD5, size) for recent-folder rescans
- **Session metadata** (timestamps, counts)

### 6.2 What Is NOT Resumable

| Scenario | Resume Possible? | Notes |
|---|---|---|
| Yield between completed folders | Yes | Next session skips completed folders |
| Yield between files in a folder | Partial | SMB: folder not marked complete, all files retried. Cloud: same |
| Yield mid-file (SMB) | No | Remote file may be partial/corrupt. Must re-upload |
| Yield mid-file (Cloud) | No | HTTP stream corrupted. Must re-upload entire file |
| Yield mid-TLS-handshake | No | Connection must be re-established |
| Yield during cloud import session | Risky | Import may be left in incomplete state on server |
| Yield during pre-flight scan | Yes | Scan restarts next session |

### 6.3 Cloud Import Session Problem

The SleepHQ cloud backend creates an "import session" at the start of the cloud
phase. Files are uploaded into this import, and it is finalized at the end.

If a yield happens mid-import:

- The import is left open on the server
- The server may time it out or leave it in a bad state
- The next session creates a **new** import
- Files already uploaded to the old import may not be counted
- Duplicate uploads are possible

This is a **significant** complication that the PR does not address.

---

## 7. Alternative Approaches

### 7.1 Current Mitigation (Already Implemented)

The current architecture already mitigates the CPAP-blocking risk through:

1. **5-second silence detection** before takeover (TrafficMonitor + FSM)
2. **Timed upload sessions** with configurable maximum duration
3. **Automatic reboot** after each session (clean SD handoff)
4. **Per-folder state persistence** (resume across reboots)

This approach accepts that the CPAP is briefly blocked during upload, but
minimizes the risk by only taking the card when the CPAP appears genuinely
idle.

### 7.2 Cooperative PCNT Polling (Not Recommended)

Instead of a GPIO ISR, the upload loops could periodically poll the PCNT
counter (which is already running during upload) to detect CPAP activity:

```text
// Hypothetical — NOT proposed for implementation
int count = 0;
pcnt_unit_get_count(pcntUnit, &count);
if (count > threshold) { /* CPAP active, yield */ }
```

**Pros:** No ISR/PCNT coexistence risk, uses existing hardware, filtered.
**Cons:** Same fundamental problems with yield latency, unwind safety, and
cloud import sessions. Polling adds overhead. Still cannot yield mid-file
safely.

### 7.3 Shorter Session Windows (Already Tunable)

The `EXCLUSIVE_ACCESS_MINUTES` config controls maximum session duration.
Reducing it limits how long the CPAP can be blocked. Combined with silence
detection, this is already a reasonable tradeoff.

### 7.4 Upload During Known-Safe Windows Only

CPAP machines have predictable SD access patterns:

- **During therapy:** periodic EDF flushes every few minutes
- **Post-therapy:** burst of writes, then idle
- **Machine off:** no access

The current silence detection already exploits this. Uploading only during
extended silence (e.g., machine off or between flush intervals) is the safest
approach and is already what the current architecture does.

---

## 8. Risk Assessment

### 8.1 Risks of Implementing Rapid Yield

| Risk | Severity | Likelihood | Notes |
|---|---|---|---|
| Mid-file cloud upload corruption | High | High | HTTP multipart stream cannot be safely interrupted |
| Cloud import session left incomplete | High | Medium | Server-side state leak |
| PCNT/GPIO ISR coexistence bug | Medium | Low-Medium | Untested on IDF 5.x |
| Yield thrashing (false positives) | Medium | Medium | Single edge trigger, no glitch filter |
| SMB remote file left open | Medium | Medium | smb2_close not called on yield |
| Increased code complexity | Medium | Certain | Every SD-holding path needs yield awareness |
| Reduced upload throughput | Low | High | Frequent yields = less data transferred per session |

### 8.2 Risks of NOT Implementing Rapid Yield

| Risk | Severity | Likelihood | Notes |
|---|---|---|---|
| CPAP blocked during upload | Low-Medium | Low | Mitigated by 5s silence detection |
| CPAP reports SD error | Low | Very Low | Only if CPAP accesses card during upload window |

The risk of NOT implementing this is low because the current silence detection
strategy already minimizes the overlap window.

---

## 9. Comparison: PR #45 vs Current Architecture

| Aspect | PR #45 (Rapid Yield) | Current Architecture |
|---|---|---|
| Strategy | Reactive (detect + yield) | Proactive (detect silence + take) |
| Trigger | Single falling edge, unfiltered | PCNT with 125ns glitch filter, 5s sustained silence |
| Coverage | Partial (SMB chunks, some folder loops) | N/A (card taken only when safe) |
| Cloud upload safety | Not addressed | N/A (card taken before upload starts) |
| CPAP protection | Theoretically better (mid-session yield) | Good (card taken during confirmed idle) |
| Complexity | High (every SD path needs yield points) | Low (single decision point before takeover) |
| Reliability | Unproven, many edge cases | Proven in production |
| Resume semantics | Partially correct | Clean session boundaries |

---

## 10. Recommendation

### 10.1 Do Not Implement PR #45 (or any variant)

The rapid yield concept should **not** be implemented in the current codebase.

**Primary reasons:**

1. **The current proactive silence detection is a better strategy.** Taking the
   card only during confirmed idle windows is fundamentally safer than trying
   to yield mid-operation.

2. **Cloud upload cannot be safely interrupted mid-file.** The HTTP multipart
   streaming protocol requires the full file to be sent once headers are
   committed. There is no safe mid-transfer abort.

3. **The claimed timing is not achievable.** Real-world yield latency would be
   seconds to minutes, not the claimed 2ms. This exceeds any SD host timeout.

4. **The complexity cost is very high.** Every code path that holds the SD card
   would need yield awareness, safe unwind logic, and testing. This is a
   large surface area for bugs.

5. **The problem being solved is already mitigated.** The 5-second silence
   requirement before takeover means the CPAP is genuinely idle when the ESP
   takes the card. The risk of CPAP trying to access the card during upload
   is low in practice.

### 10.2 If CPAP Blocking Becomes a Real Problem

If future evidence shows that the CPAP is actually being blocked during uploads
(user reports of SD errors, logs showing CPAP activity during ESP ownership),
the correct response would be:

1. **Increase the silence threshold** (e.g., 10-15 seconds instead of 5)
2. **Reduce maximum session duration** to limit the exposure window
3. **Add PCNT-based activity logging during upload** (poll the counter
   periodically and log if CPAP activity is detected, without yielding)
4. **Only then** consider a yield mechanism, designed as a first-class feature
   with proper unwind points at safe boundaries (between folders, not
   mid-file)

### 10.3 What Would a Proper Yield Design Require?

If this were ever pursued, the minimum requirements would be:

- **Yield only at safe boundaries:** between folders, never mid-file
- **Cloud import finalization before yield:** close the import session cleanly
- **SMB file close before yield:** ensure remote files are properly closed
- **Debounced trigger:** not a single edge, but sustained activity (e.g., PCNT
  count > threshold over multiple samples)
- **Cooldown after yield:** prevent immediate re-acquire thrashing
- **Full lifecycle coverage:** pre-flight, cloud, SMB, state saves
- **Comprehensive testing:** with actual CPAP hardware generating realistic
  access patterns during upload

---

## 11. Summary

| Question | Answer |
|---|---|
| Does the PR's intent make sense? | Yes, preventing CPAP SD errors is desirable |
| Does the specific implementation work? | No, timing claims are invalid, coverage is incomplete |
| Should we port it? | No |
| Should we implement a variant? | No, not currently needed |
| Is the underlying risk real? | Low, already mitigated by silence detection |
| What should we do instead? | Nothing now; monitor for evidence of actual CPAP blocking |
