# Further Findings (CO46): Hardware-Level Deep Dive, Service Lifecycle, and Sub-80MHz Feasibility

## Scope

This document is a **corrected, expanded, and hardware-focused follow-up** to:

- `docs/16-FURTHER-FINDINGS.md`
- `docs/17-FURTHER-FINDINGS-G31.MD`
- `docs/18-FURTHER-FINDINGS-C54.md`

It specifically investigates:

1. Correctness of all three prior documents
2. SD card 1-bit mode at 5 MHz — exact feasibility and implementation path
3. CPU frequency below 80 MHz — what the real constraint is and when it can be bypassed
4. ESP32 hardware accelerators already in use and what remains untapped
5. ULP (Ultra Low Power) coprocessor for CS_SENSE monitoring — detailed feasibility
6. mDNS "respond once then unload" pattern
7. OTA manager on-demand via GUI button instead of always-initialized
8. Every other brownout-minimization opportunity found in the fresh codebase review

This is **analysis only**. No code changes are proposed here.

---

## Part 1: Correctness Check of Prior Documents

### docs/16-FURTHER-FINDINGS.md

Overall: **solid and accurate**, with one factual error.

**Error**: Section 7 states `scripts/rebuild_mbedtls.py` is missing from the workspace. This is **incorrect**. The file exists at `scripts/rebuild_mbedtls.py` and is a fully functional 388-line PlatformIO pre-build script that downloads mbedTLS 2.28.7 source, cross-compiles the TLS library objects with the custom defines from `sdkconfig.defaults`, and replaces the framework's pre-compiled `libmbedtls_2.a`. This means the TLS buffer optimizations (asymmetric 16KB IN / 4KB OUT, variable buffer length, disabled session tickets, etc.) **are** in effect in production builds.

**Consequence of the error**: docs/16 Section 7 incorrectly warns that "CPU cost during TLS may be higher than assumed" and "Cloud upload could be more stressful than the docs imply". Those warnings should be retracted — the custom mbedTLS build is working as intended.

Everything else in docs/16 remains correct.

### docs/17-FURTHER-FINDINGS-G31.MD

Overall: **directionally useful but too absolute in several places**, as docs/18 already noted.

Additional correction needed beyond what docs/18 caught:

**Section 2 (SDIO frequency)**: docs/17 said frequency reduction would require "lower-level SDMMC host configuration". This is **wrong**. The Arduino ESP32 `SD_MMC.begin()` API already accepts a frequency parameter as its 4th argument:

```cpp
bool begin(const char* mountpoint="/sdcard", bool mode1bit=false,
           bool format_if_mount_failed=false,
           int sdmmc_frequency=BOARD_MAX_SDMMC_FREQ, uint8_t maxOpenFiles=5);
```

Where `BOARD_MAX_SDMMC_FREQ` defaults to `SDMMC_FREQ_HIGHSPEED` (40 MHz). Switching to 1-bit at 5 MHz is a **single-line change**, not an architectural undertaking.

### docs/18-FURTHER-FINDINGS-C54.md

Overall: **the most carefully qualified of the three**, but inherited two inaccuracies from docs/16 and docs/17.

**Inherited error 1**: Carried forward the implication that `rebuild_mbedtls.py` might be missing. It exists and works.

**Inherited error 2**: Section 2 stated that SDIO frequency reduction would "likely require lower-level SDMMC host configuration or a different SD access strategy / driver setup". As shown above, the Arduino core already exposes this as a simple parameter.

**New minor issue**: docs/18 Part 2 Section 1 describes "ultra-safe mode" as requiring WiFi to be completely off during listening. This is one valid design, but a less extreme variant — WiFi connected but with CPU at sub-80 MHz during non-WiFi phases — was not explored. This document addresses that gap.

---

## Part 2: SD Card 1-bit Mode at 5 MHz — Complete Technical Assessment

### Current state

The firmware currently mounts the SD card with:

```cpp
SD_MMC.begin("/sdcard", SDIO_BIT_MODE_FAST)
```

Where `SDIO_BIT_MODE_FAST` is defined as `false` in `pins_config.h`, meaning **4-bit SDIO mode** at the default frequency of `SDMMC_FREQ_HIGHSPEED` (40 MHz).

The codebase also already defines `SDIO_BIT_MODE_SLOW = true` in `pins_config.h` but does not currently use it.

### Available SDMMC frequency constants (from ESP-IDF)

| Constant              | Value (kHz) | Description                  |
|-----------------------|-------------|------------------------------|
| `SDMMC_FREQ_PROBING`  | 400         | Initial card identification  |
| Custom                | 5000        | 5 MHz — user-specified       |
| `SDMMC_FREQ_DEFAULT`  | 20000       | Default SD speed             |
| `SDMMC_FREQ_26M`      | 26000       | MMC 26 MHz                   |
| `SDMMC_FREQ_HIGHSPEED` | 40000      | SD High Speed (current default) |
| `SDMMC_FREQ_52M`      | 52000       | MMC 52 MHz                   |

### What switching to 1-bit at 5 MHz would look like

The change in `SDCardManager.cpp` would be:

```cpp
// Before:
SD_MMC.begin("/sdcard", SDIO_BIT_MODE_FAST)

// After:
SD_MMC.begin("/sdcard", true, false, 5000)
// 1-bit mode, no format-on-fail, 5 MHz clock
```

