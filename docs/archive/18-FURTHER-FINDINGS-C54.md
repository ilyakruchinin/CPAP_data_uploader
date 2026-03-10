# Further Findings (C54): Corrected Deep Review and Additional Brownout-Reduction Opportunities

## Scope

This document is a **corrected and expanded follow-up** to:

- `docs/16-FURTHER-FINDINGS.md`
- `docs/17-FURTHER-FINDINGS-G31.MD`

Its purpose is to:

- verify `17-FURTHER-FINDINGS-G31.MD` against the actual firmware
- correct any overstatements or inaccuracies
- re-audit the codebase for **additional** opportunities to minimize brownouts
- include deeper architectural options, even where they would change product behavior or user expectations

This is **analysis only**. No code changes are proposed here.

---

## Executive Summary

## Bottom line

The current firmware already contains most of the **obvious first-order mitigations**:

- boot-time CPU throttle to `80 MHz`
- Bluetooth disabled and memory released
- `802.11b` disabled
- early WiFi TX-power clamp before association
- default modem sleep enabled
- DFS/light-sleep infrastructure present
- Cloud TLS trimmed toward hardware-friendly ciphers and smaller buffers
- upload-time web UI protections already exist

So the remaining problem is **not** that the firmware forgot a single basic power-saving switch.

The remaining brownout exposure is more structural:

- **Cloud TLS uploads still create the heaviest RF + CPU stress**
- **WiFi reconnect / reassociation remains bursty and expensive**
- **smart-mode listening keeps the system awake far more than scheduled mode**
- **the device still keeps several convenience services alive at the same time**
- **some operations overlap in ways that maximize instantaneous current draw rather than minimize it**

## Main corrected conclusion

`docs/17-FURTHER-FINDINGS-G31.MD` was **directionally useful**, but several of its statements were too absolute.

The strongest validated opportunities are still:

- reducing the amount of time the SD card stays mounted during Cloud uploads
- reducing overlap between SD activity, CPU-heavy TLS work, and WiFi bursts
- reducing always-on WiFi/network/service activity in smart mode
- adding a stricter low-power operating profile for weak AS11 hardware

The most important *new* strategic conclusion from this re-audit is:

- **an optional “ultra-safe mode” that sacrifices always-on connectivity and live UI responsiveness would likely outperform incremental tuning**

That is the most realistic path if the goal is to minimize brownout risk as aggressively as possible.

---

## Part 1: Validation of `docs/17-FURTHER-FINDINGS-G31.MD`

## 1. Single-pass Cloud upload: directionally correct, but the benefit was overstated

### What `docs/17` got right

The current Cloud path still does both of these:

- pre-computes a content hash via `computeContentHash()`
- reads the file again during `httpMultipartUpload()`

So yes, the current implementation still reads Cloud-uploaded files **twice**.

### What needs correction

`docs/17` said this would cut SD-card activity time by **exactly 50%**.

That is too strong.

What is true is:

- the number of full-file SD reads would drop from two passes to one pass
- therefore SD **data-read volume** for Cloud uploads would be cut roughly in half

But real wall-clock and current-draw improvement would not be exactly 50% because total time also includes:

- TLS handshakes
- network waits
- server response time
- retries
- multipart framing
- reconnection paths

### Corrected assessment

This is still a **high-value optimization opportunity**.

But the right claim is:

- **likely large reduction in SD-active time during Cloud uploads**
- **not guaranteed to cut total Cloud-session power exposure by exactly 50%**

## 2. 1-bit / slower SDIO is plausible and likely useful, but `docs/17` over-implied implementation simplicity

### What `docs/17` got right

The current firmware mounts SD with:

- `SD_MMC.begin("/sdcard", SDIO_BIT_MODE_FAST)`

and `SDIO_BIT_MODE_FAST` is defined as `false`, which in the Arduino `SD_MMC.begin()` API corresponds to **4-bit mode**, not 1-bit mode.

So the firmware is indeed using the higher-throughput 4-bit SDIO configuration.

### What needs correction

`docs/17` implied the solution is simply to “underclock the SDIO frequency via the begin parameters”.

