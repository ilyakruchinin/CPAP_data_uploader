# v0.11.0 Post-Release Feedback Review

Date: 2026-03-01
Firmware: v0.11.0 (Power Optimization Release)
Sources: GitHub Issues [#29](https://github.com/amanuense/CPAP_data_uploader/issues/29), [#42](https://github.com/amanuense/CPAP_data_uploader/issues/42)

---

## 1. User Reports Summary

| Reporter | Hardware | Config | Outcome | Key Symptom |
|:---------|:---------|:-------|:--------|:------------|
| **User A** | Singapore AS11 | SMB → SMB+Cloud | ✅ **Resolved** | No SD Card Error after 16+ hours; SMB upload successful; Cloud test pending |
| **User B** | Singapore AS11 | SMB+Cloud, Smart mode | ⚠️ **Mixed** | SMB uploads fine; Cloud uploads fail on large files with WiFi disconnects; web UI presence exacerbates; uploads succeed when browser is closed |
| **User C** | Singapore AS11 | SMB only, Scheduled mode | ❌ **Still failing** | No error on insertion; 16+ hours OK idle; SD Card Error appears during overnight therapy; v0.5.1-pre-pr2 still the only working version |

---

## 2. User B: WiFi Disconnects During Cloud Uploads

### 2.1 Observed Pattern

Large BRP files (~1–2 MB) consistently fail mid-upload with `Connection lost during write`:

| File | Size | Bytes sent before failure |
|:-----|:-----|:-------------------------|
| `20260226_235156_BRP.edf` | ~2 MB | unknown (first attempt) |
| `20260226_021042_BRP.edf` | 1,951,674 | 258,048 (13%) |
| `20260225_040352_BRP.edf` | 1,021,364 | 151,552 (15%) |

Small files (CSL ~832 B, EVE ~1–2 KB, PLD ~80–180 KB, SA2 ~35–80 KB) upload successfully. Failures occur ~60 seconds into the stream of a large file.

### 2.2 Root Cause Analysis

#### A. WiFi Reconnection Race Condition (Core 0 vs Core 1) — **HIGH confidence**

The logs show a clear race between the upload task (Core 0) and the main loop (Core 1):

```
[13:10:09] [INFO] [NetRecovery] WiFi cycle complete — reconnected successfully
[13:10:09] [WARN] WiFi Event: Disconnected from AP (reason: 8)        ← immediate re-disconnect!
[13:10:09] [INFO] Connecting to WiFi: MyNetwork.IoT                    ← main loop reconnection
```

**What happens:**
1. Upload task on Core 0 detects a write failure
2. Core 0 calls `tryCoordinatedWifiCycle()` → `WiFi.disconnect()` → `WiFi.reconnect()` → succeeds
3. Meanwhile, Core 1 main loop sees `!wifiManager.isConnected()` and calls `connectStation()`
4. `connectStation()` calls `WiFi.mode(WIFI_STA)` + `esp_wifi_set_protocol()` + `WiFi.begin()` — this **tears down** the connection Core 0 just established
5. Reason 8 (WIFI_REASON_ASSOC_LEAVE) fires — this is the ESP32 voluntarily leaving the BSS because `WiFi.begin()` restarts the association

**Evidence:** Every `NetRecovery WiFi cycle complete` is immediately followed by another disconnect (reason 8) and a `Connecting to WiFi` from the main loop. The two cores are fighting over WiFi state.

**Fix:** Guard the main loop reconnection path so it does NOT call `connectStation()` while `g_wifiCyclingActive` is true, or while the upload task is running. The simplest approach: skip main-loop WiFi reconnection entirely when `uploadTaskRunning` is true — the upload task already handles its own WiFi recovery.

#### B. WiFi Power Saving During TLS Streaming — **MEDIUM confidence**

v0.11.0 removed `WiFi.setSleep(false)` during Cloud uploads (the "power leak fix"). The rationale was that ESP-IDF's WiFi driver automatically holds a PM lock during active TX/RX. However, there is a gap:

- During the `delay(500)` in the write retry loop (`SleepHQUploader.cpp:1122`), no TX/RX is happening
- The WiFi driver releases its PM lock during this idle period
- MIN_MODEM kicks in, radio sleeps
- On the reporter's Unifi AP with **UAPSD off**, the AP may not properly buffer frames for sleeping clients
- If the AP's client inactivity timeout is short, it may disassociate the ESP32

This explains why large files fail more often: they have more opportunities for the radio to sleep between write retries, and a single dropped packet can cascade into `Connection lost during write`.

**Fix options:**
1. **Targeted**: Temporarily disable power saving during the Cloud upload streaming phase only, and restore it after — essentially re-introduce `WiFi.setSleep(false)` but with proper cleanup
2. **Minimal**: Replace `delay(500)` in write retries with a busy-wait that feeds a dummy WiFi operation to keep the PM lock held

#### C. Web Server Contention — **MEDIUM confidence**

User explicitly noted: *"upload to SleepHQ took more than 10 unsuccessful attempts... I left it alone for a while and then it went fine"* and *"according to logs right after I closed browser, without a single reboot"*.

When the browser is open:
- SPA polls `/api/status` every few seconds (status snapshot refresh)
- Log tab polls `/api/logs` — during uploads, this returns truncated tail (`printLogsTail`)
- Dashboard auto-refresh may be hitting multiple endpoints
- Each HTTP response requires WiFi TX, competing with TLS upload for:
  - lwIP socket buffer space
  - WiFi TX queue
  - Radio airtime

The web server runs on Core 1 (main loop) via `handleClient()`, while the TLS upload runs on Core 0. Both write to the same WiFi TX queue. Under MIN_MODEM with DTIM-wake, the radio availability is already constrained, and contention makes it worse.

**Fix options:**
1. Throttle or pause web server status polling when an upload is active (e.g., return 503/Retry-After for non-essential endpoints)
2. Increase the status snapshot interval from 3 seconds to 10+ seconds during uploads
3. Disable chunked log streaming during uploads (already partially done via throttle, but the response itself still uses WiFi TX)

#### D. Unifi AP IoT Profile Interaction — **LOW-MEDIUM confidence**

The reporter's AP settings:
- **UAPSD off** — the AP doesn't support U-APSD, which is the efficient power-save delivery mechanism
- **Broadcast off / Multicast off** — AP filters broadcasts/multicasts, meaning DTIM beacon delivery may be minimal
- **Roaming off** — no impact

With UAPSD disabled, MIN_MODEM relies solely on legacy DTIM-based frame delivery. If the AP has an aggressive client timeout for power-save stations, it may disassociate clients that don't respond promptly after waking.

**Diagnostic suggestions (in order of preference):**
1. **Enable UAPSD on the Unifi AP** — This is the proper fix on the network side. U-APSD (Unscheduled Automatic Power Save Delivery) is the standard mechanism that makes WiFi power saving work correctly. With UAPSD enabled, the AP can deliver buffered frames to the ESP32 on demand rather than waiting for DTIM intervals, significantly reducing the chance of disassociation. This can be enabled per-SSID in the Unifi controller under WiFi settings → Advanced.
2. **Set `WIFI_PWR_SAVING = NONE`** in config.txt — disables MIN_MODEM entirely, keeping the radio always on. This eliminates the interaction but at the cost of higher power draw (~80–120 mA continuous vs ~15–30 mA average with MIN_MODEM).

### 2.3 Recommended Fixes (Priority Order)

1. **Guard main-loop WiFi reconnection during uploads** — prevents the Core 0/Core 1 race condition
2. **Temporarily disable WiFi power saving during active Cloud upload streaming** — prevents radio sleep gaps during TLS writes
3. **Throttle web server during active uploads** — reduce WiFi TX contention

---

## 3. User C: SD Card Error During Therapy

### 3.1 Observed Pattern

- v0.11.0 inserted → 16+ hours OK while CPAP is idle (outside therapy)
- Therapy starts after 21:00 (outside upload window, device in FSM `IDLE` state)
- SD Card Error discovered in the morning — user reported no therapy data for the night
- v0.5.1-pre-pr2 (original firmware) is the only version that works

**Important timing note:** The user did not confirm whether the SD Card Error was already present *before* starting therapy. Two scenarios are possible:

1. **Error during therapy** — The ESP32 interfered with the SD bus while the CPAP was actively writing EDF data during overnight therapy.
2. **Error before therapy** — The ESP32 caused the error earlier (e.g., during the upload window when it took SD card control), but the user did not check. The CPAP would have displayed "SD Card Error" from that point on and would not have started recording therapy data at all. The absence of therapy data for the night is consistent with *both* scenarios.

This distinction matters because scenario 2 points to the upload window / SD card acquisition logic rather than passive electrical interference during IDLE state.

### 3.2 Comparison: v0.5.1-pre-pr2 vs v0.11.0

Since no logs were provided, the analysis is based on code comparison of both versions.

| Aspect | v0.5.1-pre-pr2 | v0.11.0 |
|:-------|:---------------|:--------|
| **CPU frequency** | Fixed 240 MHz | 80 MHz base + DFS (80↔160 MHz) |
| **DFS** | None | `esp_pm_configure()` active, CPU scales dynamically |
| **WiFi power save** | None (radio always on) | MIN_MODEM (radio sleeps between DTIMs) |
| **WiFi TX power** | Default 19.5 dBm | 8.5 dBm |
| **802.11b** | Enabled | Disabled (OFDM only) |
| **Bluetooth** | Compiled in (unused) | Disabled at compile time, memory released |
| **CS_SENSE pin** | GPIO 32, `INPUT_PULLUP` (incorrect per schematic) | **GPIO 33**, `INPUT` (correct, verified from schematic) |
| **Bus monitoring** | Simple `digitalRead()` before `takeControl()` only | PCNT hardware counter on CS_SENSE (correct approach) |
| **Loop behavior in IDLE** | Tight loop, 60 s check interval, no explicit yields | `vTaskDelay(100 ms)` yields, allows DFS scaling |
| **Boot delay** | Fixed 30 s | 15 s + Smart Wait (5 s bus silence) |
| **MUX switch delay** | 100 ms | 300 ms |
| **FreeRTOS tick rate** | Default (100 Hz) | 1000 Hz (`CONFIG_FREERTOS_HZ=1000`) |
| **Config format** | JSON (`config.json`) | Key-value (`config.txt`) |
| **Upload architecture** | Single-threaded, blocking | Dual-core, async upload task on Core 0 |

### 3.3 Most Probable Causes

#### A. DFS Frequency Transitions Causing Power Rail Disturbances — **HIGH probability**

When DFS scales the CPU between 80 MHz and 160 MHz, the PLL relocks, causing:
- Transient current spikes (~10–30 mA for ~1 µs)
- EMI profile changes (different clock harmonics on board traces)
- Brief voltage dips on the shared power rail

On a compact SD-WiFi-PRO board, the ESP32 and SD card slot share the same power supply from the AirSense 11. The AirSense 11's SD card slot is apparently very sensitive to electrical disturbances (the whole basis for the Singapore-made AS11 issues).

**Why therapy triggers it:** During therapy, the AirSense 11 actively writes EDF data to the SD card every few seconds. Active SDIO transactions are more sensitive to power rail disturbances than an idle bus. Meanwhile, the ESP32's DFS is making CPU frequency transitions triggered by WiFi keepalive packets, mDNS responses, NTP retries, etc.

v0.5.1-pre-pr2 runs at a fixed 240 MHz — **no frequency transitions**, no PLL relocks, stable EMI profile. Higher steady-state power draw but no transients.

**Note:** The `CPU_SPEED_MHZ` config key does **not** control DFS. DFS is hardcoded in `main.cpp:400-404` to scale between 80–160 MHz regardless of the configured CPU speed. To disable DFS, a **firmware change** is needed (see §3.5).

**Ideal diagnostic:** Pin the CPU at a constant 80 MHz (lowest power, no transitions). This requires a firmware change to set `esp_pm_configure()` with `max_freq_mhz = min_freq_mhz = 80`. This eliminates DFS transitions while maintaining the lowest possible power draw — critical for the power-sensitive AS11 SD card slot.

#### B. WiFi MIN_MODEM Radio Wake/Sleep Cycles — **MEDIUM probability**

With MIN_MODEM, the WiFi radio alternates between sleep and wake states every DTIM interval (~100–300 ms depending on AP configuration). Each wake/sleep transition involves:
- Radio power-up: ~120 mA spike for ~1 ms
- Radio power-down: current drops sharply

These periodic current transients happen continuously, even during IDLE when no uploads are occurring. They could couple into the SD bus via shared power rails or PCB trace proximity.

v0.5.1-pre-pr2 keeps the radio always on — steady ~120 mA draw with no transitions.

**Diagnostic:** Test with `WIFI_PWR_SAVING = NONE` in config.txt. This is already user-configurable and doesn't require a firmware change.

#### C. CS_SENSE Pin Difference — **Not a concern** (noted for completeness)

The CS_SENSE pin changed from **GPIO 32** (`INPUT_PULLUP`) in v0.5.1-pre-pr2 to **GPIO 33** (`INPUT`, no pull-up) in v0.11.0. GPIO 33 + PCNT is the correct configuration, verified from the board schematic. The v0.5.1-pre-pr2 assignment to GPIO 32 was the inaccurate one.

CS_SENSE is on the CPAP side of the bus switch and is a passive monitoring pin — it cannot affect CPAP↔SD communication regardless of its GPIO configuration. This difference does not contribute to the SD Card Error.

#### D. Higher FreeRTOS Tick Rate — **LOW probability**

v0.11.0 sets `CONFIG_FREERTOS_HZ=1000` (1 ms tick) vs the default 100 Hz (10 ms tick) in v0.5.1-pre-pr2. The 1000 Hz tick was chosen because the ESP-IDF DFS documentation recommends it for finer idle-detection granularity. However, this is a double-edged sword: more frequent IDLE task runs → more frequent DFS evaluations → more frequent frequency transitions → more PLL relocks.

**Impact comparison across tick rates:**

| Tick Rate | Tick Period | Timer ISRs/sec (per core) | ISR CPU overhead | `vTaskDelay(1)` resolution | DFS evaluation frequency |
|:----------|:-----------|:--------------------------|:-----------------|:---------------------------|:-------------------------|
| **1000 Hz** (current) | 1 ms | 1000 | ~0.3–0.5% | 1 ms | Very frequent — maximizes transitions |
| **500 Hz** | 2 ms | 500 | ~0.15–0.25% | 2 ms | Moderate — halves transition opportunities |
| **200 Hz** | 5 ms | 200 | ~0.06–0.1% | 5 ms | Low — significantly fewer transitions |
| **100 Hz** (v0.5.1-pre-pr2) | 10 ms | 100 | ~0.03–0.05% | 10 ms | Minimal — matches original behavior |

**Practical impact of reducing tick rate:**
- **TrafficMonitor**: Uses PCNT hardware counter (not FreeRTOS timers). The 100 ms sampling in `update()` is called from the main loop, not a tick-driven callback. Even at 100 Hz, 100 ms = 10 ticks — more than adequate.
- **Upload task**: TLS write/read operations and SD I/O don't need sub-10 ms scheduling precision.
- **WiFi driver**: Uses its own hardware timers internally, completely independent of FreeRTOS tick rate.
- **vTaskDelay accuracy**: At 200 Hz, the `vTaskDelay(100 ms)` in the IDLE state loop becomes `vTaskDelay(20 ticks)` — still perfectly precise. The 50 ms delay in LISTENING becomes `vTaskDelay(10 ticks)` — also fine.
- **DFS transitions**: Each timer ISR wakes the CPU (if it was frequency-scaled down), evaluates whether a PM lock is held, and potentially triggers a frequency change. Reducing from 1000 to 200 Hz reduces the number of potential DFS transition points by 80%.

**Recommendation:** Reduce `CONFIG_FREERTOS_HZ` to **200 Hz**. This provides adequate scheduling precision for all firmware operations while reducing timer ISR overhead by 80% and — critically — reducing the frequency of DFS evaluation points. If DFS is confirmed as a contributor to SD card errors, a lower tick rate reduces the rate of power rail disturbances even without disabling DFS entirely. The 100 Hz default is also viable but 200 Hz provides a comfortable margin for any future sub-10 ms timing needs.

#### E. PCNT Hardware on CS_SENSE — **LOW probability**

v0.11.0 configures a hardware PCNT (Pulse Counter) unit on CS_SENSE with:
- Edge counting on both rising and falling edges
- 10-cycle glitch filter enabled
- Counter running continuously

The PCNT is a purely passive input peripheral — it should not drive any signal. However, it's a new hardware peripheral that v0.5.1-pre-pr2 doesn't use. The PCNT's `update()` is only called in LISTENING/MONITORING states, but the hardware counter itself runs continuously.

### 3.4 Recommended Diagnostic Steps

**Step 0 — Fix the config key (mandatory first):**
```ini
GMT_OFFSET_HOURS = -6
```
Change `GMT_OFFSET` to `GMT_OFFSET_HOURS`. This is almost certainly the primary cause (see §4.3). All subsequent tests are only meaningful after this is fixed.

If the SD Card Error persists after fixing the config key, isolate which power management change causes the regression. Note that DFS cannot currently be disabled via config — it requires a firmware change (see §3.5).

1. **Test 1 — Disable WiFi power saving (user-configurable):**
   ```ini
   WIFI_PWR_SAVING = NONE
   ```
   If this resolves → MIN_MODEM radio wake/sleep cycles are the trigger.

2. **Test 2 — Enable SAVE_LOGS for post-mortem:**
   ```ini
   SAVE_LOGS = true
   ```
   This persists logs to internal flash, allowing post-mortem analysis of what the ESP32 was doing when the SD Card Error occurred. Especially useful to confirm whether the FSM was in IDLE vs LISTENING at the time of failure.

3. **Test 3 — Firmware-side: disable DFS (requires v0.11.1 patch):**
   Pin CPU at constant 80 MHz by setting `esp_pm_configure()` with `max_freq_mhz = min_freq_mhz = 80`. This eliminates frequency transitions while maintaining the lowest possible steady-state power draw. See §3.5 for implementation options.

### 3.5 Potential Firmware Improvements

**DFS control (regardless of whether DFS is confirmed as the trigger):**
- **Add a `DISABLE_DFS` config option**: When set, call `esp_pm_configure()` with `max_freq_mhz = min_freq_mhz = 80` at boot. This pins the CPU at the lowest supported frequency (80 MHz — the WiFi PHY minimum), eliminating all frequency transitions while maintaining the lowest possible steady-state power draw. This is strictly better than reverting to 240 MHz fixed from a power perspective.
- **State-aware DFS**: Alternatively, disable DFS (pin at 80 MHz) when entering IDLE or COOLDOWN states, and re-enable it (80↔160 MHz) when entering LISTENING. This gives the best of both worlds: zero transitions during therapy, full DFS benefit during uploads.

**FreeRTOS tick rate:**
- **Reduce `CONFIG_FREERTOS_HZ` from 1000 to 200**: Reduces timer ISR overhead by 80% and — more importantly — reduces the frequency of DFS evaluation points. All firmware operations (TrafficMonitor, upload task, web server) work correctly at 200 Hz. See §3.3D for detailed analysis.

**WiFi power saving:**
- **Disable power saving during IDLE for scheduled mode**: Since scheduled mode has a clear "not uploading" period, we could keep WiFi at full power during that time (the power savings are less critical when the device isn't actively doing work).
- **Add `WIFI_PWR_SAVING = NONE` recommendation for AS11 users who still see issues**.

---

## 4. Cross-Cutting Observations

### 4.1 WiFi.setSleep(false) Removal — Revisit Needed

The v0.11.0 removal of `WiFi.setSleep(false)` from `SleepHQUploader.cpp` was correct in principle (the ESP-IDF PM lock mechanism should handle this). However, in practice:
- The PM lock is only held during active `tlsClient->write()` / `tlsClient->read()` calls
- Between write retries (500 ms delay), between file uploads, and during response waits, the radio can sleep
- This creates gaps where the AP may decide to disassociate a sleeping client

**Recommendation:** Re-introduce `WiFi.setSleep(WIFI_PS_NONE)` at the START of a Cloud upload session and restore the configured power saving at the END. This is a surgical, scoped fix — not a leak like the original.

### 4.2 Intentional Reboots — Communication Gap

A user was alarmed by the intentional reboots between backend cycling (SMB → reboot → Cloud). While the reboots themselves are expected behavior, the WiFi disconnects and failed Cloud uploads made the overall experience look broken. Better logging and web UI messaging would help:
- Log: `[FSM] Upload session complete for SMB — soft-rebooting to switch to Cloud backend`
- Web UI: Show "Rebooting to switch upload backend..." status message

### 4.3 Config Key Compatibility

The reporter's v0.11.0 config uses `GMT_OFFSET = -6`. **CONFIRMED: `GMT_OFFSET` is NOT a recognized key.** Only `GMT_OFFSET_HOURS` is parsed (verified in `Config.cpp:191`). The key is silently ignored and the timezone defaults to UTC (0).

**This is potentially the PRIMARY cause of the SD Card Error.** The upload window miscalculation:
- User intends window: 9:00–21:00 CST (UTC-6)
- Firmware applies: 9:00–21:00 **UTC** = 3:00–15:00 CST
- Therapy starts at ~21:00 CST = 03:00 UTC — **inside the firmware's upload window**
- The ESP32 is in **LISTENING** state, not IDLE
- After detecting 300 seconds (5 min) of bus silence (the reporter's `INACTIVITY_SECONDS = 300`), the FSM transitions to ACQUIRING and **takes SD card control during therapy**
- The CPAP is writing EDF data but the ESP32 snatches the SD card → **SD Card Error**

This also explains:
- **Why 16+ hours of idle was fine**: Before 03:00 CST (09:00 UTC), the firmware was in IDLE state — the upload window hadn't opened yet (from the firmware's perspective). The ESP32 never attempted SD card access.
- **Why v0.5.1-pre-pr2 works**: It uses a different config key (`"GMT_OFFSET_HOURS"` in JSON format) AND has a different upload scheduling model (simple time check, no FSM with aggressive bus-silence detection)
- **Why the error appeared between 03:00 and 07:00**: 03:00 CST = 09:00 UTC — exactly when the upload window opens. The ESP32 enters LISTENING, waits for 300 s of bus silence, then takes SD card control. If the CPAP happened to pause writes for 5 minutes during therapy, the ESP32 would grab the card.
- **Alternative timeline**: The SD Card Error may have appeared at 03:00 CST (09:00 UTC) when the ESP32 first took card control, but the user only discovered it in the morning. Since the CPAP stops recording once an SD Card Error is detected, there would be no therapy data — which matches the user's report.

**Status (v0.11.1):** ✅ Fixed. `GMT_OFFSET` is now accepted as a backward-compatible alias for `GMT_OFFSET_HOURS` in `Config.cpp`. Both key names work.

---

## 5. Summary of Action Items

### Implemented in v0.11.1

| # | Item | Status |
|:--|:-----|:-------|
| 1 | **Accept `GMT_OFFSET` as alias for `GMT_OFFSET_HOURS` in Config.cpp** | ✅ Done |
| 2 | Guard main-loop WiFi reconnection when `uploadTaskRunning` | ✅ Done |
| 3 | Re-introduce scoped `WiFi.setSleep(WIFI_PS_NONE)` for Cloud uploads | ❌ Rejected (5× power increase) |
| 4 | DFS respects `CPU_SPEED_MHZ` — 80 MHz default locks CPU (no DFS transitions) | ✅ Done |
| 5 | Reduce `CONFIG_FREERTOS_HZ` from 1000 to 100 | ✅ Done |
| 6 | Default TX power reduced from 8.5 dBm to 5.0 dBm | ✅ Done |
| 7 | ChaCha20/AES-256 disabled in mbedtls (force HW AES-128-GCM) | ✅ Done |
| 8 | Auto light-sleep enabled with GPIO 33 wakeup + PM lock management | ✅ Done |
| 9 | Tickless idle enabled for light-sleep support | ✅ Done |
| 10 | BT memory release diagnostic logging added | ✅ Done |

### Diagnostic (need user testing)

| # | Item |
|:--|:-----|
| 1 | Verify `GMT_OFFSET_HOURS` is set correctly in config.txt |
| 2 | If SD Card Error persists, test with `WIFI_PWR_SAVING = NONE` |
| 3 | Enable `SAVE_LOGS = true` for post-mortem analysis |
| 4 | For WiFi disconnect issues, **enable UAPSD on AP** (preferred) or test `WIFI_PWR_SAVING = NONE` |

### Future (v0.12.x)

| # | Item |
|:--|:-----|
| 1 | Throttle web server status endpoints during active uploads |
| 2 | Improve reboot messaging in logs and web UI |
| 3 | Remove backend-cycling reboots (heap is now stable enough) |