This is a **trivial one-line change**. No lower-level driver work is needed.

### Throughput impact

At 1-bit, 5 MHz:
- Theoretical max: ~5 Mbit/s = ~625 KB/s
- Practical (with protocol overhead): ~400-500 KB/s

The firmware's upload throughput is bottlenecked by:
- WiFi: typically 1-4 Mbit/s effective for TCP upload
- TLS encryption: CPU-bound at 80 MHz
- Effective upload speed: typically ~100-300 KB/s for Cloud, ~500-1000 KB/s for SMB

So at 5 MHz 1-bit:
- **Cloud uploads**: SD card read speed (~400-500 KB/s) still exceeds TLS upload speed (~100-300 KB/s). **No measurable slowdown.**
- **SMB uploads**: The SD card may occasionally become the bottleneck if SMB throughput exceeds ~400 KB/s on a fast local network. Slight slowdown possible in the best-case SMB scenario, but the difference is small and occurs only during active upload bursts.

### Power impact

4-bit SDIO at 40 MHz drives 4 data lines + clock + command at high frequency. Switching to 1-bit at 5 MHz:

- **Reduces active data lines from 4 to 1** (3 lines become idle, each saves ~1-5 mA of switching current)
- **Reduces clock frequency by 8x** (40 MHz → 5 MHz), which reduces dynamic power proportionally
- **Reduces SDMMC DMA burst intensity** — smaller, more spread-out bus transactions

Estimated instantaneous SD bus current reduction: **15-30 mA peak** during active reads.

This is meaningful because the SD card read often overlaps with WiFi TX and TLS computation.

### Recommendation

**Strong recommendation to implement.** This is the single easiest hardware-level power optimization available. One line of code, zero functional downside for the actual upload speeds this firmware achieves, and material reduction in peak bus current.

For even more conservative operation, consider making this configurable:

```
SD_BUS_MODE=SLOW    # 1-bit, 5 MHz (default for brownout-sensitive hardware)
SD_BUS_MODE=FAST    # 4-bit, 40 MHz (for users on robust power supplies)
```

---

## Part 3: CPU Frequency Below 80 MHz — The Real Constraint

### The 80 MHz floor: what it actually is

The ESP32 WiFi PHY requires a minimum CPU frequency of **80 MHz** when the WiFi stack is active. This is enforced by the WiFi driver, which internally acquires a `ESP_PM_CPU_FREQ_MAX` PM lock during active WiFi operations (scanning, association, data TX/RX, beacon processing).

The 80 MHz floor is not an SD card constraint, not a PCNT constraint, and not a general ESP32 hardware constraint. It is **specifically and exclusively a WiFi PHY constraint**.

### What happens below 80 MHz

The ESP32 supports these CPU frequencies:

| Frequency | Clock Source | Notes                                    |
|-----------|-------------|------------------------------------------|
| 240 MHz   | PLL         | Maximum performance                      |
| 160 MHz   | PLL         | High performance                         |
| 80 MHz    | PLL         | WiFi PHY minimum                         |
| 40 MHz    | XTAL        | No PLL needed — significant power saving |
| 20 MHz    | XTAL        | Very low power                           |
| 10 MHz    | XTAL        | Minimum practical                        |

When the CPU runs from the XTAL (40 MHz crystal) at 40 MHz or below:
- The PLL is powered down entirely
- This alone saves approximately **5-10 mA** of static current
- APB bus clock also drops (to 40 MHz at 40 MHz CPU, or lower)
- All peripherals clocked from APB run slower

### When can the CPU go below 80 MHz?

**Only when WiFi is disconnected or disabled.**

In the current firmware architecture, WiFi is always connected. So the CPU cannot drop below 80 MHz during normal operation.

However, if the firmware adopted a design where WiFi is **disabled during certain phases** (e.g., the LISTENING state in smart mode, or the idle period between scheduled upload windows), the CPU could drop to 40 MHz or even 20 MHz during those phases.

### Does 1-bit SDIO at 5 MHz unlock lower CPU frequencies?

**Not directly, but it removes one of the obstacles.**

The SD card itself does not impose a CPU frequency minimum. The SDMMC host peripheral runs from the APB clock, and at 40 MHz CPU (APB = 40 MHz), a 5 MHz SD clock is comfortably achievable.

The PCNT peripheral also runs from the APB clock. At 40 MHz APB, PCNT still counts edges perfectly. The glitch filter value would need adjustment (currently set to 10 APB cycles = ~125 ns at 80 MHz; at 40 MHz APB it would be ~250 ns, which is still fine for CS_SENSE detection at MHz-range SD bus speeds).

So with WiFi off:
- CPU at 40 MHz: **all firmware peripherals work fine** (PCNT, SDMMC at 5 MHz, LittleFS, GPIO, UART/Serial)
- CPU at 20 MHz: **most peripherals work**, but SDMMC at 5 MHz needs APB ≥ 10 MHz (met at 20 MHz CPU). Serial baud rate accuracy may degrade.
- CPU at 10 MHz: **not recommended** — APB at 10 MHz limits SDMMC to ≤ 2.5 MHz, and Serial at 115200 baud becomes unreliable.

### Practical architecture for sub-80 MHz operation

