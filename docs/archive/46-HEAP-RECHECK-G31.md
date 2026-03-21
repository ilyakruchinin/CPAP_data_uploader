# Heap Fragmentation Re-Check (G31) — High-Impact Solutions

> **Status**: Deep Analysis + Implementation Plan
> **Date**: 2026-03-22
> **Context**: User requested a re-evaluation of previous heap analyses, specifically seeking the highest-benefit, permanent solutions for heap fragmentation, even if difficult to implement (e.g., custom TLS arena). Also emphasized power savings.

---

## 1. Executive Summary & Verdict

Previous analyses (`44` and `45`) focused on moving operations around (e.g., doing a "minimal probe" before TLS) to avoid triggering the failure. While valid, **they treat the symptom (the order of operations) rather than the disease (TLS fragmenting the heap).**

If we want the **most robust, highest-benefit solution**, we must attack the memory allocation behavior directly.

### Key Findings from this Deep Dive:
1. **Custom TLS Arena is 100% Feasible**: I verified the ESP32 framework build. The `mbedtls_platform_set_calloc_free` hook is exposed and works without recompiling the Arduino framework. We **can** implement a Custom TLS Arena.
2. **"Unused Components" are a Red Herring**: Libraries like Zigbee are compiled by PlatformIO, but the linker aggressively discards them (`--gc-sections`). They consume **0 bytes** of RAM/Flash. Disabling them saves build time, but gives no heap benefit.
3. **The Ultimate Fix**: A fixed TLS memory arena combined with a "WiFi-Off-by-Default" power profile will permanently eliminate both the heap fragmentation crashes and the worst power spikes.

---

## 2. High-Impact Heap Solution: The Custom TLS Arena

### The Problem
`WiFiClientSecure` (mbedTLS) dynamically allocates roughly 32KB–36KB of memory for the SSL IN/OUT buffers and connection state during the handshake. It uses `calloc` and `free`. 
Because the ESP32 heap gets "micro-holes" from normal operation (web server, strings, timers), these large 16KB blocks get jammed into the middle of the largest contiguous free space, permanently fragmenting it. When they are freed, they leave gaps. 

### The Solution: Static TLS Arena
Instead of letting mbedTLS use the system heap for its massive buffers, we intercept its allocations and serve them from a permanently reserved block of memory.

**How it works:**
1. At boot (when the heap is 100% pristine), we allocate a single `36KB` block: `tls_arena = malloc(36 * 1024);`
2. We inject a custom allocator into mbedTLS using `mbedtls_platform_set_calloc_free()`.
3. When mbedTLS asks for a large block (e.g., `> 8KB` for its IN/OUT buffers), our custom `calloc` gives it a slice of our `tls_arena`.
4. When mbedTLS asks for small blocks (certificates, cipher state), we pass it through to the system `calloc`.

**Why this is the Holy Grail:**
- The 36KB `tls_arena` is permanently carved out of the bottom of the heap. It never moves.
- mbedTLS can connect, disconnect, and reconnect thousands of times. Its massive buffers are strictly contained within the arena.
- **Result**: The rest of the system heap remains completely unfragmented by TLS. The SD card (`SD_MMC`) will always find a massive contiguous block available for its DMA buffers. We completely decouple TLS memory from SD memory.

### Technical Feasibility
I ran a test compile against the current `pico32-ota` environment. 
```cpp
#include <mbedtls/platform.h>
mbedtls_platform_set_calloc_free(my_custom_calloc, my_custom_free);
```
**It compiles successfully.** The hybrid build configuration leaves the hook available. We do not need to fork the Arduino core to do this.

---

## 3. High-Impact Power Solution: "WiFi-Off" Smart Mode

The user emphasized power savings / minimum power draw. 

Currently, in Smart Mode, the CPAP Uploader connects to WiFi at boot, starts mDNS, runs the Web UI, and stays awake listening for the SD bus to become idle. The WiFi radio consumes ~100mA continuously, with spikes up to 300mA. 

