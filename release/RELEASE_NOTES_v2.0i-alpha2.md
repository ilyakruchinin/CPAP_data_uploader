# CPAP AutoSync v2.0i-alpha2

**Incremental patch release on top of v2.0i-alpha1.**
OTA update from v2.0i-alpha1 is supported — no USB flash required.

---

## Bug Fixes

### Task Watchdog Crash During Large Cloud Uploads

**Symptom:** Uploading large files (>100 KB) to SleepHQ could trigger an ESP-IDF task watchdog reset, crashing and rebooting the device mid-upload. The upload would succeed on the next attempt after reboot.

**Root cause:** The TLS streaming write loop fed the watchdog only after each complete chunk was written. A single `tlsClient->write()` call can block for >30 seconds when TCP flow control stalls (server slow to ACK, network congestion). Since the 30-second task watchdog was not fed during the blocking call, it fired.

**Fix:** The watchdog is now fed after every successful partial TLS write and during EAGAIN retry back-off within the inner write loop. This ensures the watchdog stays happy even if individual write calls take significant time due to network conditions.

### OTA Firmware Upload: Duplicate Content-Length Headers

**Symptom:** Browser showed `ERR_RESPONSE_HEADERS_MULTIPLE_CONTENT_LENGTH` after OTA firmware upload, even though the update succeeded and the device rebooted correctly.

**Root cause:** The Arduino WebServer framework calls the upload handler (chunk callback) for each data chunk, then calls the completion handler to send the HTTP response. The upload handler was also calling `server->send()` on success, producing two sets of response headers on the same connection.

**Fix:** All `server->send()` calls removed from the upload chunk handler. The completion handler is now the sole place that sends the HTTP response, then triggers the device restart.

### OTA Upload: "Network Error" on Success

**Symptom:** After a successful OTA upload, the web UI displayed "Network error" instead of a success message with countdown.

**Root cause:** The device reboots after a successful OTA, which kills the TCP connection. The XHR `error` event fires because the connection drops, and the JavaScript error handler showed "Network error" unconditionally.

**Fix:** The upload JavaScript now tracks whether the file transfer reached 100%. If a network error occurs after full upload, it is treated as a successful OTA (device is rebooting) and shows the success message with redirect countdown.

### PCNT Re-Check Bypass for Manual Upload Trigger

**Symptom:** When triggering an upload via the web interface, the PCNT silence re-check still aborted the upload cycle (because the CPAP was actively using the SD card or insufficient idle time had accumulated).

**Root cause:** The `g_triggerUploadFlag` was cleared in the FSM's LISTENING handler before the UPLOADING handler created the upload task parameters. By the time `params->forceTriggered` was read, the flag was already `false`.

**Fix:** A latched copy (`g_uploadWasForceTriggered`) is set when the FSM transitions due to a web trigger and consumed when the upload task params are created. The PCNT re-check still runs and logs idle time for diagnostics, but no longer aborts force-triggered uploads.

---

## Improvements

### System Tab Graphs: Time-Based Rendering with Gap Detection

**Problem:** The Heap History and CPU Load graphs on the System tab used index-based positioning with no timestamps. When the browser tab was backgrounded (e.g., switching apps on mobile), polling paused but old data points remained in the array. On return, the graph displayed stale data as if it were continuous recent activity, creating a misleading picture.

**Fix:**
- Data points now carry timestamps (`Date.now()` at collection time)
- Points older than 2 minutes are pruned by age, not by count
- X-axis positioning is time-proportional (maps to the fixed 2-minute window)
- Gaps >6.5 seconds (2× the poll interval) break the SVG path — missing periods show as empty space rather than interpolated lines
- Switching between tabs (e.g., Dashboard → System) preserves data continuity for short absences

---

## Files Changed

- `src/SleepHQUploader.cpp` — WDT feed in TLS inner write loop
- `src/CpapWebServer.cpp` — OTA response handling, PCNT force-trigger latch
- `src/main.cpp` — `g_uploadWasForceTriggered` latch variable
- `include/web_ui.h` — Time-based graph rendering with gap detection