The design would require a **phased power profile**:

| Phase       | WiFi    | CPU      | SD Card | Current Draw |
|-------------|---------|----------|---------|--------------|
| LISTENING   | OFF     | 40 MHz   | Not mounted | ~15-20 mA |
| ACQUIRING   | OFF     | 40 MHz   | Mounted (5 MHz 1-bit) | ~25-35 mA |
| Pre-upload  | Connecting | 80 MHz | Mounted | ~80-120 mA peak |
| UPLOADING   | ON      | 80 MHz   | Mounted (5 MHz 1-bit) | ~100-300 mA peak |
| COOLDOWN    | ON or OFF | 40 MHz | Not mounted | ~15-30 mA |

The key transitions:
1. **LISTENING → ACQUIRING**: CPU stays at 40 MHz, no WiFi needed yet
2. **ACQUIRING → Pre-upload**: CPU scales to 80 MHz, WiFi connects
3. **Upload complete → COOLDOWN**: WiFi can disconnect, CPU drops to 40 MHz

### Is 40 MHz during LISTENING worth it?

Compared to current 80 MHz with WiFi ON:

| State     | Current (now)  | Current (40 MHz, WiFi off) | Savings |
|-----------|---------------|----------------------------|---------|
| LISTENING | ~30-50 mA     | ~15-20 mA                 | ~15-30 mA |
| Per day (16h) | ~0.5-0.8 Ah | ~0.24-0.32 Ah          | ~50% daily energy |

This is a **very significant** reduction for smart-mode operation, where the device spends the majority of the day in LISTENING.

### Trade-off

WiFi off during LISTENING means:
- Web UI unreachable until an upload session begins
- mDNS not available
- No live log streaming
- No remote trigger capability

This is exactly the "ultra-safe mode" concept from docs/18, but now with a concrete technical path: **WiFi off + CPU at 40 MHz + PCNT still counting**.

### Recommendation

For brownout-sensitive AS11 hardware in smart mode, this is the **single largest power reduction available**. It should be offered as an opt-in configuration option:

```
POWER_PROFILE=NORMAL    # WiFi always on, 80 MHz, full UI (current behavior)
POWER_PROFILE=LOW       # WiFi on-demand, 40 MHz idle, UI only during upload windows
```

---

## Part 4: ESP32 Hardware Accelerators — What Is Used and What Remains

### Already used (confirmed in codebase and build config)

| Accelerator | Where Used | Status |
|---|---|---|
| **AES hardware engine** | mbedTLS (TLS encryption) | Enabled by default via ESP-IDF `CONFIG_MBEDTLS_HARDWARE_AES=y` (default, not overridden in sdkconfig.defaults) |
| **SHA hardware engine** | mbedTLS (TLS hashing, certificate verification) | Enabled by default via ESP-IDF `CONFIG_MBEDTLS_HARDWARE_SHA=y` (default) |
| **MPI (big number) hardware engine** | mbedTLS (RSA/ECDHE key exchange during TLS handshake) | Enabled by default via ESP-IDF `CONFIG_MBEDTLS_HARDWARE_MPI=y` (default) |
| **PCNT (Pulse Counter)** | `TrafficMonitor.cpp` — CS_SENSE edge counting | Hardware counts edges autonomously; CPU only reads counter every 100 ms |
| **SDMMC host controller** | `SDCardManager.cpp` — SD card access via DMA | Hardware DMA transfers between SD card and memory |
| **WiFi MAC hardware** | WPA2 encryption, AMPDU aggregation, beacon processing | All handled in hardware by the WiFi MAC subsystem |
| **RNG (Random Number Generator)** | WiFi, TLS (nonce generation, session keys) | Hardware entropy source, always active when WiFi is on |

### Available but NOT currently used

| Accelerator | Potential Use | Feasibility | Impact |
|---|---|---|---|
| **ULP coprocessor** | CS_SENSE monitoring during light/deep sleep | HIGH — GPIO 33 is RTC_GPIO8, accessible by ULP | Major (see Part 5) |
| **Hardware CRC32** | ROM function `esp_rom_crc32_le()` available | LOW priority — no hot CRC path in current code | Minimal |
| **DAC** | Not applicable | N/A | None |
| **Touch sensor** | Not applicable | N/A | None |
| **I2S** | Not applicable | N/A | None |

### Key finding: the ESP32's hardware crypto is already fully utilized

The TLS path already benefits from hardware AES-128-GCM (AES engine + GCM mode), hardware SHA-256 (for HMAC and certificate chains), and hardware MPI (for the ECDHE key exchange in the TLS handshake). The custom `rebuild_mbedtls.py` script ensures the correct mbedTLS build is in place.

There is **no unused hardware crypto accelerator** that could further reduce TLS CPU load. The remaining TLS cost is inherent protocol overhead (record framing, state machine, memory copies) that cannot be hardware-offloaded on the ESP32.

### MD5 hashing: uses ESP32 ROM function, not software

The `computeContentHash()` and upload-loop MD5 calculations use `esp32/rom/md5_hash.h` (confirmed in `SleepHQUploader.cpp` includes). This is a ROM-resident MD5 implementation. While not a dedicated hardware accelerator like AES/SHA, ROM functions execute from the instruction cache and are significantly faster than pure software implementations.

