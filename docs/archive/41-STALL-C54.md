# SMB Stall / Watchdog Reset Analysis

## Executive Summary

**Issue Identified:** The device periodically reboots during upload sessions with `Task watchdog timeout` resets. In the captured logs, the resets occur after smart mode confirms CPAP bus silence, starts an upload task on Core 0, and enters the SMB upload path.

**Root Cause:** The failures are consistent with a blocking SMB operation stalling inside `libsmb2` long enough to trip the ESP32 task watchdog. The current code feeds the watchdog before and after SMB operations, but cannot feed it while a single blocking `libsmb2` call is stuck. As a result, a hung `smb2_write()` / related SMB call can panic the system even though the higher-level upload logic is otherwise healthy.

**Most Likely Failing Area:** `SMBUploader::upload()` during large file transfer, especially a blocking `smb2_write()` or possibly a related close/reconnect call.

**Best Fix Direction:** Do **not** start with a full async SMB rewrite. The best first fix is to set real per-command `libsmb2` timeouts and convert backend stalls into clean upload failures instead of hard watchdog resets.

---

## Scope of Investigation

This analysis is based on:

- `watchdog.txt.tmp`
- `src/main.cpp`
- `src/FileUploader.cpp`
- `src/SMBUploader.cpp`
- bundled `components/libsmb2`

The goal was to determine why the board was rebooting repeatedly on the latest firmware and what the cleanest mitigation strategy would be.

---

## What the Log Shows

### Reboot Pattern

The repeated reset sequence in `watchdog.txt.tmp` shows:

```text
Reset reason: Task watchdog timeout
System reset due to watchdog timeout - possible hang or power issue
```

at regular intervals, including:

- `05:53:47`
- `05:55:36`
- `05:57:25`
- `05:59:14`
- `06:01:04`
- `06:02:54`
- `06:04:42`
- `06:06:31`
- `06:08:21`

The cadence is very regular: boot, initialize, wait for bus silence, attempt upload, then reset.

### Transition Into Upload

Later in the same log, the FSM transition is captured clearly:

```text
[17:15:46] [FSM] 62s of bus silence confirmed
[17:15:46] [FSM] LISTENING -> ACQUIRING
[17:15:46] [FSM] ACQUIRING -> UPLOADING
[17:15:46] [FSM] Heap before upload task: fh=162280 ma=90100
[17:15:46] [FSM] Upload task started on Core 0 (non-blocking)
[17:15:47] [FSM] SD card control acquired
[17:15:47] [FileUploader] Session start: DUAL mode, maxMinutes=3×2=6 filter=2
```

The upload then proceeds through pre-flight and into the SMB phase:

```text
[17:15:49] [FileUploader] Pre-flight: smb_work=1 cloud_work=0
[17:15:49] [FileUploader] === Phase 2: SMB Session ===
```

This confirms that the watchdog resets are not happening during boot, WiFi join, NTP, or web server startup. They happen after the upload task starts and enters SMB work.

### Large-File Correlation

In the more complete later run, the last persisted line before a subsequent crash is:

```text
[17:12:13] [FileUploader] Uploading file: 20260311_231300_BRP.edf (1735602 bytes)
```

Immediately after that log cut-off, the next boot reports:

```text
Reset reason: Software panic/exception
```

This is strong evidence that the failure occurs inside the transfer path for large SMB file operations.

---

## Why This Is Not the Custom Software Watchdog

The firmware has two separate watchdog-related mechanisms:

### 1. ESP Task Watchdog

This is the watchdog that the logs are actually reporting:

- reset reason `ESP_RST_TASK_WDT`
- boot log prints `Task watchdog timeout`

### 2. Custom Upload Heartbeat Watchdog

`main.cpp` also implements a higher-level software watchdog using `g_uploadHeartbeat`:

```cpp
if (uploadTaskRunning && g_uploadHeartbeat > 0 &&
    (millis() - g_uploadHeartbeat > UPLOAD_WATCHDOG_TIMEOUT_MS)) {
    LOG_ERROR("[FSM] Upload task appears hung (no heartbeat for 2 minutes) — rebooting");
    ...
    esp_restart();
}
```

However, the log file does **not** show the custom watchdog message:

```text
[FSM] Upload task appears hung (no heartbeat for 2 minutes) — rebooting
```

and does **not** show the associated controlled reboot reason path.

So the observed resets are coming from the **ESP task watchdog panic path**, not the 2-minute application-level watchdog.

---

## Code Path Analysis

### FSM Behavior (`src/main.cpp`)

When smart mode sees enough CPAP inactivity:

1. `LISTENING -> ACQUIRING`
2. `ACQUIRING -> UPLOADING`
3. upload task is created on **Core 0**
4. task watchdog is relaxed to **30 seconds** during upload

Relevant behavior:

- `handleListening()` waits for silence
- `handleUploading()` reconfigures task WDT to 30s and launches `uploadTaskFunction()`
- upload task is pinned to Core 0

This means any single blocking call inside the upload task must return often enough for the task to keep servicing the watchdog.

### Upload Session (`src/FileUploader.cpp`)

The session flow is:

1. mount / take SD control
2. run pre-flight scan
3. decide backend work
4. enter SMB phase
5. upload files sequentially

The pre-flight scan itself can be expensive, but the strongest evidence points past pre-flight and into file transfer, because:

- later logs show pre-flight completing successfully
- the captured failure occurs while uploading a large `BRP.edf`

