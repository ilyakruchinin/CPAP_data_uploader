# Deep Architectural Optimization Findings (G31)

## Scope & Objective

This document explores deeper, architectural-level optimization opportunities within the `CPAP_data_uploader` codebase to fundamentally minimize peak power draw and eliminate brownout risks. 

While `16-FURTHER-FINDINGS.md` reviewed the *current* state of the firmware's mitigations, this document identifies structural bottlenecks, hardware usage patterns, and logic flaws that still expose the system to high current (`di/dt`) transients.

---

## 1. The "Read-Twice" Penalty: Incomplete Single-Pass Cloud Uploads

### The Flaw
The codebase was previously updated to support sending the `content_hash` (MD5) in the multipart footer of the SleepHQ API request, which theoretically allows for "single-pass" streaming. However, `SleepHQUploader.cpp` still calls `computeContentHash()` *before* starting the upload. 

This means the firmware reads the **entire file** from the SD card to calculate the MD5, and then reads the **entire file again** to stream it over TLS. 

### The Power Impact
The SD card is kept active (drawing ~20-50mA) for **twice as long as necessary** during Cloud uploads. Because Cloud uploads are the most power-intensive phase, doubling the SD card active time drastically widens the window of vulnerability where a voltage sag can crash the system. 

### Architectural Fix
Fully implement true single-pass streaming. 
- Remove `computeContentHash()` pre-calculation.
- Compute the MD5 hash *on-the-fly* during the `file.read()` loop inside `httpMultipartUpload()`.
- Send the resulting hash hex string in the multipart footer.
- **Benefit**: Cuts SD card activity time by exactly 50% during Cloud uploads.

---

## 2. Over-provisioned SD Card I/O (1-bit vs. 4-bit SDIO & Clock Speed)

### The Flaw
In `SDCardManager.cpp`, the SD card is initialized with:
`SD_MMC.begin("/sdcard", SDIO_BIT_MODE_FAST)` 
Where `SDIO_BIT_MODE_FAST` is defined as `false`. In the ESP32 Arduino core, this second parameter is `mode1bit`. Passing `false` forces the SD card into **4-bit SDIO mode** at default high frequencies (usually 20MHz or 40MHz). 

### The Power Impact
4-bit SDIO drives four data lines simultaneously at high frequency, which demands significantly higher peak current from the 3.3V rail. Since the firmware's upload speed is fundamentally bottlenecked by WiFi and TLS encryption (~100-300 KB/s), running the SD card at multi-Megabyte/second speeds is vastly over-provisioned and needlessly stressful on the power supply.

### Architectural Fix
- Force **1-bit SDIO mode** (`mode1bit = true`).
- Explicitly underclock the SDIO frequency via the `begin()` parameters (e.g., clamp `sdmmc_frequency` to 10MHz or 5MHz).
- **Benefit**: Smooths out `di/dt` spikes from SD card reads without slowing down the actual upload, directly protecting the shared 3.3V rail.

---

## 3. The Overlap Problem: Decoupling SD I/O and WiFi TX

### The Flaw
Within the upload loop (`httpMultipartUpload`), the firmware reads a chunk from the SD card (`file.read(buffer, toRead)`) and immediately writes it to the TLS socket (`tlsClient->write`). 

Because WiFi TX on the ESP32 is handled asynchronously by the MAC layer (lwIP), the physical WiFi radio bursts RF energy *at the exact same time* the CPU is clocking the SD card to fetch the next chunk. 

### The Power Impact
Simultaneous SD Card Read (~40mA) + CPU at 160MHz for TLS (~40mA) + WiFi TX Burst (~250mA) creates the absolute highest possible peak current "stack" in the system. 

### Architectural Fix
Introduce strict pacing or a "ping-pong" isolation model:
- Read a larger chunk from the SD card into RAM.
- Pause or briefly yield SD operations.
- Flush the buffer to the TLS socket and yield to let the MAC layer complete the TX burst.
- Ensure the CPU is not aggressively driving the SD bus while the WiFi radio is actively transmitting. 

---

## 4. `LISTENING` State Power Drain: The ULP Coprocessor Opportunity