The ESP32 SHA hardware accelerator does **not** support MD5 (only SHA-1, SHA-256, SHA-384, SHA-512). So the ROM MD5 is the best available option.

---

## Part 5: ULP Coprocessor for CS_SENSE Monitoring — Detailed Feasibility

### Hardware compatibility check

| Requirement | Status | Detail |
|---|---|---|
| ULP available on ESP32-PICO-D4? | **YES** | ULP-FSM coprocessor is present on all ESP32 variants |
| CS_SENSE (GPIO 33) accessible by ULP? | **YES** | GPIO 33 = RTC_GPIO8, one of the RTC-domain GPIOs the ULP can read |
| ULP memory available? | **YES** | 8 KB RTC slow memory shared between ULP program and RTC data |
| ULP can wake main CPU? | **YES** | Via `ulp_wakeup` instruction or RTC timer |

### What the ULP can do

The ULP-FSM on the base ESP32 is programmed in a simple assembly language. It can:

- Read RTC GPIO pins (including GPIO 33 / CS_SENSE)
- Maintain counters and timers in RTC slow memory
- Execute simple branching logic
- Wake the main CPU via software interrupt
- Run periodically on the RTC timer (configurable interval, typically 10 ms - 1000 ms)

### Proposed ULP program logic

```
// Pseudocode for ULP CS_SENSE monitor

every 100ms:
    read GPIO 33 (CS_SENSE)
    if level indicates activity (low):
        reset idle_counter to 0
    else:
        idle_counter += 1
    
    if idle_counter >= (INACTIVITY_SECONDS * 10):
        // Bus has been silent for the required duration
        wake main CPU
        halt ULP
```

### What this enables

With the ULP monitoring CS_SENSE:

1. The main CPU can release the PM lock in LISTENING state
2. With `light_sleep_enable = true` and the PM lock released, the CPU enters automatic light sleep
3. The CPU wakes only on:
   - ULP interrupt (bus silence detected → time to upload)
   - WiFi DTIM beacon (if WiFi is still connected)
   - GPIO wakeup (if configured)
4. If WiFi is disconnected in LISTENING, the CPU can enter even deeper sleep between ULP checks

### Power savings estimate

| Mode | Current (LISTENING) | Notes |
|---|---|---|
| Current (CPU at 80 MHz, PM lock held, WiFi on) | ~30-50 mA | CPU always awake, WiFi associated |
| ULP + light sleep + WiFi on | ~5-15 mA | CPU sleeps between DTIM, ULP samples GPIO |
| ULP + light sleep + WiFi off | ~1-3 mA | Only ULP + RTC running |
| ULP + deep sleep + WiFi off | ~0.15-0.5 mA | Maximum savings, but WiFi reconnect needed |

### Important constraints

**ULP-FSM limitations:**
- No direct PCNT access — the ULP cannot read the PCNT counter register. It can only read GPIO levels.
- GPIO level sampling (not edge counting) means the ULP detects "is the bus currently active?" rather than "how many edges occurred?" This is sufficient for idle detection but not for the current `TrafficMonitor` sample-buffer statistics.
- ULP program size is limited to ~8 KB of RTC slow memory (shared with data variables).

**Product behavior impact:**
- If WiFi is kept on during ULP-monitored LISTENING: web UI remains reachable but CPU wake latency on incoming HTTP requests may add ~100-300 ms of perceived lag.
- If WiFi is off during ULP-monitored LISTENING: full "ultra-safe" mode, no connectivity until upload trigger.

### Implementation complexity

- **Medium**. ULP programs are written in ULP assembly (or ULP-C with the ulp-riscv toolchain on S2/S3, but ESP32 base only supports assembly via `ulp_macros.h`).
- Requires: ULP assembly source file, build integration via `ulp_embed_binary()` in CMake or PlatformIO equivalent, shared RTC memory variables for idle counter and threshold.
- Testing requires careful validation that GPIO 33 level transitions are reliably detected at the ULP sampling rate.

### Recommendation

**High-value opportunity, medium implementation effort.**

The ULP approach is the only path that truly solves the "smart mode keeps the CPU awake all day" problem without eliminating smart mode entirely.

For a phased approach:
1. First: implement WiFi-off + 40 MHz CPU during LISTENING (simpler, large savings)
2. Second: add ULP monitoring to enable true light-sleep during LISTENING (deeper savings)

---

## Part 6: mDNS — "Respond Once Then Unload" Pattern

### Current behavior

mDNS is started immediately after WiFi connects (`wifiManager.startMDNS()`) and remains active for the lifetime of the connection. It is restarted on every WiFi reconnect. The ESP-IDF mDNS component runs a background task that:

- Joins the multicast group `224.0.0.251`
- Listens for and responds to `.local` queries
- Maintains service advertisement records
- Consumes: ~4 KB task stack + multicast socket + periodic multicast traffic

### Why mDNS matters for brownout

mDNS is not a major power consumer by itself. But it has secondary effects:

- **Multicast group membership** causes the WiFi radio to wake on multicast frames (not just unicast), reducing the effectiveness of modem sleep
- **Reconnect-time mDNS restart** adds activity during an already-expensive reconnection sequence
- **Always-on responder task** prevents the firmware from fully quiescing network activity

### "Respond once then unload" pattern