### SMB Transfer (`src/SMBUploader.cpp`)

The transfer loop feeds the watchdog around chunk operations:

```cpp
feedUploadHeartbeat();
bytesWritten = smb2_write(...);
...
feedUploadHeartbeat();
taskYIELD();
yield();
```

This is good as long as `smb2_write()` returns in a timely manner.

The problem is that if one `libsmb2` call blocks for too long, the code cannot feed the watchdog until control returns.

That makes the design vulnerable to a hard stall inside a single SMB library call.

---

## Why the Log Does Not Show a Better Final Error

This is an important detail.

### Controlled Reboots Flush Logs

The logger flushes explicitly on controlled `esp_restart()` paths via:

- `Logger::flushBeforeReboot()`
- `setRebootReason(...)`

That works for:

- post-upload heap recovery reboot
- custom software watchdog reboot
- web-triggered reboot/reset

### Panic / Task-WDT Resets Do Not

The observed resets are not going through those controlled paths. They are abrupt task watchdog / panic resets.

That means:

- `flushBeforeReboot()` is never reached
- `setRebootReason(...)` is never written
- periodic LittleFS flush is skipped during active upload
- the last useful log lines remain only in RAM and are lost on reset

This is why the persisted log often only shows:

- the coarse reset reason on next boot
- not the exact blocking SMB function that caused it

---

## Async SMB: Feasible, But Not the Best First Fix

The bundled `libsmb2` component does support async APIs and event-loop integration.

Examples present in the repo include:

- `components/libsmb2/examples/smb2-put-async.c`
- `components/libsmb2/examples/smb2-ls-async.c`
- `components/libsmb2/examples/smb2-ls-epoll.c`

The library exposes APIs such as:

- `smb2_open_async`
- `smb2_connect_share_async`
- `smb2_close_async`
- `smb2_get_fd`
- `smb2_which_events`
- `smb2_service_fd`

So, **yes**, SMB can be made asynchronous.

### Why a Full Async Rewrite Is Not the Best First Step

A full async migration would require substantial changes to:

- SMB connection lifecycle
- file open / write / close sequencing
- retry logic
- progress tracking
- cancellation / abort handling
- watchdog feeding model
- interaction with the upload FSM and SD ownership window

That is a valid long-term architecture if maximum robustness is needed, but it is much more invasive than necessary for the current bug.

---

## Better Immediate Fix: Use `libsmb2` Timeouts

The bundled library already supports per-command timeouts:

```c
void smb2_set_timeout(struct smb2_context *smb2, int seconds);
```

The documentation in `components/libsmb2/include/smb2/libsmb2.h` states that commands can be aborted with `SMB2_STATUS_IO_TIMEOUT` when the timeout elapses.

### Why This Is the Best First Fix

This directly addresses the root problem:

- if an SMB command wedges, it times out inside the library
- control returns to the uploader
- the task watchdog is not left starving for >30s
- the upload can fail gracefully instead of panicking the board

### Why This Is More Elegant Than Just Extending the WDT

Increasing the watchdog timeout would only mask the symptom.

If `smb2_write()` can block for 30 seconds, it can block for 60 seconds too. A longer task watchdog would simply make the device hang longer before rebooting.

A real SMB timeout turns a system-wide failure into a normal backend error.

---

## Recommended Course of Action

### Recommended Fix Order

#### 1. Add Real SMB Command Timeouts

Set a timeout on the SMB context when it is created / initialized.

Target behavior:

- SMB operations should fail cleanly in **less than** the 30s task-WDT window
- the uploader should get a timeout error instead of hanging indefinitely

A practical target is likely around **10-15 seconds** rather than 30.

#### 2. Treat SMB Timeout / Stall as a Recoverable Upload Failure

On timeout or stalled transport:

- disconnect / tear down SMB context
- retry once if appropriate
- otherwise fail the current file or session cleanly
- release SD card
- move to cooldown instead of reboot loop

#### 3. Improve Diagnostic Logging Around SMB Timeout Paths

Add precise logs such as:

- SMB open timed out
- SMB write timed out at offset X
- SMB close timed out
- reconnect attempted / reconnect failed

That will make future failures diagnosable from persisted logs even if the backend remains unreliable.

#### 4. Consider Async SMB Only as Phase 2

If timeouts + graceful failure handling still leave unacceptable stalls or responsiveness problems, then consider rewriting SMB operations around the async `libsmb2` API.

---

## Approaches Ranked by Value

### Best
- **Use `smb2_set_timeout()` + graceful session failure handling**

### Good Additional Hardening
- **retry once on timeout and recreate SMB context**
- **back off before retrying SMB in the next session**

### Not Recommended as Primary Fix
- **just increase the task watchdog timeout**
- **keep relying on reboot as the main recovery mechanism**

### Long-Term Architectural Upgrade
- **full async SMB uploader state machine**

---

## Final Conclusion

The device is rebooting because SMB operations can stall long enough to trip the ESP task watchdog during upload. The issue is not the application-level heartbeat watchdog; it is a lower-level blocking backend call that starves the upload task of opportunities to feed the task WDT.

The most elegant and least invasive fix is:

1. add real `libsmb2` command timeouts
2. convert SMB stalls into clean upload failures instead of whole-device resets
3. improve timeout-path diagnostics
4. only pursue async SMB if a later hardening pass is needed

That approach addresses the root cause while preserving the current architecture and minimizing implementation risk.