That is too direct for the current code shape.

From the current implementation, the firmware only uses the Arduino `SD_MMC.begin()` wrapper with the simple mode flag. The codebase does **not** currently expose a frequency-tuning path in the same way that SPI-based SD code would.

### Corrected assessment

The architectural idea still stands:

- **1-bit mode is a serious candidate for reducing peak SD-bus stress**
- **lower SD clocking is a serious candidate too**

But frequency reduction would likely require:

- lower-level SDMMC host configuration
- or a different SD access strategy / driver setup
- not just a trivial parameter tweak in the current wrapper usage

So this remains a **good architectural direction**, but not a trivial near-term switch.

## 3. “CPU at 160 MHz during TLS” is not the current default behavior

### What `docs/17` got wrong

`docs/17` described the overlap problem as involving “CPU at 160MHz for TLS”.

That is **not** the current default configuration.

The current code in `main.cpp` configures:

- `pm_config.max_freq_mhz = targetCpuMhz`
- `pm_config.min_freq_mhz = 80`

And `Config.cpp` currently defaults to:

- `CPU_SPEED_MHZ = 80`

So in the default configuration:

- max = 80
- min = 80
- DFS is effectively disabled
- the CPU does **not** normally jump to 160 MHz during TLS

### Corrected assessment

The real overlap problem is still valid, but the statement should be:

- **overlap between SD reads, CPU/TLS work, and WiFi transmission is still undesirable even at 80 MHz**
- the 160 MHz spike concern only applies when a user configures `CPU_SPEED_MHZ = 160`

## 4. ULP offload is promising, but `docs/17` overstated feasibility and consequences

### What `docs/17` got right

The current FSM definitely keeps the PM lock held in `LISTENING`, and smart mode spends a great deal of time there.

That means:

- smart mode prevents the deeper low-power behavior available in `IDLE` and `COOLDOWN`
- this is one of the biggest structural differences between scheduled and smart mode

### What needs correction

`docs/17` implied the ULP could straightforwardly let the system enter deep sleep or true light sleep while preserving behavior.

That needs qualification.

Deep sleep would have major product implications:

- WiFi association would be lost
- mDNS would disappear
- the web UI would not remain continuously reachable
- wake/resume behavior would need redesign

Even light-sleep + ULP-assisted wake would need careful validation around:

- GPIO sampling reliability
- wake latency
- whether the current monitoring semantics remain acceptable

### Corrected assessment

ULP-assisted monitoring remains a **high-potential architectural direction**, but it is best described as:

- **a possible redesign for a specialized low-power mode**
- **not a guaranteed drop-in improvement for the current always-on behavior**

## 5. lwIP window reduction is plausible, but it is an experimental tuning area, not a firm recommendation

### What `docs/17` got right

It is reasonable to suspect that tighter TCP pacing could reduce burst density.

### What needs correction

The current build already tunes lwIP, but in the opposite direction for robustness:

- `CONFIG_LWIP_PBUF_POOL_SIZE=16`
- `CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=32`
- `CONFIG_LWIP_TCP_RECVMBOX_SIZE=8`
- `CONFIG_LWIP_UDP_RECVMBOX_SIZE=6`

Those settings are present to reduce resource exhaustion under concurrent load.

So recommending smaller TCP windows or MSS values as a primary power fix is too speculative without measurement, because it could also:

- increase airtime
- increase retries
- increase protocol overhead
- worsen total session duration
- destabilize already-sensitive mixed workloads

### Corrected assessment

lwIP pacing changes belong in the:

- **measurement-driven experimental bucket**
- **not** the top-priority brownout recommendation bucket

## 6. Web-server lockdown is directionally correct, but the firmware already contains meaningful upload-time protections

### What `docs/17` got right

The web UI can still add network activity during uploads.

### What it underplayed

The code already includes several upload-time protections:

- UI rate-limiting by endpoint slot
- `429` responses in pressure situations
- explicit `Connection: close`
- forced socket release via `server->client().stop()`
- log endpoint throttling while uploads are active
- compact `/api/sd-activity` payload during upload

Also, much of the status path is already carefully optimized:

- periodic status snapshot rebuild is stack-based and zero-heap by design
- `/api/status` is designed to remain available during uploads without relying on stale cached data

### Corrected assessment

The right statement is:

- the web server is still a **secondary amplifier**
- further tightening is possible
- but this area is **not untouched** and should not be described as if uploads currently happen with a fully unrestricted UI

---

## Part 2: Additional Findings from the Fresh Codebase Review

## 1. The strongest remaining architectural lever is an optional “ultra-safe mode” that turns WiFi into an on-demand resource

### Current behavior

The device connects to WiFi, starts mDNS, and keeps the web server alive continuously.

This is convenient, but it means that in smart mode the system spends long periods with:

- WiFi associated
- multicast/mDNS behavior available
- browser/UI activity possible
- reconnect behavior possible if the AP is unstable

### Why this matters

If the user’s true priority is *brownout avoidance above all else*, then always-on connectivity is not aligned with that goal.

### Architectural opportunity

Introduce a dedicated ultra-safe operating profile where the firmware intentionally gives up some convenience:

- WiFi off while merely listening for CPAP inactivity
- WiFi only enabled after idle has been confirmed and an upload session is actually about to begin
- mDNS disabled entirely in this profile
- web UI unavailable except during a short diagnostic window or after manual wake

### Why this is important

This is one of the few changes that reduces **both**:

- baseline power draw across the whole day
- opportunities for reconnect bursts and background network work

### Assessment

If the target population includes extremely weak AS11 units, this is probably the **single biggest architectural option** still available.

It is a product-behavior change, not a small optimization.

## 2. Defer optional services until after uploads, or disable them in constrained profiles

The current firmware initializes and/or maintains multiple conveniences:

- web server
- mDNS
- OTA manager (in OTA builds)
- persistent log saving
- monitoring support

Each may be reasonable individually, but together they increase:

- RAM pressure
- open socket count
- periodic background work
- opportunities for network contention during uploads

### Opportunity

Create deployment profiles rather than one always-featured behavior set.

For example:

- **ultra-safe profile**: no OTA, no mDNS, web UI minimized, scheduled mode only
- **balanced profile**: current default style
- **full convenience profile**: current rich UI + OTA + mDNS behavior

### Why this matters

Brownout risk is not only about instantaneous current. It is also about:

- heap pressure
- retry behavior under partial failure
- how many concurrent subsystems can wake the radio and CPU

A profile-based design would let the weakest hardware run a much leaner system.

## 3. Scheduled mode should be treated as the preferred safety mode, not just one option among equals

This was already hinted earlier, but the current code confirms it strongly.

The PM lock is released only in:

- `IDLE`
- `COOLDOWN`

In smart mode, the system continuously cycles back into `LISTENING`, where the PM lock remains held.

That means scheduled mode is not merely a UX alternative. It is an actual **power architecture difference**.

### Recommendation

For brownout-sensitive installations, the design guidance should strongly prefer:

- scheduled mode
- narrow upload windows
- minimal browser interaction during the window

This is one of the highest-confidence recommendations in the entire analysis.

## 4. Cloud upload remains the dominant electrical stressor, so feature shedding around Cloud is still the most defensible simplification

The current Cloud path still includes:

- TLS handshake and keep-alive management
- OAuth and import orchestration
- large encrypted multipart uploads
- timeout handling
- reconnect/recovery logic
- raw socket reads/writes with repeated wait loops

By comparison, SMB is still an active network load, but Cloud remains the more CPU-expensive and reconnect-sensitive path.

### Architectural implication

If a user has a marginal AS11 unit and only needs one backend, the best design simplification is probably:

- **SMB-only build/profile**
- or **Cloud-disabled runtime profile**

That is more likely to help than micro-optimizing the UI.

## 5. The firmware still favors continuous responsiveness over electrical quiet

The codebase contains many signs that responsiveness remains a design goal:

- main loop continues handling web traffic continuously
- SSE log push runs from the main loop when active
- `/api/status` remains live during uploads
- monitoring can interrupt non-upload states
- WiFi is kept associated continuously

This is not inherently wrong.

But it means the system is still optimized for:

- availability
- live diagnostics
- convenience

rather than for the absolute minimum possible electrical activity.

### Consequence

If brownout minimization becomes the top requirement, the architecture needs a clearer mode separation between:

- **diagnostic / interactive operation**
- **electrically quiet / upload-safe operation**

That separation does not currently exist strongly enough.

## 6. SSE is not huge by itself, but it is one of the few truly continuous live network features

The current UI prefers `EventSource('/api/logs/stream')` when supported.

That is often more efficient than repeated fetches, but it still means:

- a persistent connection is kept open
- the main loop checks/pushes SSE traffic continuously
- log-heavy periods can produce frequent incremental writes

### Assessment

SSE is not the main problem.

But in a strict brownout-minimization design, SSE belongs in the category of:

- useful for diagnostics
- unnecessary in the lowest-power operating profile

## 7. Persistent logs are safer than SD logging, but still create background flash activity

The firmware now persists logs to `LittleFS`, and flushes every `10s`.

That is much better than using the CPAP SD card for logging, but it still means:

- periodic internal flash writes
- periodic CPU wake/work
- additional activity even when not uploading

### Assessment

This is not a top brownout cause.

But a strict low-power profile could still reasonably change logging policy, for example:

- flush less frequently in steady-state idle
- suspend periodic flushes during the most sensitive upload phases
- reduce log verbosity automatically in constrained mode

## 8. Boot- and reconnect-time service sequencing can still be improved conceptually

The firmware already avoids some earlier mistakes, but the architecture still tends to bring services up eagerly.

Examples:

- WiFi is connected before the long-lived runtime begins
- mDNS is started immediately after connect
- OTA is initialized in OTA builds
- web server is created during boot rather than lazily

### Opportunity

For the most constrained profile, the system could defer some of these until one of the following is true:

- upload completed successfully
- user explicitly requests diagnostics
- a manual diagnostic mode is entered

This is more of a product-mode redesign than a bug fix, but it could materially reduce exposure.

## 9. The current lwIP / mailbox tuning appears stability-oriented, not power-oriented

This matters because it explains why some “reduce buffers to save power” ideas are not obviously good.

Current build tuning increases resilience for mixed workloads by enlarging some lwIP resources.

That suggests the system has already encountered concurrency pressure between:

- upload traffic
- web traffic
- mixed socket behavior

### Implication

Future low-power work should not blindly remove these stability-oriented tunings.

Instead, the better strategy is likely:

- reduce the number of concurrent services and connections
- reduce situations that require generous buffering in the first place

That is more architecturally sound than destabilizing the TCP/IP stack.

## 10. WiFi recovery is now coordinated, but recovery itself is still power-expensive by nature

The code now guards against concurrent reconnect attempts and provides a coordinated WiFi cycle path.

That is good.

But even well-coordinated recovery still involves:

- disconnects
- waits
- reassociation
- mDNS restart
- possible TLS rebuild

So this remains a hotspot by design, even when implemented correctly.

### Implication

The best way to reduce recovery-related brownout risk is not only to optimize recovery logic, but to reduce the need for recovery at all:

- fewer simultaneous services
- less background network activity
- cleaner AP conditions
- lower Cloud pressure

---

## Part 3: Highest-Value Remaining Opportunities, Re-Prioritized

## Tier 1: Highest-confidence, highest-value opportunities

### 1. Introduce an optional ultra-safe runtime profile

Characteristics:

- scheduled mode only
- WiFi disabled except shortly before and during uploads
- mDNS disabled
- web UI unavailable except on-demand
- OTA disabled
- reduced logging cadence

This is the strongest remaining architectural move if the objective is maximum brownout avoidance.

### 2. Implement true single-pass Cloud upload

This remains a strong opportunity because it directly reduces SD-card read exposure during the heaviest backend path.

### 3. Offer a hard recommendation or dedicated mode favoring scheduled operation over smart mode

This is a high-confidence reduction because the current power-management behavior already makes scheduled mode materially quieter.

### 4. Offer a constrained backend profile that disables Cloud on weak hardware

If the hardware is marginal, removing the most CPU- and TLS-heavy feature is one of the cleanest ways to reduce risk.