### The Flaw
To detect bus silence, the FSM enters the `LISTENING` state. In this state, `TrafficMonitor` reads the PCNT (Pulse Counter) hardware to track `CS_SENSE` activity. Because the CPU must poll this counter and evaluate the elapsed time, it holds an `esp_pm_lock` (`fsm_active`), forcing the CPU to stay awake at 80MHz. 
In `smart` mode, the device spends >90% of the day in this state.

### The Power Impact
This completely negates the benefit of `CONFIG_PM_ENABLE` during the day. The CPU cannot enter light-sleep, keeping baseline current draw at ~20-30mA instead of dropping to <2mA.

### Architectural Fix
Offload `CS_SENSE` monitoring to the ESP32's **ULP (Ultra Low Power) Coprocessor**.
- The main CPU can release the PM lock and enter deep sleep or true light sleep.
- The ULP runs a tiny assembly/C program that counts pulses and tracks time. 
- When the ULP detects absolute silence for `INACTIVITY_SECONDS`, it wakes the main CPU.
- **Benefit**: Achieves massive daily energy savings, drastically reducing thermal load and long-term stress on the CPAP power supply.

---

## 5. TCP Window & Burst Pacing (lwIP Tuning)

### The Flaw
The ESP32 uses standard lwIP TCP configurations, which attempt to send packets as fast as the TCP window allows. During TLS uploads, this results in dense, back-to-back WiFi TX bursts. 

### The Power Impact
Dense RF bursts prevent the internal capacitors from recovering, driving the 3.3V rail voltage down progressively (voltage sag) until a brownout occurs.

### Architectural Fix
Tune lwIP at the framework/build level to pace packets.
- Reduce `TCP_WND` (TCP Window) and `TCP_MSS` (Maximum Segment Size).
- This forces the ESP32 to wait for TCP ACKs more frequently, naturally spacing out the WiFi TX bursts.
- **Benefit**: Lowers the *average* current over any given millisecond window, giving the power rail time to physically recover between RF bursts.

---

## 6. `WIFI_PS_MAX_MODEM` Needs `listen_interval` Enforcement

### The Flaw
As noted in prior findings, the firmware exposes `SAVE_MAX` which invokes `WIFI_PS_MAX_MODEM`. However, the ESP-IDF documentation states that `MAX_MODEM` requires explicitly setting the `listen_interval` in the `wifi_config_t` station struct. Without it, the ESP32 defaults to waking up on every AP DTIM beacon (usually every 100-300ms).

### Architectural Fix
- When the user selects `SAVE_MAX`, update the station configuration using `esp_wifi_set_config()` and set `.sta.listen_interval` to a high value (e.g., 10 or 20). 
- **Benefit**: Forces the radio to stay off for 1-3 seconds at a time during `IDLE` or `COOLDOWN`, deeply cutting idle power.

---

## 7. Web Server Lockdown During Uploads

### The Flaw
The synchronous web server (`handleClient`) remains fully active during the `UPLOADING` state. If a user leaves a browser tab open, it will relentlessly poll `/api/status` or stream `/api/logs` via SSE. 

### The Power Impact
Servicing web requests forces context switches, allocates memory, and triggers additional WiFi RX/TX cycles right in the middle of a delicate TLS Cloud upload. This acts as a chaotic multiplier for peak current.

### Architectural Fix
Implement a strict "Upload Mode Lockdown". 
- When `currentPhase == UPLOADING`, the web server should aggressively drop (or dramatically simplify) all non-essential API requests.
- Close the SSE log stream dynamically when an upload begins, forcing the browser to back off.
- **Benefit**: Eliminates unpredictable network and CPU load spikes during the most vulnerable hardware phase.

---

## Summary of Priority Architecture Shifts

If minimizing brownouts is the ultimate goal, the following architectural changes offer the highest impact:

1. **Move to 1-bit, low-frequency SDIO mode** (Immediate power reduction on the SD bus).
2. **Implement true Single-Pass Cloud Uploads** (Cuts SD card active time and TLS overlap by 50%).
3. **Decouple SD Reads from WiFi TX** (Prevents the simultaneous power stack of SD + CPU + RF).
4. **Offload bus monitoring to the ULP** (Allows true CPU sleep during the 16-hour `LISTENING` phase).
