# Log Display and Stitching Analysis

## Executive Summary

**Issue Identified:** The Web GUI Logs tab exhibits out-of-order timestamps where older logs (from previous boots) appear interleaved with newer logs, creating a timeline that appears to go backwards.

**Root Cause:** The client-side log stitching algorithm (`_appendLogs()` in `web_ui.h`) has a flaw in its boot-detection logic that causes it to incorrectly include pre-reboot context lines that contain timestamps from a different time period (before NTP sync).

**Severity:** Medium — logs are confusing but not lost; download functionality works correctly.

---

## Architecture Overview

### Log Storage System (Logger.cpp / Logger.h)

The logging system uses a dual-layer architecture:

#### Layer 1: Circular RAM Buffer (Volatile)
- **Size:** 8KB (configurable via `LOG_BUFFER_SIZE`, static BSS — not heap)
- **Structure:** Monotonic indices (`headIndex`, `tailIndex`) with modulo arithmetic
- **Overflow behavior:** Overwrites oldest data, tracks `totalBytesLost`
- **Thread safety:** FreeRTOS mutex for dual-core ESP32
- **Content:** All log lines with `[HH:MM:SS] [LEVEL]` timestamps

#### Layer 2: Persistent NAND Storage (LittleFS)
- **Files:** `syslog.0.txt` (active) through `syslog.3.txt` (oldest)
- **Rotation:** 4 files × 32KB each = 128KB total budget
- **Flush interval:** Every 10 seconds (skipped during uploads for power/performance)
- **Boot separator:** `=== BOOT <version> (heap <free>/<max>) ===` written at boot
- **Gap detection:** When buffer wraps before flush, a "LOG GAP" message is inserted

### Web Log APIs (CpapWebServer.cpp)

| Endpoint | Purpose | Implementation |
|---|---|---|
| `/api/logs` | Current RAM buffer only | `Logger::printLogs()` — tail-to-head order |
| `/api/logs/full` | NAND + RAM backfill | `streamSavedLogs()` (oldest→newest) + `printLogs()` |
| `/api/logs/saved` | Download attachment | Same as `/full` but with `Content-Disposition: attachment` |
| `/api/logs/stream` | SSE live push | `pushSseLogs()` — reads new bytes from circular buffer |

### Client-Side Log Handling (web_ui.h)

**State variables:**
```javascript
var logAtBottom=true,clientLogBuf=[],lastSeenLine='',LOG_BUF_MAX=2000;
var backfillDone=false,sseConnected=false;
```

**Log tab lifecycle:**
1. `tab('logs')` called → checks `backfillDone`
2. If not done: `fetchBackfill()` → GET `/api/logs/full`
3. After backfill: `startSse()` → connects to `/api/logs/stream`
4. Fallback: `startLogPoll()` → polls `/api/logs` every 4 seconds

---

## The Stitching Algorithm

### `_appendLogs(text)` — Core Logic

The function processes server responses and maintains `clientLogBuf`:

```javascript
function _appendLogs(text){
  var lines=text.split('\n');
  
  // 1. Find the LAST boot banner in the response
  var bootIdx=-1;
  for(var i=lines.length-1;i>=0;i--){
    if(lines[i].indexOf('=== CPAP Data Auto-Uploader ===')>=0){bootIdx=i;break;}
  }
  
  // 2. Determine new lines to append
  var newLines;
  if(bootIdx>=0){
    // Boot banner found — determine if this is NEW reboot or same boot
    var lastSeenPos=-1;
    if(lastSeenLine){
      for(var i=lines.length-1;i>=0;i--){
        if(lines[i]===lastSeenLine){lastSeenPos=i;break;}
      }
    }
    
    if(lastSeenPos>bootIdx){
      // Same boot continuing — append only new tail
      newLines=lines.slice(lastSeenPos+1);
    } else if(clientLogBuf.length===0){
      // Fresh page load — include ALL lines
      newLines=lines.slice(startFrom);
    } else {
      // *** PROBLEM CASE: "Genuinely new reboot" ***
      // Searches backwards for === BOOT separator
      var ctxStart=bootIdx;
      for(var j=bootIdx-1;j>=Math.max(0,bootIdx-60);j--){
        if(lines[j].indexOf('=== BOOT ')>=0){ctxStart=Math.max(0,j-10);break;}
      }
      clientLogBuf.push('','─── DEVICE REBOOTED ───','');
      newLines=lines.slice(ctxStart);  // <-- BUG: includes old timestamps
    }
  } else {
    // No boot banner — fallback to lastSeenLine dedup
    // ... additional logic for buffer-wrap detection
  }
  // ... append to clientLogBuf, trim to LOG_BUF_MAX
}
```