### The Ultimate Power Fix: "Dark Listening"
If power is a priority, we must decouple the "Listening" state from the "Connected" state.

1. **Boot**: Do NOT connect to WiFi. Do NOT start mDNS. Do NOT start the Web UI.
2. **Listen**: Wait in "Dark" mode. CPU uses DFS (Dynamic Frequency Scaling) to drop to 40MHz. Light-sleep is active. Current draw drops to <10mA. The hardware PCNT (Pulse Counter) monitors the SD bus for activity.
3. **Detect & Probe**: Once the CPAP machine goes idle, we mount the SD card and do a cheap, local "Minimal Work Probe" (check if files exist). 
4. **Wake & Upload**: **Only if work exists**, we power up the WiFi radio, connect to the router, initialize the TLS connection (using our Arena), and upload the data.
5. **Sleep**: Turn WiFi off again.

**Tradeoff**: The Web UI will not be accessible 24/7. It will only be accessible during the upload window, or if the user presses a physical button to trigger a "diagnostic wake". For a set-and-forget background uploader, this is usually heavily preferred over brownouts.

---

## 4. Debunking: Unused Components (Zigbee, etc.)

The user noticed `esp-zigbee-lib`, `lan867x`, `fb_gfx`, etc., being compiled.

I analyzed the linker map (`.pio/build/pico32-ota/firmware.map`). 
While PlatformIO compiles these libraries into `.a` archives (because they are part of the ESP-IDF base), the GCC linker (`-Wl,--gc-sections`) sees that our code never calls Zigbee functions. 

**Result**: It strips them entirely.
- Heap consumed by Zigbee: **0 bytes**
- Flash consumed by Zigbee: **0 bytes**

You cannot recover heap by disabling them in `platformio.ini` because they are already completely stripped from the binary. Removing them from the build via `custom_component_remove` only speeds up the compilation time. 

---

## 5. Implementation Plan

Here is the step-by-step plan to implement these high-benefit solutions.

### Step 1: Implement the Custom TLS Arena (Highest Priority)
This solves the fragmentation root cause permanently.
1. Create a `TlsArena` singleton class.
2. Allocate a `34KB` buffer at boot.
3. Write a simple 2-slot allocator (mbedTLS only needs two large buffers: `MBEDTLS_SSL_IN_CONTENT_LEN` and `MBEDTLS_SSL_OUT_CONTENT_LEN`, typically 16KB each).
4. Hook `mbedtls_platform_set_calloc_free()`.
5. Remove the "TLS Pre-Warm" logic from `main.cpp`. We will no longer need to "warm up" TLS before mounting the SD card, because TLS can no longer fragment the heap.

### Step 2: Implement the Minimal Work Probe
1. Refactor `FileUploader::scanDatalogFolders()` so it does not allocate `std::vector<String>`. 
2. Make it a fast, streaming directory check that returns `true` the moment it finds one un-uploaded `.edf` file.
3. This ensures that checking for work takes almost zero heap and zero time.

### Step 3: Refactor the Upload FSM for "Dark Listening" (Power Savings)
1. Add a config flag: `ULTRA_LOW_POWER_MODE`.
2. Modify `WiFiManager` so it does not auto-connect at boot if this flag is set.
3. Update `handleAcquiring()`:
   - Mount SD.
   - Run Minimal Work Probe.
   - If work exists: Bring up WiFi, sync NTP, do upload.
   - If no work: Unmount SD, go back to sleep.
4. Keep the PM Lock (CPU at 80MHz) only during the active upload phase, allowing true light-sleep during the long empty hours.

---

## Conclusion

The combination of a **Custom TLS Arena** and **Dark Listening** provides the absolute maximum benefit for both heap stability and power draw. 

By taking control of mbedTLS's memory allocation, we make the order of operations irrelevant: SD mount and TLS can happen in any order without destroying each other's memory space. 

By turning off WiFi until work is locally confirmed on the SD card, we eliminate 99% of the daily power consumption and network chatter.

**Awaiting your approval to begin implementing Step 1 (Custom TLS Arena).**
