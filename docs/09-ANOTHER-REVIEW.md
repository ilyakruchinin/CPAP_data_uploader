# Post-v0.11.0 Follow-Up Review

Date: 2026-03-01
Based on: Updated GitHub Issues [#29](https://github.com/amanuense/CPAP_data_uploader/issues/29), [#42](https://github.com/amanuense/CPAP_data_uploader/issues/42), and previous power optimization research (`docs/06-*`, `docs/07-*`).

This document supersedes previous analyses and re-evaluates the user feedback based on new data, logs, and a deeper comparison between SMB and CLOUD resource profiles.

---

## 1. Issue #42: Persistent SD Card Error (Singapore AS11)

### 1.1 The Finding
The reporter confirmed testing v0.11.0 and reported that the SD Card Error still occurred. No new logs were provided for this specific v0.11.0 test. (The logs provided earlier in the thread were from a previous test on `v0.9.2`, which was a high-power build running at 240MHz). 

Since v0.11.0 was tested and *did* fail, we must look at what in v0.11.0 could still be causing the Singapore AS11 to crash.

### 1.2 Root Cause Analysis
Based on the previous review and code analysis, there are two primary reasons v0.11.0 still fails for him:

1. **The `GMT_OFFSET` Bug (Schedule Misalignment):**
   The reporter's config uses `GMT_OFFSET = -6`. However, `v0.11.0`'s `Config.cpp` only parses `GMT_OFFSET_HOURS`. The `GMT_OFFSET` key is silently ignored, and the timezone defaults to UTC (0). 
   - **Result:** The intended 9:00–21:00 CST upload window is applied as 9:00–21:00 UTC (which is 3:00–15:00 CST). Therapy at 21:00 CST (03:00 UTC) falls **inside the firmware's active upload window**. 
   - The ESP32 waits for bus silence, then aggressively takes SD card control *during the night* while the CPAP is trying to use it.

2. **The DFS Hardcoding Bug (Power Transients):**
   Even if the schedule was correct, `v0.11.0` has a critical flaw in its DFS implementation. While `CPU_SPEED_MHZ` can be set to 80, `main.cpp` hardcodes the DFS ceiling to 160 MHz (`pm_config.max_freq_mhz = 160;`).
   - **Result:** Whenever the ESP32 performs a background task (like an NTP sync, mDNS response, or WiFi keepalive), the CPU instantly boosts from 80 to 160 MHz.
   - This creates a rapid `di/dt` current spike that the Singapore AS11's weak 3.3V supply cannot handle, causing a voltage sag that glitches the SD card slot.

**Status (v0.11.1):** ✅ All fixes implemented.
1. `GMT_OFFSET` is now accepted as a backward-compatible alias for `GMT_OFFSET_HOURS` in `Config.cpp`.
2. DFS hardcoding fixed — `CPU_SPEED_MHZ = 80` now truly locks the CPU at 80 MHz (no DFS boost).
3. Reporter should test v0.11.1 with `GMT_OFFSET_HOURS` and `CPU_SPEED_MHZ = 80`.

---

## 2. Issue #29: 6-Hour Offline Event

### 2.1 The Finding
A user reported that the ESP32 went offline at 10:30 during a scheduled upload and did not come back online until 16:45 (6+ hours later). The web system log showed the first record at 16:45, indicating a fresh boot at that time, meaning the device was **stuck/hung** for 6 hours without triggering a hardware watchdog reset.

### 2.2 Root Cause Analysis
This points to a severe networking stack corruption caused by the race condition identified in the previous review, combined with the FreeRTOS Task Watchdog configuration:
1. **The Watchdog Blind Spot:** In `main.cpp`, we call `esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0))` when starting the upload task to prevent the intensive TLS handshake from triggering a watchdog reset.
2. **The Race Condition:** If a CLOUD upload fails, the Core 0 upload task calls `tryCoordinatedWifiCycle()` to reset the connection. At the exact same time, the Core 1 main loop detects `!isConnected()` and calls `WiFi.begin()`.
3. **The Lockup:** Calling WiFi driver functions concurrently from two different cores without a mutex corrupts the lwIP/WiFi driver state machine. Because the Task Watchdog for Core 0 (where the WiFi task runs) is disabled, if the WiFi driver gets stuck in a deadlock or infinite loop, the ESP32 drops off the network but does **not** reboot. Core 1 continues to run its `loop()`, so the hardware watchdog is satisfied, but the device remains stranded offline.

**Status (v0.11.1):** ✅ Fixed. The main loop's WiFi reconnection is now guarded with `!uploadTaskRunning`, giving the upload task exclusive control over WiFi state recovery.

---

## 3. The Core Question: Why SMB works, but CLOUD fails

The central mystery is why a Singapore-made AS11 handles SMB uploads perfectly but crashes or drops WiFi during CLOUD uploads.

This comes down to the fundamental differences in resource and power profiles between the two protocols.

### 3.1 The "DFS Boost" Power Transient (The Smoking Gun)

In the preliminary power research (`docs/06-POWER-REDUCTION-PRELIMINARY-1.md`), the core recommendation was: *"locking the CPU core frequency to the 80 MHz floor eliminates the severe current spikes responsible for host failure."* 

However, in `v0.11.0`, Dynamic Frequency Scaling (DFS) was implemented with a hardcoded `max_freq_mhz = 160`. This violates the original design principle and creates a massive difference between SMB and CLOUD:

- **SMB Profile (Stable & Low Power):** SMB uses plaintext TCP. CPU usage is very low (just copying memory buffers to lwIP). Because the CPU is mostly idle waiting for SD I/O or network ACKs, the FreeRTOS IDLE task runs frequently. The DFS governor sees the idle time and keeps the CPU parked safely at **80 MHz**. Power draw remains low and flat.
- **CLOUD Profile (Spiky & High Power):** CLOUD uses TLS 1.2 (`WiFiClientSecure` / `mbedtls`). Every chunk of data sent must be encrypted using AES/ChaCha20. This requires intense mathematical computation, pinning the CPU to 100% active load. The DFS governor sees zero idle time and immediately **boosts the CPU clock from 80 MHz to 160 MHz** to process the encryption, then drops it back to 80 MHz when waiting for the network.
- **The Hardware Failure:** The sudden jump from 80 to 160 MHz creates a massive transient current spike (high `di/dt`). Weak power supplies (like the Singapore AS11) cannot handle this sudden change, causing a rapid voltage sag on the 3.3V line. This voltage sag causes the WiFi RF synthesizer to lose calibration (dropping the connection - Reason 8) or glitches the SD card.

**Fix:** We must allow users to truly lock the CPU at 80 MHz. We must change `pm_config.max_freq_mhz = 160;` to `pm_config.max_freq_mhz = config.getCpuSpeedMhz();`. This will allow `CPU_SPEED_MHZ = 80` to disable DFS boosting entirely, eliminating these transient spikes.

### 3.2 Contiguous Heap Exhaustion

- **SMB:** Allocates a fixed ~8KB buffer and has virtually zero protocol state overhead.
- **CLOUD:** Requires ~45KB of **contiguous** heap just to perform the TLS handshake, plus buffers. 
If the heap becomes fragmented during a long CLOUD session, lwIP may silently fail to allocate internal RX/TX buffers. When lwIP drops packets internally due to memory starvation, the TCP window collapses, leading to `Connection lost during write` and WiFi disconnects.

### 3.3 WiFi Power Saving (MIN_MODEM) Interaction

- `v0.11.0` removed `WiFi.setSleep(false)` during uploads to save power.
- During CLOUD uploads, the retry loops contain `delay(500)`. During this 500ms delay, the CPU is idle and the WiFi radio goes to sleep.
- Unifi APs with IoT profiles (UAPSD disabled) are known to forcefully disassociate sleeping clients during active high-bandwidth sessions if they don't respond to DTIM beacons fast enough. This explains the observed `Reason 8 (Assoc Leave)` disconnects.

---

## 4. Next Steps & Recommended Fixes (v0.11.1 Patch)

The following changes have been implemented in `v0.11.1`:

1. ✅ **Fix DFS Hardcoding (Critical Power Fix):** 
   `main.cpp` now uses the configured CPU speed as the DFS ceiling. Setting `CPU_SPEED_MHZ=80` truly locks the CPU to 80MHz, eliminating di/dt power transients. Auto light-sleep is also enabled for further power savings.

2. ✅ **Fix WiFi Race Condition (Critical Stability Fix):**
   The main loop's `connectStation()` call is now guarded with `!uploadTaskRunning`, preventing Core 1 from corrupting the WiFi driver state while Core 0 manages connection recovery.

3. ❌ **Restore Scoped `WiFi.setSleep(WIFI_PS_NONE)` for CLOUD — REJECTED:**
   This was rejected as it increases average power draw 5× (22 mA → 120 mA). AP compatibility with MIN_MODEM is an AP configuration issue (enable UAPSD), not a firmware problem.

4. ✅ **Additional v0.11.1 power optimizations:**
   - Default TX power reduced from 8.5 dBm to 5.0 dBm
   - ChaCha20/Poly1305 disabled in mbedtls (force HW-accelerated AES ciphers)
   - AES-256 disabled (force AES-128 — fewer rounds, less computation)
   - FreeRTOS tick rate reduced from 1000 Hz to 100 Hz
   - Tickless idle enabled for auto light-sleep support
   - GPIO 33 (CS_SENSE) wakeup configured for light-sleep
   - PM lock management added to FSM state transitions