---

## The Bug: Time Goes Backwards

### User's Example Explained

```
[21:10:38] [INFO] [FSM] RELEASING -> COOLDOWN     <- Previous boot (evening)
[12:16:27] [INFO] [FileUploader] Session start...   <- New boot (afternoon)
```

**What happened:**

1. **Previous boot (21:10 / 9:10 PM):**
   - Device was running, time synced via NTP
   - Logs written with timestamps ~21:10
   - State: `RELEASING -> COOLDOWN`

2. **Reboot occurs:**
   - Device reboots (soft reboot, heap recovery, or user-triggered)
   - On boot, time starts from 0 (or RTC value) until NTP syncs
   - NTP sync happens, time jumps to correct value (~12:16 / 12:16 PM)

3. **New session starts (12:16 / 12:16 PM):**
   - Logs written with new timestamps
   - State: `Session start: DUAL mode`

4. **Client fetches logs:**
   - `fetchBackfill()` calls `/api/logs/full`
   - Response contains:
     - NAND logs from previous boot (with 21:10 timestamps)
     - Current buffer with new boot (with 12:16 timestamps)
   - `_appendLogs()` detects boot banner, searches for `=== BOOT` separator
   - Includes "context" from `ctxStart` which includes 21:10 lines

### Root Cause Analysis

**The algorithm assumes:** Logs within a single boot have monotonically increasing timestamps.

**The reality:** After a reboot, time can go "backwards" because:
- NTP sync hasn't happened yet (timestamps may be 1970 or RTC drift)
- Or NTP sync happens and corrects to the actual wall time
- The previous boot had already-synced time

**Specific code flaw (web_ui.h:800-806):**
```javascript
// "Genuinely new reboot" branch
newBootDetected=true;
var ctxStart=bootIdx;
for(var j=bootIdx-1;j>=Math.max(0,bootIdx-60);j--){
  if(lines[j].indexOf('=== BOOT ')>=0){ctxStart=Math.max(0,j-10);break;}
}
clientLogBuf.push('','─── DEVICE REBOOTED ───','');
newLines=lines.slice(ctxStart);  // BUG: ctxStart may include old timestamps
```

The algorithm searches backwards for `=== BOOT` to capture pre-reboot context (like "Rebooting for clean state..."), but it doesn't validate that the timestamps in this context window are chronologically consistent with the new boot's timeline.

---

## PERSISTENT_LOGS TRUE vs FALSE Behavior

### PERSISTENT_LOGS = FALSE (Logs only in RAM buffer)

| Aspect | Behavior |
|---|---|
| Survives reboot | No — buffer is volatile |
| `/api/logs/full` content | Only current boot's logs since power-on |
| Boot context | Minimal — only what's in RAM at fetch time |
| Time backwards bug | Less likely — old timestamps disappear on reboot |
| Log gaps | Possible if buffer wraps before flush (but no persistent flush) |
| Use case | Debugging current session only |

**Stitching behavior:**
- After reboot, `clientLogBuf` is cleared (page reload)
- `fetchBackfill()` gets only current boot logs
- No old timestamps to interleave

### PERSISTENT_LOGS = TRUE (Logs flushed to LittleFS)

| Aspect | Behavior |
|---|---|
| Survives reboot | Yes — NAND logs persist across boots |
| `/api/logs/full` content | Historical logs from previous boots + current boot |
| Boot context | Extensive — multiple boots in chronological order |
| Time backwards bug | **Likely** — old timestamps from NAND interleave with new boot |
| Log gaps | Detected and annotated with "LOG GAP" message |
| Use case | Long-term troubleshooting, post-mortem analysis |

**Stitching behavior:**
- `streamSavedLogs()` streams `syslog.3.txt` → `syslog.0.txt` (oldest→newest)
- Then appends current RAM buffer
- Contains multiple boot cycles with potentially divergent timelines
- `_appendLogs()` must correctly stitch across boot boundaries

---

## Code Paths for Log Display

### Logs Tab Display (Inline viewing)

```
User clicks "Logs" tab
  └─> tab('logs') 
      ├─> if !backfillDone: fetchBackfill()
      │       └─> GET /api/logs/full
      │           └─> handleApiLogsFull()
      │               ├─> dumpSavedLogsPeriodic() — flush RAM to NAND
      │               ├─> streamSavedLogs() — send syslog.3..0 (oldest→newest)
      │               └─> printLogs() — append current RAM buffer
      │           └─> Response: [oldest NAND logs][current RAM logs]
      └─> _appendLogs(responseText) — stitch into clientLogBuf
          ├─> Detect boot banner
          ├─> Find lastSeenLine position
          ├─> Determine ctxStart (BUG: may include old timestamps)
          └─> Push to clientLogBuf
      └─> _renderLogBuf() — update DOM
```

