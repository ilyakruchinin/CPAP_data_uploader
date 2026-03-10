# CPU Core Load Distribution Analysis

## Current Task Topology

The ESP32 has two cores (Core 0 and Core 1). The Arduino-ESP32 framework
assigns them as follows by default:

| Core | Role | Tasks |
|---|---|---|
| **Core 0** | Protocol core | WiFi/lwIP stack, `esp_timer`, `upload` task (pinned), FreeRTOS idle0 |
| **Core 1** | Application core | Arduino `loop()` + `setup()`, web server `handleClient()`, FSM, log flush, FreeRTOS idle1 |

### Observed Behavior (from CPU Load chart)

- **Core 0**: 5–50% load during idle, spikes to 60–70% during uploads (WiFi TX + TLS + SMB/HTTPS I/O)
- **Core 1**: Near 0% load consistently — Arduino `loop()` is lightweight (poll timers, check flags, call `handleClient()`)

This asymmetry is expected: Core 0 does all the heavy lifting (WiFi protocol
processing, TCP/TLS, and the upload task), while Core 1 just runs the main
loop which is mostly `delay()` and timer checks.

## Why Core 1 Is Nearly Idle

The main `loop()` on Core 1 does:
1. Check timers (log flush, NTP retry, WiFi reconnect) — microseconds
2. `webServer->handleClient()` — only active when a browser is connected
3. FSM state checks — simple flag/timer comparisons
4. `trafficMonitor.update()` — SD_MMC bus monitoring (lightweight)
5. Push SSE logs — a few hundred bytes at most

None of these are CPU-intensive. The upload task (which IS CPU-intensive due
to TLS crypto and SMB protocol) is pinned to Core 0.

## Analysis: Could Tasks Be Better Distributed?

### Option A: Move Upload Task to Core 1

**Pros:**
- Would balance load between cores
- Core 0 would be free for WiFi protocol processing during uploads

**Cons:**
- **WiFi/lwIP runs on Core 0** — the upload task needs tight coupling with
  the network stack. Moving it to Core 1 would add cross-core IPC latency
  for every `send()`/`recv()` call
- Arduino `loop()` runs on Core 1 — if the upload task monopolizes Core 1,
  the web server becomes unresponsive (the exact problem the current pinning
  solves)
- The ESP-IDF WiFi driver internally queues work to Core 0; having the upload
  task on Core 1 means every socket call crosses cores via FreeRTOS queues

**Verdict: Not recommended.** The current pinning is correct — upload I/O
co-locates with the network stack on Core 0 for minimum latency, while the
web server stays responsive on Core 1.

### Option B: Pin Web Server to Core 0, Upload to Core 1

**Pros:**
- Upload gets a dedicated core during heavy crypto

**Cons:**
- Web server would compete with WiFi driver on Core 0
- `handleClient()` during active uploads would starve WiFi protocol handling
- This is worse than the current arrangement

**Verdict: Not recommended.**

### Option C: Run TLS Crypto on Core 1 (Split Upload)

**Pros:**
- TLS `mbedtls_ssl_write()` / `mbedtls_ssl_read()` are the most CPU-intensive
  parts of CLOUD uploads. Running crypto on Core 1 would offload Core 0

**Cons:**
- mbedTLS is deeply integrated with the socket layer; splitting crypto from I/O
  requires major refactoring or a custom transport layer
- ESP32 has hardware AES acceleration that works from either core — the bottleneck
  is not the AES itself but the TLS record processing overhead
- Risk of introducing subtle threading bugs for marginal gain

**Verdict: High effort, marginal benefit, not recommended.**

### Option D: Use Both Cores for Parallel Folder Uploads

**Pros:**
- Could upload two folders simultaneously (e.g., one SMB, one CLOUD)

**Cons:**
- SMB and CLOUD backends share WiFi — parallelism would create socket contention
- Heap pressure would double (two TLS contexts, two SMB contexts)
- Already fragmentation-sensitive; this would make it worse
- Upload state management would need thread-safe redesign

**Verdict: Not feasible given heap constraints.**

## Recommendations

### 1. Current Pinning Is Optimal (No Change Needed)

The upload-on-Core-0, loop-on-Core-1 split is the right architecture:
- Upload co-locates with WiFi/lwIP for minimum network latency
- Web server stays responsive for user interaction during uploads
- Core 1's low utilization is actually a feature — it ensures the UI never lags

### 2. Power Consumption Impact: Minimal

Core 1 being nearly idle means it enters light-sleep between DTIM intervals
(when `g_pmLock` is released in IDLE/COOLDOWN states). The ESP32's power
management already handles this — an idle core at 80 MHz with light-sleep
draws ~2–3 mA. Redistributing tasks would not materially improve this.

### 3. Potential Minor Optimization: Log Flush Offload

The 10-second log flush (`dumpSavedLogsPeriodic`) currently runs on Core 1
(main loop). During an upload, Core 0 is busy while Core 1 is idle — this is
already optimal placement. No change needed.

### 4. Monitor for Core 0 Saturation

The real risk is Core 0 overload during intensive uploads (TLS + WiFi +
upload task). If Core 0 load consistently hits 90–100%, consider:
- Reducing WiFi TX power (lower retransmission overhead)
- Increasing `INACTIVITY_SECONDS` to reduce upload frequency
- Using `CPU_SPEED_MHZ=160` for faster TLS processing (at the cost of higher
  peak current — only for non-power-constrained setups)

## Summary

| Aspect | Status |
|---|---|
| Task pinning | ✅ Optimal — no change recommended |
| Core 1 near-idle | ✅ By design — ensures UI responsiveness |
| Power impact | ✅ Light-sleep handles idle core efficiently |
| Load balancing | ⚠️ Asymmetric but correct for this architecture |
| Action items | None — monitor Core 0 saturation during heavy uploads |