The idea: start mDNS on boot (or WiFi connect), let it respond to the initial browser discovery query, then after a configurable timeout (e.g., 60 seconds), call `MDNS.end()` to tear down the responder.

**How it would work:**

1. `wifiManager.startMDNS()` as today
2. Start a timer: `mdnsShutdownAt = millis() + 60000`
3. In the main loop: `if (millis() > mdnsShutdownAt && mdnsStarted) { MDNS.end(); mdnsStarted = false; }`
4. The browser that found the device via `cpap.local` in the first 60 seconds will have resolved the IP address and bookmarked it. Subsequent requests use the IP directly.
5. The existing `redirectToIpIfMdnsRequest()` already redirects `.local` requests to the IP address, so even if mDNS is stopped, previously-open browser tabs would be redirected.

**On WiFi reconnect:**
- Do NOT restart mDNS (the browser already knows the IP)
- Or restart it for another brief window if the IP changed (DHCP lease change)

### Power savings

- Eliminates multicast wake events after the initial discovery window
- Removes the mDNS task and its periodic processing
- Reduces WiFi reconnect overhead (no mDNS restart needed)

Estimated savings: **small but real** — primarily by improving modem sleep effectiveness. Perhaps ~1-3 mA average reduction due to fewer multicast wakes.

### Alternative: configurable mDNS

```
MDNS_MODE=ALWAYS     # Current behavior
MDNS_MODE=TIMED      # Active for 60 seconds after boot/reconnect, then stops
MDNS_MODE=OFF        # Never started
```

### Recommendation

**Good incremental improvement.** The "timed mDNS" pattern is clean, backward-compatible (first discovery still works), and reduces long-term network noise. The existing `redirectToIpIfMdnsRequest()` helper already handles the "mDNS is gone but browser still uses .local" edge case.

---

## Part 7: OTA Manager — On-Demand Initialization via GUI Button

### Current behavior

The `OTAManager` is initialized at boot in `setup()`:

```cpp
#ifdef ENABLE_OTA_UPDATES
    if (!otaManager.begin()) { ... }
    otaManager.setCurrentVersion(VERSION_STRING);
#endif
```

And linked to the web server:

```cpp
webServer->setOTAManager(&otaManager);
```

The web server then registers OTA-related routes (firmware upload endpoint, version check, etc.) at `begin()` time.

### What the OTA manager actually costs at idle

Looking at `OTAManager.cpp`, the `begin()` function is remarkably lightweight:

```cpp
bool OTAManager::begin() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    // ... log partition label
    return true;
}
```

It does **not**:
- Start any background polling or check task
- Open any network connections
- Allocate any significant heap
- Register any periodic callbacks

The OTA manager is essentially a **dormant state machine** that only activates when the web server receives a firmware upload POST request.

### What does cost something

The web server registers OTA routes at boot. These route handlers exist in the route table. The `HTTPClient` include in `OTAManager.h` pulls in code for the `updateFromURL()` method (download-from-URL OTA), but this is code in flash, not RAM.

The actual cost of having OTA initialized at boot:

- ~100 bytes of RAM for the `OTAManager` object
- Flash code for OTA routes (compiled in via `ENABLE_OTA_UPDATES` flag)
- Attack surface: OTA upload endpoint is always reachable

### "Button-triggered OTA" pattern

The user's idea is sound from a security and cleanliness perspective, even if the power savings are minimal:

1. Do NOT register OTA routes at `begin()` time
2. Add a "Enable OTA Updates" button to the web UI
3. When pressed, the button triggers an API call (e.g., `GET /api/ota-enable`)
4. The handler registers the OTA routes dynamically and sets a flag: `otaEnabled = true`
5. After a timeout (e.g., 10 minutes) or after a successful update, deregister the routes
6. While OTA is not enabled, the upload endpoint returns 404 or 403

### Benefits

- **Security**: OTA endpoint not exposed by default — reduces attack surface
- **Cleanliness**: Clear user intent before accepting firmware uploads
- **Marginal power**: No practical power difference (OTA is already dormant)
- **UX**: User explicitly opts into update mode — less chance of accidental uploads

### Implementation complexity

**Low.** The Arduino `WebServer` class supports `server->on()` at any time, so routes can be registered dynamically. The main design question is whether to also deregister routes after a timeout (which requires removing the route handler — the Arduino WebServer API does not natively support `server->removeRoute()`, so a guard flag in the handler is simpler).

### Recommendation

**Worth implementing for security and UX, but not a meaningful brownout optimization.** The OTA manager is already effectively idle. The main value is attack surface reduction and explicit user intent.

---

## Part 8: Additional Findings from Fresh Codebase Review

### 1. The two-pass Cloud upload problem is confirmed and the fix is straightforward

The upload path in `SleepHQUploader.cpp` currently:

1. Calls `computeContentHash()` which opens the file, reads it entirely, computes MD5, closes the file
2. Calls `httpMultipartUpload()` which opens the file again, reads it entirely while streaming over TLS, and computes MD5 on the fly (again!)

The second MD5 computation in `httpMultipartUpload()` is used as a **verification checksum** (`calculatedFileChecksum`) to detect corruption. The first MD5 from `computeContentHash()` is the `content_hash` sent to the SleepHQ API.

