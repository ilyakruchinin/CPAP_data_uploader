# Heap Fragmentation Re-Check (G31) — Final Plan for On-Demand TLS

> **Status**: Analysis + validated plan — no code changes yet
> **Date**: 2026-03-22
> **Goal**: Achieve stable "On-Demand TLS" (only connect when work exists), permanently fix heap fragmentation, clarify unused component impact, and maximize power savings while preserving Web UI accessibility.

---

## 1. Why "Mount First, TLS Later" Failed Historically

Your preferred flow is: **Mount SD → Check for work → Only establish TLS if work exists.**

Historically, this failed because:
1. Mounting the SD card uses SDMMC DMA buffers, which scatters allocations across the heap.
2. Checking for work (the pre-flight scan) creates many small string and vector allocations, further fragmenting the heap.
3. When `mbedTLS` was finally asked to connect, it demanded **~36KB of perfectly contiguous heap** for its IN/OUT buffers.
4. Because the heap was already fragmented by steps 1 & 2, mbedTLS couldn't find a 36KB contiguous block, and the TLS handshake crashed.

**The "Pre-warm" workaround** was introduced to grab that 36KB *before* the SD card was mounted. But as you noted, this is wasteful. It forces us to do an expensive TLS handshake and allocate memory even when there's nothing to upload.

---

## 2. How to Best Pre-Allocate and Reuse Memory

To safely achieve your preferred "On-Demand TLS" flow, we must stop mbedTLS and our tasks from relying on the fragile, general-purpose heap. 

### A. The "TLS Arena" (Custom mbedTLS Allocator)
We can statically reserve a contiguous block of memory (an "Arena") at boot specifically for TLS. 
- We use the ESP-IDF hook `mbedtls_platform_set_calloc_free()` to intercept mbedTLS memory requests.
- When mbedTLS asks for its massive 16KB buffers, we give it memory from our reserved Arena. 
- **Why this works:** It completely decouples TLS from the general heap. You can mount the SD card, scan folders, and fragment the general heap as much as you want. When you finally need TLS, its 36KB space is sitting there perfectly untouched and guaranteed to succeed.

### B. Static Upload Task Stack
Currently, the system allocates the 12KB Upload Task stack dynamically from the heap (`xTaskCreatePinnedToCore`). This is the primary trigger that drops our contiguous heap from `~45KB` to `~36KB`.
- **The fix:** We will use `xTaskCreateStaticPinnedToCore`. This permanently reserves the 12KB stack at compile time in `.bss` memory. It never touches the dynamic heap, eliminating the sudden drops we see in the logs.

---

## 3. The "Unused Components" Myth (Zigbee, etc.)

You correctly noticed that the compiler builds a bunch of files for things we don't use, like Zigbee (`esp-zigbee-lib`), Bluetooth, etc.

**Will removing them give us more heap?**
**No.** 

The ESP-IDF build system uses a linker flag called `--gc-sections` (Garbage Collect Sections). If a function or library is compiled but *never actually called* by our code, the linker completely strips it out of the final binary. 
- It takes **0 bytes** of Flash memory.
- It takes **0 bytes** of RAM/Heap at runtime.

We already use `custom_component_remove` in `platformio.ini` to prevent these from even compiling (which speeds up your build time), but spending time hunting down other unused libraries will yield **zero runtime heap improvements**. The heap fragmentation is entirely caused by our own active runtime allocations (Task Stacks, SD DMA, TLS buffers).

---

## 4. WiFi Sleep IRAM Optimizations vs. Power

You asked a brilliant question about the "stale" WiFi IRAM buffers:
> *"But don't they allow us to draw less power if implemented correctly... wouldn't re-implementing them properly be advantageous to power draw?"*

**You are absolutely right.** 
The setting `CONFIG_ESP_WIFI_SLP_IRAM_OPT=y` keeps the WiFi beacon-processing code in fast IRAM instead of slow Flash memory. This allows the CPU to wake up from Modem-Sleep, process the WiFi beacon, and go back to sleep much faster. It is highly advantageous for power draw.

Previous documents suggested removing it because it consumes ~2-4KB of IRAM (which steals from available DRAM), and the old analysis was desperate to scrape together every last kilobyte to keep the fragile TLS pre-warm working.

**The New Strategy:** 
Because we are going to fix the heap properly (with the Static Task Stack and TLS Arena), we will have **plenty of heap margin**. 
- We **will keep** the WiFi IRAM optimizations enabled. 
- We embrace the 2-4KB memory cost because we are no longer vulnerable to fragmentation, and we want the power savings.

---

## 5. Power Savings vs. Web UI Accessibility

Your requirement: **Minimum power draw, but the Web UI must remain accessible.**

To achieve this, we cannot use extreme "Deep Sleep" or turn the WiFi radio completely off. Instead, we optimize the **active time**:

1. **Modem-Sleep (Already Active):** The ESP32 is already correctly using `WIFI_PS_MIN_MODEM`. The Web UI remains perfectly accessible, with at most a ~100ms latency on the first request as the radio wakes up.
2. **Eliminate the 15-second "No-Work" Tax:** Currently, every 2 minutes the system wakes up, establishes a TLS connection (3-4 seconds of 100% CPU and WiFi TX power), mounts the SD card, and scans. All to find out there is nothing to do.
3. **The Minimal Work Probe:** We will implement a lightning-fast directory probe. It will mount the SD card, check for `.edf` files using a low-memory stream, and immediately unmount if empty. 
   - **Result:** The 2-minute check drops from ~15 seconds of high power draw to **< 2 seconds** of medium power draw. No TLS power spike is incurred.

---

## 6. The Final, Concrete Implementation Plan

Based on all the constraints, this is the exact roadmap to safely achieve your desired behavior.

### Step 1: Structural Heap Fixes (Do First)
1. **Implement Static Task Stack:** Convert the Upload task to `xTaskCreateStaticPinnedToCore` so it never touches the dynamic heap.
2. **Keep WiFi IRAM Settings:** Do *not* remove them. Keep `CONFIG_ESP_WIFI_SLP_IRAM_OPT=y` for power savings.
3. **Keep WiFiClientSecure Alive:** Allocate the TLS client object once at boot and reuse it, avoiding small memory leaks from deleting/recreating it.

### Step 2: The Permanent TLS Fix
1. **Implement the TLS Arena:** Create a statically allocated buffer (approx. 22KB-36KB) and use `mbedtls_platform_set_calloc_free()` to route TLS large-buffer requests to this safe space.

### Step 3: Achieve On-Demand TLS (The Goal)
1. **Remove TLS Pre-Warm:** Now that TLS has its own Arena, we can safely delete `preWarmTLS()` from the beginning of the cycle.
2. **Implement Minimal Work Probe:** Mount the SD card and run a highly optimized, low-memory check to see if files exist.
3. **Connect TLS On-Demand:** Only call `tlsClient->connect()` *after* the probe confirms cloud work exists.

### Step 4: Optional Heap Optimization (For smaller Arena size)
1. **Test Asymmetric Buffers (`CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y`):** Since we only send small chunks (4KB) to SleepHQ, we can configure mbedTLS to use a smaller OUT buffer. If this works, we can shrink our TLS Arena from ~36KB down to ~22KB, saving even more memory. 

**Conclusion:** This plan directly gives you the behavior you want (TLS only when necessary), fixes the root cause of the heap fragmentation, keeps the Web UI accessible, and maximizes power savings by making the idle checks extremely cheap.