### 5. Explore 1-bit SDIO and lower SDIO clocking in a dedicated low-power branch/profile

This is likely one of the most promising hardware-interface-side optimizations still available.

## Tier 2: Good opportunities, but with more design tradeoffs

### 6. Tighten upload-time web behavior further

Possibilities:

- forcibly close SSE at upload start
- serve only compact status during upload
- block nonessential endpoints more aggressively
- refuse monitor-mode entry during or near uploads

This is worthwhile, but secondary compared with Cloud, reconnect, and smart-mode behavior.

### 7. Defer or disable mDNS and OTA except when truly needed

These features are convenient, but they are not electrically free.

### 8. Make persistent logging power-aware

Lower-value than the items above, but still sensible in a constrained profile.

### 9. Investigate ULP-assisted smart-mode listening as a specialized redesign

Promising, but substantial redesign and product-behavior tradeoffs are involved.

## Tier 3: Experimental / measurement-driven areas

### 10. lwIP packet pacing / smaller TCP windows / MSS changes

Potentially helpful, but highly measurement-dependent.

### 11. More explicit SD-read / WiFi-write decoupling

Still promising, but the exact current benefit depends on driver scheduling, lwIP behavior, and how much overlap is actually occurring at the electrical level.

### 12. Single-core or alternative task-affinity designs

Possible, but lower confidence.

The current split already isolates the upload task on Core 0 and keeps the main loop responsive on Core 1. A different affinity design may help in some ways but is not yet a clearly justified top-priority brownout fix.

---

## Part 4: What I Would Correct in `docs/17-FURTHER-FINDINGS-G31.MD`

If that document is kept, these statements should be softened or corrected:

- “cuts SD card activity time by exactly 50%”
  - better: **cuts full-file SD reads roughly in half for Cloud uploads**

- “CPU at 160MHz for TLS”
  - better: **CPU/TLS work overlaps with SD and WiFi; 160MHz only applies when configured**

- “completely negates the benefit of CONFIG_PM_ENABLE during the day”
  - better: **substantially limits low-power behavior in smart mode because LISTENING holds the PM lock**

- “enter deep sleep or true light sleep” via ULP
  - better: **possible redesign path, but with major product and connectivity tradeoffs**

- “reduce TCP_WND/TCP_MSS” as a clear fix
  - better: **experimental tuning area requiring measurement**

- “web server lockdown” as if current uploads are unprotected
  - better: **further tightening possible, but substantial upload-time protections already exist**

---

## Final Conclusion

After re-checking both the code and `docs/17-FURTHER-FINDINGS-G31.MD`, my conclusion is:

- `docs/17` identified several *good directions*, especially around Cloud upload structure, smart-mode power cost, and SD/WiFi overlap
- but some of its statements were too absolute and should not be treated as settled fact
- the strongest remaining opportunities are **architectural simplifications**, not minor tuning alone

If the goal is to minimize brownouts as much as possible, the firmware should not only be tuned. It should offer a mode that is intentionally **less interactive, less connected, and less concurrent**.

That means the best remaining path is not:

- more UI polish
- more concurrency
- more live diagnostics

It is:

- fewer always-on services
- less always-on WiFi
- less live interactivity during vulnerable periods
- less Cloud/TLS pressure on marginal hardware
- less time with the SD bus mounted under heavy network load

In short:

- **The current firmware is already substantially improved.**
- **The next big gains come from trading convenience for electrical safety.**
- **An ultra-safe low-power profile is likely the most powerful remaining design option.**

---

## Key Code Areas Re-Checked for This Review

- `src/main.cpp`
- `src/WiFiManager.cpp`
- `src/CpapWebServer.cpp`
- `include/web_ui.h`
- `src/SleepHQUploader.cpp`
- `src/SMBUploader.cpp`
- `src/FileUploader.cpp`
- `src/SDCardManager.cpp`
- `src/TrafficMonitor.cpp`
- `src/ScheduleManager.cpp`
- `src/UploadStateManager.cpp`
- `src/Logger.cpp`
- `sdkconfig.defaults`
- `platformio.ini`