### Download All Logs Button

```
User clicks "Download All Logs"
  └─> downloadSavedLogs()
      └─> window.location = '/api/logs/saved'
          └─> handleApiLogsSaved()
              ├─> dumpSavedLogsPeriodic() — flush RAM to NAND
              ├─> streamSavedLogs() — send syslog.3..0 (oldest→newest)
              ├─> printLogs() — append current RAM buffer
              └─> Browser download as 'cpap_logs.txt'
```

**Key difference:** Download is a single-shot file download, not stitched incrementally. The server ensures chronological order (oldest NAND → newest RAM), but the **client-side stitching bug still applies** if the user has the Logs tab open (which populates `clientLogBuf` with potentially out-of-order content before the download).

However, the download itself contains correctly-ordered content from the server. The bug manifests in the **display**, not the downloaded file.

---

## SSE (Server-Sent Events) Behavior

### Connection Lifecycle

```
Client: GET /api/logs/stream
Server: handleApiLogsStream()
  ├─> g_sseClient = server->client() — take over socket
  ├─> g_sseLastPushedIndex = Logger::getHeadIndex() — snapshot
  └─> g_sseActive = true

Main loop (every ~100ms):
  └─> pushSseLogs()
      ├─> Check g_sseClient.connected()
      ├─> Read new bytes: [g_sseLastPushedIndex .. currentHead)
      ├─> Format as SSE: "data: <line>\n\n"
      └─> Send to client

Client SSE handler:
  └─> sseSource.onmessage = function(e){
      ├─> _appendLogs(e.data + '\n')
      └─> _renderLogBuf()
  }
```

### SSE Throttling During Upload

```cpp
// In pushSseLogs() — CpapWebServer.cpp:1428
const bool uploadInProgress = uploadTaskRunning;
const unsigned long minPushIntervalMs = uploadInProgress ? 250 : 0;
const uint32_t maxBytesPerPush = uploadInProgress ? 512 : 2048;
```

During uploads:
- Minimum 250ms between pushes (vs. no throttle normally)
- Max 512 bytes per push (vs. 2048 normally)
- Prevents log traffic from interfering with upload performance

---

## Browser RAM and Buffer Management

### Client-Side Buffer (`clientLogBuf`)

- **Max lines:** 2000 (`LOG_BUF_MAX`)
- **Overflow:** Oldest lines dropped (`clientLogBuf.slice(clientLogBuf.length-LOG_BUF_MAX)`)
- **Persistence:** Survives soft reboots (JavaScript variable, not `localStorage`)
- **Clear:** `clearLogBuf()` function available in UI

### Scroll Behavior

```javascript
// Auto-scroll to bottom only if user was already near bottom
logAtBottom=(b.scrollHeight-b.scrollTop-b.clientHeight)<60;
b.textContent=clientLogBuf.join('\n');
if(logAtBottom)b.scrollTop=b.scrollHeight;
```

---

## The Problem Scenario (Step-by-Step)

### Setup
- `PERSISTENT_LOGS=TRUE` in config
- Device has been running for days
- User opens Web GUI and clicks Logs tab

### Sequence

1. **Initial page load**
   - `clientLogBuf = []` (empty)
   - `backfillDone = false`
   - `lastSeenLine = ''`

2. **User clicks Logs tab**
   - `fetchBackfill()` → GET `/api/logs/full`
   - Server responds with:
     ```
     [Lines from syslog.3.txt - days ago]
     [Lines from syslog.2.txt - yesterday]
     [Lines from syslog.1.txt - this morning]
     [Lines from syslog.0.txt - current boot, old timestamps from previous session]
     [Lines from RAM buffer - current boot, current timestamps]
     ```

3. **First `_appendLogs()` call**
   - Splits into lines
   - Searches backwards for boot banner
   - Finds boot banner at some index
   - `lastSeenLine = ''` (was empty), so `lastSeenPos = -1`
   - `clientLogBuf.length === 0`, so takes "fresh page load" branch
   - `newLines = lines.slice(startFrom)` — includes ALL lines
   - `clientLogBuf` now has 2000+ lines from multiple boots

4. **Device reboots (user-triggered or automatic)**
   - Browser keeps `clientLogBuf` in memory (SPA, no page reload)
   - Time on device resets, then NTP syncs to new time