Since the API accepts `content_hash` in the **multipart footer** (after the file data), the pre-computation is unnecessary. The on-the-fly MD5 from the upload loop can serve both purposes:
- Sent in the footer as `content_hash`
- Compared against expected value for integrity verification

This would:
- Eliminate one full file read (saves ~50% of SD card active time per Cloud file)
- Remove one full MD5 pass (saves CPU time)
- Reduce the window of SD-card-mounted time during the most vulnerable phase

### 2. TrafficMonitor sample buffer is unnecessarily large for idle detection

`TrafficMonitor` maintains a `MAX_SAMPLES = 300` circular buffer of `ActivitySample` structs (8 bytes each = **2.4 KB** of RAM) for the web-based SD Activity Monitor.

This buffer is only useful when the MONITORING tab is active in the web UI. During normal LISTENING operation, it accumulates data that is never displayed.

**Opportunity**: Only maintain the sample buffer when MONITORING mode is active. During LISTENING, only track the simple idle counter (which uses no buffer).

Savings: ~2.4 KB of RAM, which matters for heap fragmentation during mixed SMB+Cloud uploads.

### 3. NTP sync delay is 5 seconds and is power-expensive

`ScheduleManager::syncTime()` contains:

```cpp
LOG("[NTP] Waiting 5 seconds for network to stabilize...");
delay(5000);
```

This 5-second blocking delay runs at 80 MHz with WiFi active after initial connection. It is skipped on heap-recovery reboots (`g_heapRecoveryBoot`) but runs on every cold boot.

**Opportunity**: Reduce to 1-2 seconds or make it non-blocking. NTP uses UDP and typically responds in <500 ms. The 5-second wait is overly conservative for most networks.

### 4. WiFi connection attempt is 15 seconds max (30 attempts × 500 ms)

`WiFiManager::connectStation()` loops up to 30 times with 500 ms delays:

```cpp
while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    attempts++;
}
```

This is 15 seconds at 80 MHz with WiFi fully active (scanning, probing). On a clean network, association typically completes in 2-5 seconds.

**Opportunity**: Reduce the timeout to 10 seconds (20 attempts) or implement exponential backoff. The remaining 5 seconds of attempts are unlikely to succeed if the first 10 failed, and they cost ~30-50 mA each second.

### 5. Boot stabilization delay is 15 seconds on cold boot

```cpp
LOG("Waiting 15s for electrical stabilization...");
delay(15000);
```

This is extremely conservative. While some stabilization time is needed, 15 seconds at 80 MHz with WiFi about to connect is expensive.

**Opportunity**: Reduce to 5-8 seconds. The CPAP machine's power rail should stabilize within 1-2 seconds. The remaining time was likely added as a safety margin for SD card state, not power rail stability.

### 6. The 500 ms delay in SD card `takeControl()` could be reduced at 5 MHz

```cpp
// Wait for SD card to stabilize after control switch
delay(500);
```

At 40 MHz 4-bit SDIO, the SD card initialization sequence is fast and aggressive. At 5 MHz 1-bit, the initialization is gentler and produces lower peak current, but the stabilization delay is about card power-up timing, not bus speed.

**Opportunity**: Test whether 200-300 ms is sufficient at 5 MHz 1-bit mode. The lower bus speed actually helps here because the SDMMC driver's initialization probing is less aggressive.

### 7. The MUX switch delay is 300 ms and is safe to keep

```cpp
void SDCardManager::setControlPin(bool espControl) {
    digitalWrite(SD_SWITCH_PIN, espControl ? SD_SWITCH_ESP_VALUE : SD_SWITCH_CPAP_VALUE);
    delay(300);  // Wait for MUX switch to settle
}
```

This is for the analog MUX to physically switch and for the CPAP machine to detect the bus change. 300 ms is reasonable and should not be reduced — it is a hardware timing requirement, not a software overhead.

### 8. `listen_interval` for `WIFI_PS_MAX_MODEM` is still not implemented

All three prior documents noted this. The code still calls `WiFi.setSleep(WIFI_PS_MAX_MODEM)` without configuring `wifi_config_t.sta.listen_interval` via `esp_wifi_set_config()`.

**Without `listen_interval`**: MAX_MODEM mode defaults to waking on every DTIM beacon (typically every 100-300 ms), providing minimal additional benefit over MIN_MODEM.

**With `listen_interval = 10`**: The radio would sleep through 10 DTIM intervals, waking only every 1-3 seconds. This would provide substantial power savings during IDLE and COOLDOWN states.

**Implementation**: ~10 lines of code in `WiFiManager::applyPowerSettings()` to call `esp_wifi_get_config()`, set `.sta.listen_interval`, and `esp_wifi_set_config()`.

### 9. `pushSseLogs()` runs every main loop iteration even with no SSE client

```cpp
void pushSseLogs() {
    if (!g_sseActive) return;  // Early exit — essentially free
    ...
}
```

The early `g_sseActive` check makes this nearly free. However, when an SSE client IS connected, the function checks for new log data and writes it to the socket on every loop iteration (~10-100 ms). During uploads, this means SSE pushes compete with upload traffic for WiFi TX time.

**Opportunity**: When `uploadTaskRunning` is true, either:
- Forcibly close the SSE connection (`g_sseClient.stop(); g_sseActive = false;`)
- Or throttle SSE pushes to once per second instead of every loop iteration

