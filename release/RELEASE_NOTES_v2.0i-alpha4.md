# CPAP AutoSync v2.0i-alpha4

**Incremental patch release on top of v2.0i-alpha3.**
OTA update from v2.0i-alpha1, alpha2, or v2.0i-alpha3 is supported — no USB flash required.

---

## Multi-Tab Detection (3-Layer System)

**Problem:** Multiple browser tabs/browsers/devices connected to the ESP32 simultaneously cause heap fragmentation, network contention, and watchdog reboots during uploads.

**Solution:** Three-layer detection system that works across tabs, browsers, AND devices:

1. **BroadcastChannel** — instant same-browser detection (Chrome↔Chrome, Firefox↔Firefox)
2. **SSE sequence counter** — server increments `sse_seq` on every SSE connect; clients detect foreign connections via `/api/status` polling (cross-browser AND cross-device)
3. **Rapid-disconnect heuristic** — SSE dropping within 10s while status works indicates another client stole the slot

**Behavior when duplicates detected:**
- Warning banner appears (pulsing red animation)
- Status polling throttles from 5s → 15s intervals
- SSE streaming stops (fallback to polling) to free TCP socket + lwIP buffers
- Banner auto-clears and SSE resumes when other clients disconnect

**Files:** `include/web_ui.h`, `src/CpapWebServer.cpp`

---

## Watchdog & Socket Timeout Hardening

**Problem:** Task watchdog crashes during TLS operations under low heap conditions. Core dumps showed the upload task hanging in `start_ssl_client()` during TLS handshake (blocking for 14+ seconds), and `tlsClient->write()` blocking indefinitely on zombie connections.

**Fixes:**
- **WDT feeding before TLS handshakes** — `esp_task_wdt_reset()` called before every `tlsClient->connect()` to give the 30s task WDT a full window for handshake completion
- **WDT feeding after handshake** — regardless of success/failure, to prevent starvation during subsequent operations
- **SO_SNDTIMEO socket option** — sets 20-second send timeout on the underlying TCP socket after TLS connect. `WiFiClientSecure::setTimeout()` only sets receive timeout; without send timeout, writes on zombie connections block forever → watchdog reboot
- **WDT feeding during header/preamble/footer writes** — protects the upload task during all TLS write phases

**Result:** Eliminates watchdog reboots from TLS blocking operations. Confirmed by core dump analysis showing upload task as the hung task on CPU 0.

**Files:** `src/SleepHQUploader.cpp`, `include/SleepHQUploader.h`

---

## Core Dump Panic Details Extraction

**Problem:** After watchdog or panic resets, the reboot reason was logged but the specific failure context (which task hung, what operation was in progress) was lost.

**Solution:** On boot after WDT/panic reset, the firmware now scans the coredump partition for the `ESP_PANIC_DETAILS` ELF note and extracts the human-readable panic reason. This appears in logs as:

```
Previous crash: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time: - upload (CPU 0)
```

**Benefit:** Immediate visibility into crash root cause without requiring manual core dump decoding.

**Files:** `src/main.cpp`

---

## Files Changed

- `include/web_ui.h` — Multi-tab detection (3-layer), auto-resume SSE on contention clear
- `src/CpapWebServer.cpp` — SSE connection counter (`sse_seq`) for cross-browser/device detection
- `src/SleepHQUploader.cpp` — WDT feeding before/after TLS handshakes, SO_SNDTIMEO socket timeouts, WDT feeds during write phases
- `include/SleepHQUploader.h` — `setSendTimeout()` declaration
- `src/main.cpp` — `extractPanicDetailsFromCoredump()` function, panic detail logging on boot