5. **SSE reconnects or poll resumes**
   - Response contains new boot banner
   - `_appendLogs()` detects `bootIdx >= 0`
   - `lastSeenLine` is set to last line from OLD boot
   - Searches for `lastSeenLine` in response — **NOT FOUND** (buffer wrapped, different boot)
   - Takes "genuinely new reboot" branch
   - Searches backwards for `=== BOOT` separator
   - `ctxStart` ends up including lines from OLD boot with OLD timestamps
   - Adds "─── DEVICE REBOOTED ───" separator
   - Appends `lines.slice(ctxStart)` — **includes 21:10 timestamp lines**

6. **Result:**
   ```
   [21:10:38] [INFO] [FSM] RELEASING -> COOLDOWN     <- OLD, included erroneously
   [12:16:27] [INFO] [FileUploader] Session start...   <- NEW, correct
   ```

---

## Recommendations

### Immediate Workaround

**For users:** Click "Clear buffer" button (🗑 icon) in Logs tab after device reboots. This resets `clientLogBuf` and triggers a fresh `fetchBackfill()`.

### Proper Fix Options

#### Option A: Timestamp Validation (Recommended)

Modify `_appendLogs()` to validate chronological order:

```javascript
// After computing newLines, validate timestamps
function _validateAndInsert(lines, separator){
  // Parse timestamps from last line of clientLogBuf
  var lastTime = _parseTimestamp(clientLogBuf[clientLogBuf.length-1]);
  
  // Filter lines that are chronologically before lastTime
  var validLines = lines.filter(function(line){
    var t = _parseTimestamp(line);
    return t === null || t >= lastTime;  // null = no timestamp, allow through
  });
  
  if(validLines.length < lines.length){
    // Some lines were rejected — add a note
    clientLogBuf.push('[Log lines omitted: timestamps from previous boot]');
  }
  
  clientLogBuf.push(separator);
  clientLogBuf.push.apply(clientLogBuf, validLines);
}
```

#### Option B: Boot-Relative Timestamps

Change server-side to output monotonic "seconds since boot" timestamps, then convert to wall time client-side. This would make the ordering explicit.

#### Option C: Separate Buffers Per Boot

Instead of a single `clientLogBuf`, maintain an array of boot-specific buffers. The UI shows them with clear visual separation (collapsible sections).

#### Option D: Server-Side Deduplication

Add a `since=` parameter to `/api/logs/full` that accepts a timestamp or head index. Client sends its `lastSeenLine` or index, server returns only newer content. Eliminates client-side stitching complexity.

---

## Verification Steps

To confirm this analysis reproduces the issue:

1. Set `PERSISTENT_LOGS=TRUE` in config
2. Let device run for a while (generate logs)
3. Open Web GUI, click Logs tab (loads backfill)
4. Trigger a soft reboot via web UI
5. Wait for device to reconnect and NTP sync
6. Observe Logs tab — old timestamps from previous boot will appear after "─── DEVICE REBOOTED ───" separator

**Expected (correct):**
```
[12:14:00] [INFO] === BOOT v1.0i-beta2 (heap 171612/110580) ===
[12:14:05] [INFO] [FSM] IDLE -> LISTENING
...
```

**Observed (bug):**
```
─── DEVICE REBOOTED ───
[21:10:38] [INFO] [FSM] RELEASING -> COOLDOWN      <- OLD, wrong time
[12:16:27] [INFO] [FileUploader] Session start...  <- NEW, correct
```

---

## Files Involved

| File | Relevance |
|---|---|
| `src/Logger.cpp` | Circular buffer, persistent log flush, `streamSavedLogs()` |
| `include/Logger.h` | Buffer constants, `getHeadIndex()` for SSE |
| `src/CpapWebServer.cpp` | Log API handlers, SSE push logic, `pushSseLogs()` |
| `include/CpapWebServer.h` | Endpoint declarations |
| `include/web_ui.h` | Client-side JavaScript, `_appendLogs()`, `_renderLogBuf()` |

---

## Summary

The log display confusion is caused by the client-side stitching algorithm in `web_ui.h` incorrectly including pre-reboot context lines that have timestamps from a different NTP-sync period. The server sends correctly-ordered data, but the client's boot-detection heuristic fails to account for time discontinuities across reboots.

**The fix should be client-side:** either validate timestamps before appending, or simplify the stitching logic to not include "context" from previous boots.

**PERSISTENT_LOGS=TRUE makes this bug more visible** because old logs survive reboots and create the opportunity for timestamp interleaving. With `PERSISTENT_LOGS=FALSE`, the circular buffer clears on reboot, reducing (but not eliminating) the chance of this issue.