### 10. Status snapshot rebuilds every 3 seconds regardless of connected clients

```cpp
if (millis() - lastStatusSnapMs >= 3000) {
    webServer->updateStatusSnapshot();
    lastStatusSnapMs = millis();
}
```

`updateStatusSnapshot()` is stack-based and zero-heap, so it is lightweight. But it still consumes CPU cycles every 3 seconds even when no browser is connected.

**Opportunity**: Only rebuild the snapshot when a `/api/status` request arrives (on-demand). The function is already safe to call from the request handler. This would eliminate ~330 unnecessary rebuilds per 1000 seconds of idle time.

---

## Part 9: Comprehensive Priority Matrix

### Tier 1: Highest impact, lowest risk

| # | Optimization | Effort | Impact | Risk |
|---|---|---|---|---|
| 1 | **Switch SD to 1-bit mode at 5 MHz** | Trivial (1 line) | 15-30 mA peak reduction during SD reads | Very low — throughput still exceeds upload speed |
| 2 | **Eliminate two-pass Cloud hashing** (single-pass upload) | Medium (~50 lines) | ~50% reduction in SD active time per Cloud file | Low — API supports footer hash, on-fly MD5 already computed |
| 3 | **Implement `listen_interval` for MAX_MODEM** | Low (~10 lines) | Significant idle power reduction in IDLE/COOLDOWN | Low — well-documented ESP-IDF API |
| 4 | **Timed mDNS** (60s window then stop) | Low (~15 lines) | Fewer multicast wakes, cleaner modem sleep | Very low — IP redirect already exists |

### Tier 2: High impact, requires design decisions

| # | Optimization | Effort | Impact | Risk |
|---|---|---|---|---|
| 5 | **WiFi-off + 40 MHz CPU during LISTENING** (opt-in low-power profile) | Medium-high | ~50% daily energy reduction in smart mode | Medium — loss of live UI during listening |
| 6 | **Reduce boot/NTP/connect delays** (15s→5s stabilization, 5s→2s NTP, 15s→10s WiFi) | Low | ~15-20 seconds less at 80 MHz + WiFi per boot | Low — conservative margins reduced |
| 7 | **Close SSE during uploads** or throttle to 1/s | Low | Eliminates competing WiFi TX during upload | Low — browser reconnects SSE automatically |
| 8 | **On-demand status snapshot** (rebuild on request, not every 3s) | Low | Eliminates unnecessary CPU work | Very low |

### Tier 3: High impact, significant implementation effort

| # | Optimization | Effort | Impact | Risk |
|---|---|---|---|---|
| 9 | **ULP coprocessor for CS_SENSE** (true light-sleep in LISTENING) | High (ULP assembly + integration) | Drops LISTENING current from 30-50 mA to 1-15 mA | Medium — ULP assembly, GPIO sampling validation |
| 10 | **OTA on-demand** (button-triggered route registration) | Low | Minimal power impact, security improvement | Very low |
| 11 | **Conditional TrafficMonitor sample buffer** (only allocate in MONITORING) | Low | ~2.4 KB heap savings | Low |

### Tier 4: Experimental / measurement-dependent

| # | Optimization | Effort | Impact | Risk |
|---|---|---|---|---|
| 12 | lwIP TCP window pacing | Medium | Unknown — needs measurement | High — could worsen throughput/reliability |
| 13 | Explicit SD-read / WiFi-write decoupling (ping-pong) | Medium | Unknown — depends on actual overlap timing | Medium |
| 14 | SD card stabilization delay reduction (500 ms → 200 ms) | Trivial | Small — saves 300 ms per mount at ~30 mA | Low but needs hardware testing |

---

## Part 10: Combined Power Profile Concept

Drawing together all findings, here is what a comprehensive low-power configuration could look like:

### "Brownout-Safe" profile (all Tier 1 + selected Tier 2)

```ini
# config.txt for brownout-sensitive AS11 hardware
UPLOAD_MODE=scheduled
UPLOAD_START_HOUR=10
UPLOAD_END_HOUR=12
CPU_SPEED_MHZ=80
WIFI_TX_PWR=LOW
WIFI_PWR_SAVING=MAX         # With listen_interval properly configured
SD_BUS_MODE=SLOW            # 1-bit, 5 MHz
MDNS_MODE=TIMED             # 60 seconds then stop
```

### "Ultra-Safe" profile (all tiers, maximum power reduction)

```ini
# config.txt for most marginal hardware
UPLOAD_MODE=scheduled
UPLOAD_START_HOUR=10
UPLOAD_END_HOUR=11
CPU_SPEED_MHZ=80             # 80 during WiFi, 40 during idle (auto)
WIFI_TX_PWR=LOW
WIFI_PWR_SAVING=MAX
SD_BUS_MODE=SLOW
MDNS_MODE=OFF
POWER_PROFILE=LOW            # WiFi off during non-upload, CPU at 40 MHz
ENDPOINT_TYPE=SMB            # No TLS pressure
```

### Estimated current draw comparison

| Phase | Current Profile | "Brownout-Safe" | "Ultra-Safe" |
|---|---|---|---|
| Smart LISTENING | ~30-50 mA | N/A (scheduled only) | N/A (scheduled only) |
| Scheduled IDLE | ~25-40 mA | ~15-25 mA | ~5-15 mA |
| Upload (Cloud) | ~150-300 mA peak | ~100-200 mA peak | N/A (SMB only) |
| Upload (SMB) | ~80-150 mA peak | ~60-120 mA peak | ~50-100 mA peak |
| COOLDOWN | ~25-40 mA | ~15-25 mA | ~5-15 mA |

---

## Part 11: Summary of All Corrections to Prior Documents

### docs/16

| Section | Issue | Correction |
|---|---|---|
| 7 | States `rebuild_mbedtls.py` is missing | File exists and works correctly — TLS optimizations ARE in effect |
| 7 | Warns TLS CPU cost "may be higher than assumed" | Should be retracted |

### docs/17

| Section | Issue | Correction |
|---|---|---|
| 1 | "Cuts SD card activity time by exactly 50%" | "Cuts full-file SD reads roughly in half for Cloud uploads" |
| 2 | Implies frequency tuning requires low-level driver work | `SD_MMC.begin()` has a 4th frequency parameter — trivial change |
| 3 | "CPU at 160MHz for TLS" | Default is 80 MHz; 160 only when user configures it |
| 4 | ULP enables "deep sleep or true light sleep" straightforwardly | Feasible but requires ULP assembly, GPIO 33 validation, and product trade-offs |
| 5 | Recommends TCP_WND/TCP_MSS reduction as a clear fix | Experimental; could destabilize concurrent workloads |
| 7 | Web server "remains fully active" during uploads | Rate limiting, 429 responses, socket release, and log throttling already exist |

### docs/18

| Section | Issue | Correction |
|---|---|---|
| Part 1, §2 | "Would likely require lower-level SDMMC host configuration" | Arduino `SD_MMC.begin()` already accepts frequency parameter |
| Part 2, §1 | Ultra-safe mode only considered as WiFi on/off binary | CPU at 40 MHz without WiFi is a concrete intermediate option |
| Inherited | Carried forward `rebuild_mbedtls.py` concern from docs/16 | File exists and works |

---

## Final Conclusion

After reading all three prior documents, checking them against the code, and performing a fresh hardware-focused audit:

### The single easiest win

**Switch SD card to 1-bit mode at 5 MHz.** One line of code. Material peak current reduction. No functional downside for actual upload speeds.

### The single largest win

**WiFi-off + 40 MHz CPU during non-upload phases** (opt-in low-power profile). This halves the daily energy consumption in smart mode and dramatically reduces the brownout window. The 80 MHz WiFi floor only applies when WiFi is active — with WiFi off, the CPU can safely drop to 40 MHz while PCNT continues to count CS_SENSE edges.

### The deepest future win

**ULP coprocessor monitoring of CS_SENSE.** This allows the main CPU to truly sleep during LISTENING, reducing idle current from ~30-50 mA to potentially ~1-3 mA. Implementation requires ULP assembly programming but the hardware path is confirmed: GPIO 33 is RTC_GPIO8, accessible by the ULP.

### What the prior docs got right

- Cloud/TLS is the dominant electrical stressor — confirmed
- Scheduled mode is materially safer than smart mode — confirmed
- 802.11b disable was the right call — confirmed
- Hardware AES/SHA are already utilized for TLS — confirmed
- The firmware's existing power management infrastructure is solid — confirmed

### What the prior docs got wrong or overstated

- `rebuild_mbedtls.py` exists and works (docs/16 error)
- SDIO frequency tuning is trivial, not complex (docs/17 and 18 error)
- CPU does not jump to 160 MHz during TLS by default (docs/17 error)
- ULP is feasible but not straightforward (docs/17 overstatement)
- Web server already has meaningful upload-time protections (docs/17 understatement)

### Services that can be made smarter without removal

- **mDNS**: "respond for 60 seconds then stop" — keeps first-discovery UX, eliminates ongoing multicast cost
- **OTA**: "enable via GUI button" — keeps full functionality, reduces always-on attack surface
- **Web server**: keep running, but close SSE and serve minimal payloads during uploads
- **Status snapshot**: rebuild on-demand rather than every 3 seconds
- **Log flushing**: reduce frequency during IDLE/COOLDOWN (30s instead of 10s)

---

## Key Code Areas Reviewed

- `src/main.cpp`
- `src/WiFiManager.cpp`
- `src/CpapWebServer.cpp`
- `include/web_ui.h`
- `src/SleepHQUploader.cpp`
- `src/SMBUploader.cpp`
- `src/FileUploader.cpp`
- `src/SDCardManager.cpp`
- `src/TrafficMonitor.cpp`
- `include/TrafficMonitor.h`
- `src/ScheduleManager.cpp`
- `src/UploadStateManager.cpp`
- `src/Logger.cpp`
- `src/OTAManager.cpp`
- `include/OTAManager.h`
- `include/pins_config.h`
- `sdkconfig.defaults`
- `platformio.ini`
- `scripts/rebuild_mbedtls.py`
- Framework: `/root/.platformio/packages/framework-arduinoespressif32/libraries/SD_MMC/src/SD_MMC.h`
- Framework: `tools/sdk/esp32/include/driver/include/driver/sdmmc_types.h`
- Framework: `tools/sdk/esp32/include/esp_pm/include/esp32/pm.h`
