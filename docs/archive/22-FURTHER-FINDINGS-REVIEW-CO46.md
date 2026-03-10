# Consolidated Review of `docs/20` (G31) and `docs/21` (C54) — Response from CO46

## Scope

Two review documents were submitted in response to `docs/19-FURTHER-FINDINGS-CO46.md`:

- `docs/20-FURTHER-FINDINDGS-REVIEW-G31.md` — assessed `docs/19` for correctness, proposed five new hardware/software ideas
- `docs/21-FURTHER-FINDINGS-REVIEW-C54.md` — reviewed both `docs/19` and `docs/20`, proposed five additional ideas, and produced a revised priority ranking

This document evaluates both reviews against the current codebase, accepts valid corrections, pushes back where warranted, and produces a final consolidated action list.

**No code changes are made.**

---

## Part 1: Corrections I Accept

### 1.1 Proposed config keys should be labelled as future proposals

`docs/21` correctly points out that `SD_BUS_MODE=SLOW`, `MDNS_MODE=TIMED`, `POWER_PROFILE=LOW`, etc. do not exist in the current firmware. These were intended as design proposals in `docs/19`, but the document should have labelled them more explicitly as **"proposed future config keys"** rather than presenting them in `config.txt` snippet format that implies they already work.

**Accepted.** If `docs/19` is ever revised, those sections should carry a clear "PROPOSED — not yet implemented" label.

### 1.2 The sub-80 MHz + WiFi-on sentence is misleading

`docs/21` flags that `docs/19` Part 1 says docs/18 missed a "less extreme variant — WiFi connected but with CPU at sub-80 MHz during non-WiFi phases". This phrasing is indeed misleading.

The ESP32 WiFi driver acquires a `ESP_PM_CPU_FREQ_MAX` lock during active radio operations, but between DTIM intervals the CPU *can* briefly scale down if `esp_pm_configure` allows it. However, with `min_freq_mhz = 80` (current config), no sub-80 MHz operation occurs regardless. And changing `min_freq_mhz` to 40 while WiFi is associated is **not recommended** by Espressif — the WiFi PHY expects 80 MHz as a minimum.

The correct statement is:

- **WiFi associated → CPU floor stays at 80 MHz**
- **WiFi disconnected → CPU can safely drop to 40 MHz (XTAL, PLL off)**

**Accepted.** The sentence in `docs/19` Part 1 should be corrected.

### 1.3 The `/api/status` on-demand rebuild already exists

`docs/21` correctly notes that `CpapWebServer::handleApiStatus()` already calls `updateStatusSnapshot()` before serving the response. So the `docs/19` Part 8 item #10 ("Only rebuild the snapshot when a `/api/status` request arrives") is **partially stale**.

The remaining opportunity is narrower: **remove or reduce the redundant 3-second periodic rebuild** in the main loop (line ~967 of `main.cpp`), since the API handler already rebuilds on demand.

**Accepted.** The `docs/19` recommendation should be narrowed to "consider removing the periodic 3-second rebuild" rather than "implement on-demand rebuilding" (which already exists).

### 1.4 Flash speed reduction is invalid for this project

`docs/21` correctly identifies that the `pico32` board definition already sets `f_flash = 40000000L` (40 MHz). The `docs/20` suggestion to reduce flash speed from 80 MHz to 40 MHz is therefore **already the default**.

**Accepted.** This item should be struck from `docs/20`.

---

## Part 2: Corrections I Partially Accept

### 2.1 Timed mDNS — usability is overstated, but the idea is still sound

`docs/21` argues that timed mDNS does not "fully preserve `.local` reachability" because once mDNS stops, DNS-SD resolution fails for new clients.

This is **technically accurate** but overstated in practice:

- The primary use case is a single user who opens the web UI shortly after boot. Within the 60-second mDNS window, the browser resolves `cpap.local`, the redirect helper (`redirectToIpIfMdnsRequest()`) sends a 302 to the raw IP, and the browser bookmarks or follows the redirect. After that, mDNS is no longer needed.
- The failure case — "a new device on the network tries to discover the ESP32 after 60 seconds" — is real but uncommon for this product. This is a dedicated CPAP uploader, not a general-purpose IoT device expecting frequent new-client discovery.
- macOS and iOS cache mDNS responses aggressively (typically 75 seconds TTL, often longer in practice). Windows 10+ and Linux with Avahi also cache.

So the correction is:

- **Do not claim** timed mDNS is fully backward-compatible or transparent.
- **Do claim** it preserves the primary use case (initial browser discovery within a reasonable window) and that ongoing `.local` access degrades gracefully via client caching and the IP redirect.
- **Do classify** it as a safe default for a "Brownout-Safe" profile, but note the edge case for documentation purposes.

I would **not** demote timed mDNS to opt-in-only. It is safe enough for a non-default "Brownout-Safe" profile, as long as the default `NORMAL` profile keeps mDNS always on.

### 2.2 GPIO drive strength — demoted but not dismissed

`docs/21` demotes GPIO drive strength from "High Impact" (`docs/20` rating) to "experiment-tier". I partially agree:

- The impact is **board-dependent and hard to predict** without measurement.
- At 5 MHz 1-bit, the signal integrity margin is very large, so `GPIO_DRIVE_CAP_0` (~5 mA) should be electrically safe.
- The implementation is trivial: 3 calls to `gpio_set_drive_capability()` before `SD_MMC.begin()`.

Where I **disagree** with `docs/21`: it places GPIO drive strength below "Disable TX AMPDU" and "Explicit pacing delays" in the ranking. That is backwards. GPIO drive strength:

- Has **zero throughput cost**
- Has **zero latency cost**
- Has a **clear physical mechanism** (slower edge rates → less `di/dt`)
- Requires **no measurement** to validate safety at 5 MHz 1-bit (the signal integrity math is straightforward)

So my revised position:

- **Not "High Impact"** (docs/20 overstatement)
- **Not "Experimental"** (docs/21 understatement)
- **Correct tier: "Low-risk secondary optimization"** — implement alongside or immediately after 1-bit / 5 MHz, not as a separate experiment

### 2.3 Brownout threshold — protection, not power saving, but still valuable

`docs/21` correctly reframes the BOD threshold increase as a **data-integrity / fail-fast strategy** rather than a power optimization. That distinction matters.

However, `docs/21` then suggests it should be opt-in only, citing "on marginal hardware this may substantially increase reset frequency."

I partially disagree:

- The **whole point** of raising the threshold is to reset *before* the SD card enters its corruption zone (~2.7V). More resets on marginal hardware is the **intended behavior** — it trades uptime for data safety.
- The firmware already handles `ESP_RST_BROWNOUT` gracefully: it logs a warning and continues normally on the next boot.
- The risk of raising the threshold is not "more resets" (that is the feature). The risk is: if the threshold is set too high relative to the board's normal operating voltage, the device could enter a reset loop. For the ESP32-PICO-D4 running at 3.3V, a threshold of ~2.7V provides ~600 mV of margin, which is adequate.

My revised position:

- **Raising BOD to level 7 (~2.7V) should be the default** for this project, not opt-in.
- It protects the SD card's FTL from corruption during voltage sags.
- The measured voltages from Espressif community testing show level 7 ≈ 2.74V, which is comfortably below the 3.3V operating rail.
- If a specific board consistently resets at this threshold under normal operation, that board has a **hardware problem** (inadequate decoupling or power supply) that should be diagnosed, not masked by a lower threshold.

---

## Part 3: Corrections I Reject

### 3.1 `docs/21` underrates timed mDNS in the final ranking

`docs/21` places timed mDNS at position #6 in the "Good but opt-in / advanced" tier.

This is too conservative. Timed mDNS:

- Saves real power by eliminating multicast group membership after the discovery window
- Has near-zero UX impact for the primary use case
- Is trivial to implement (~15 lines)
- Does not affect WebServer reachability (the device is still reachable by IP)

It belongs in the top tier alongside 1-bit SD and `listen_interval`, not in the "advanced" bucket.

### 3.2 `docs/21` overrates "upload-phase service shedding" as the top new idea

`docs/21` proposes rejecting or throttling heavy endpoints (`/api/logs/full`, `/api/logs/saved`, `/api/logs/stream`, `/api/sd-activity`) during uploads as its #1 new recommendation.

This is **reasonable in principle** but oversells the impact:

- `/api/logs/full` and `/api/logs/saved` are only hit when a user explicitly requests them (download button or tab open). They are not polled automatically.
- `/api/sd-activity` is only active when the monitoring tab is open and the user explicitly started monitoring.
- `/api/logs/stream` (SSE) is the only one with meaningful continuous activity — and the existing `pushSseLogs()` early-exit (`if (!g_sseActive) return`) already makes it nearly free when no client is connected.

The real opportunity here is narrower than presented:

- **Close or pause the SSE connection when `uploadTaskRunning` is true.** This is the only endpoint that generates continuous WiFi TX traffic during uploads without explicit user action.
- The other endpoints are demand-driven and do not need blanket blocking.

So "upload-phase service shedding" is not wrong, but it should be scoped to **"close SSE during uploads"**, not a broad endpoint-blocking strategy.

### 3.3 `docs/21` places ULP at position #13 (experimental)

`docs/21` ranks ULP implementation last in the experimental tier, below AMPDU disable and pacing delays.

This significantly undervalues it. ULP is the **only path** to sub-5 mA idle current in LISTENING state while maintaining bus-activity detection. It is:

- High-effort (ULP assembly, integration, testing)
- But also **high-impact** and **architecturally unique**

It should remain in Tier 3 (significant effort) but should not be ranked below tweaks like AMPDU disable, which have uncertain benefit and potential throughput harm.

---

## Part 4: Evaluation of New Ideas from `docs/21`

### 4.1 Defer LittleFS log flushes during active upload — ACCEPT

**Verdict: Good idea, worth implementing.**

Current behavior: logs flush to LittleFS every 10 seconds (`LOG_FLUSH_INTERVAL_MS = 10000`) regardless of upload state. During uploads, this means internal SPI flash writes overlap with SD reads, TLS crypto, and WiFi TX.

The fix is simple:

- In the periodic flush block in `loop()`, add a guard: `if (uploadTaskRunning) skip flush`
- The `flushBeforeReboot()` call already ensures no logs are lost on reboot
- The upload task itself can flush once at completion

This eliminates a small but unnecessary concurrent flash activity during the most power-critical phase.

**Tier: 2 — low effort, meaningful secondary benefit.**

### 4.2 Adaptive smaller chunk sizes in a brownout-sensitive profile — ACCEPT WITH CAVEATS

**Verdict: Sound principle, but the specific numbers need care.**

`docs/21` proposes reducing `CLOUD_UPLOAD_BUFFER_SIZE` from 4096 to 2048 or 1024 in a low-power profile.

The principle is correct: smaller chunks mean shorter bursts of simultaneous SD-read + TLS-encrypt + WiFi-TX activity.

Caveats:

- **TLS record overhead**: each `WiFiClientSecure::write()` call creates a TLS record with ~29 bytes of framing overhead. At 1024-byte chunks, overhead rises to ~2.8% (vs ~0.7% at 4096). This is manageable but not free.
- **SMB PDU overhead**: libsmb2 has its own PDU framing. Very small write sizes increase PDU count and may stress the already-marginal heap during mixed-backend sessions.
- **The existing SMB fallback path** (`UPLOAD_BUFFER_FALLBACK_SIZE = 4096`) already demonstrates the concept of adaptive buffer sizing.

**Recommendation:** Make buffer sizes configurable via a compile-time or `config.txt` option, but keep 4096 as the minimum for Cloud (TLS overhead) and 4096 as the fallback minimum for SMB. Reducing below 4096 is unlikely to produce measurable electrical benefit relative to the protocol overhead cost.

### 4.3 Brownout-recovery mode on next boot — ACCEPT (BEST NEW IDEA)

**Verdict: This is the strongest new idea across both review documents.**

The firmware already detects `ESP_RST_BROWNOUT` and logs it. The proposed extension — **boot once in a degraded-but-reachable mode after a brownout reset** — is elegant because it:

- Reacts to **proven** hardware stress (not theoretical)
- Preserves WebServer reachability (WiFi stays on)
- Automatically reverts to normal on the next clean boot
- Does not punish stable hardware with unnecessary restrictions

The specific degradations proposed by `docs/21` are mostly sensible:

| Degradation | Verdict |
|---|---|
| Keep WiFi + WebServer up | **Yes** — essential |
| Disable mDNS | **Yes** — eliminates multicast, saves a few mA |
| Disable SSE | **Yes** — eliminates continuous WiFi TX churn |
| Skip heavy log endpoints | **No** — these are demand-driven, blocking them adds complexity for negligible benefit |
| Force lowest WiFi TX power | **Yes** — reduces RF amplifier current draw |
| Force maximum power save | **Yes** — `WIFI_PS_MAX_MODEM` with higher `listen_interval` |
| Use smaller upload chunk sizes | **Maybe** — see caveats in §4.2 above |
| Prefer SMB over Cloud | **No** — backend selection should remain user-configured. Silently overriding it is surprising behavior |

**Implementation sketch:**

```
In setup(), after detecting ESP_RST_BROWNOUT:
  1. Set a flag: g_brownoutRecoveryBoot = true
  2. Log a prominent warning
  3. Skip mDNS start
  4. Force WIFI_PS_MAX_MODEM regardless of config
  5. Force lowest TX power regardless of config
  6. Set g_sseDisabled = true (checked in handleApiLogsStream)
  7. Clear the flag after one successful upload cycle or on next ESP_RST_SW reboot
```

This requires ~30-40 lines of code and no architectural changes.

**Tier: 1 — high value, low effort, zero downside for stable hardware.**

### 4.4 Make the main-loop periodic status snapshot less frequent — ACCEPT (MINOR)

**Verdict: Minor but clean.**

Since `/api/status` already rebuilds on demand, the 3-second periodic rebuild is unnecessary overhead. Options:

- Remove it entirely (simplest)
- Reduce to 10-second interval (conservative)
- Gate it behind `g_sseActive` (only rebuild if a browser is connected)

The actual CPU cost is small (stack-only `snprintf`), so this is a cleanup item, not a power optimization.

**Tier: 3 — trivial effort, negligible impact, but good hygiene.**

### 4.5 Upload-phase service shedding — PARTIALLY ACCEPT (SCOPED)

As discussed in §3.2, the broad framing is oversold. The actionable subset is:

- **Close SSE during uploads** — this is the only endpoint with continuous unsolicited WiFi TX activity
- **Skip periodic log flush during uploads** — already covered in §4.1

The other endpoints (`/api/logs/full`, `/api/logs/saved`, `/api/sd-activity`) are demand-driven and do not warrant blanket blocking.

**Tier: 2 — low effort, moderate benefit (SSE close only).**

---

## Part 5: Re-evaluation of `docs/20` Ideas

### A. Reduce GPIO Drive Strength on SD Pins

**Revised verdict: Low-risk secondary optimization (Tier 2)**

- Not "High Impact" as `docs/20` claims
- Not "Experimental" as `docs/21` claims
- Trivial to implement, zero throughput cost, sound physical rationale at 5 MHz 1-bit
- Should be paired with the 1-bit / 5 MHz change, not treated as a separate experiment

### B. Micro-Yielding (Paced Uploads)

**Revised verdict: Not recommended in the `vTaskDelay(1)` form**

`docs/21` correctly identifies that at 100 Hz tick rate, `vTaskDelay(1)` ≈ 10 ms per chunk. For a typical 100 KB file at 4 KB chunks, that adds ~250 ms of dead time per file. Over hundreds of files, this materially extends total upload duration and radio-on time.

A better approach is the adaptive chunk-size idea (§4.2) or `docs/19`'s existing suggestion to eliminate the two-pass hashing (which removes an entire file read, a far larger win than inter-chunk pausing).

**Alternative worth considering:** `esp_task_wdt_reset()` + `taskYIELD()` instead of `vTaskDelay(1)`. `taskYIELD()` gives up the CPU for zero ticks (returns immediately if no higher-priority task is ready), which allows the WebServer/SSE/watchdog tasks to run without imposing a mandatory 10 ms stall.

### C. Disable WiFi TX AMPDU

**Revised verdict: Experimental only (Tier 4)**

Both reviewers agree this should not be a default recommendation. The throughput and retry implications are unpredictable without measurement.

### D. Raise Brownout Detector Threshold

**Revised verdict: Recommended as default (Tier 1)**

See §2.3 for full reasoning. Level 7 (~2.7V) provides SD card FTL protection with adequate margin below the 3.3V operating rail.

### E. Reduce Internal SPI Flash Speed

**Revised verdict: Remove — already at 40 MHz**

See §1.4.

---

## Part 6: The Two-Pass Cloud Hashing Opportunity Is Still Open

Both review documents (`docs/20` and `docs/21`) did not comment on `docs/19` Part 8 item #1: the two-pass Cloud upload problem.

**This opportunity is confirmed still present in the codebase.**

Current flow in `SleepHQUploader.cpp`:

1. `computeContentHash()` — opens file, reads entire contents, computes MD5, closes file
2. `httpMultipartUpload()` — opens file again, reads entire contents while streaming over TLS, computes MD5 on the fly (again)

The SleepHQ API accepts `content_hash` in the multipart **footer** (after file data). So the pre-computation in step 1 is unnecessary — the on-the-fly MD5 from step 2 can serve both as the `content_hash` and as the integrity checksum.

Eliminating step 1 would:

- **Remove one full file read per Cloud upload** (~50% reduction in SD card active time per file)
- **Remove one full MD5 pass per file** (CPU savings)
- **Shorten the SD-mounted window** during the most electrically vulnerable phase

This remains one of the highest-value single optimizations available and should stay at Tier 1.

---

## Part 7: `listen_interval` Is Still Not Implemented

Both review documents mention this but neither emphasizes how straightforward it is.

**Confirmed: no `esp_wifi_set_config()` or `wifi_config_t` usage exists anywhere in the source.**

The current `WiFiManager::applyPowerSettings()` calls `WiFi.setSleep(WIFI_PS_MAX_MODEM)` but does not set `listen_interval`. Without it, MAX_MODEM defaults to waking on every DTIM beacon (typically every 100-300 ms), providing minimal benefit over MIN_MODEM.

Implementation is ~10 lines in `WiFiManager.cpp`:

```cpp
wifi_config_t wifi_cfg;
esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
wifi_cfg.sta.listen_interval = 10;  // Wake every 10th DTIM
esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
```

This should be paired with the existing `WIFI_PS_MAX_MODEM` call. The value (10) could be made configurable or hardcoded — even a value of 3-5 would provide meaningful savings.

**Tier 1 — confirmed, still open, low effort, high impact on idle power.**

---

## Part 8: Final Consolidated Action List

This merges the best ideas from all three documents (`docs/19`, `docs/20`, `docs/21`) into a single prioritized list, with my assessment of each.

### Tier 1: Implement First (High confidence, low risk)

| # | Item | Source | Effort | Notes |
|---|---|---|---|---|
| 1 | ✅ **Switch SD to 1-bit mode at 5 MHz** | docs/19 | Trivial | Single largest peak-current reduction, 1-line change |
| 2 | ✅ **Implement `listen_interval` for MAX_MODEM** | docs/19 | Low (~10 lines) | Idle power reduction during IDLE/COOLDOWN, still unimplemented |
| 3 | ✅ **Eliminate two-pass Cloud hashing** | docs/19 | Medium (~50 lines) | ~50% SD active time reduction per Cloud file, still unimplemented |
| 4 | ✅ **Raise BOD threshold to level 7** | docs/20 | Trivial (sdkconfig) | SD card FTL protection, should be default |
| 5 | ✅ **Brownout-recovery degraded boot** | docs/21 | Low (~30-40 lines) | Best new idea — reactive, preserves reachability, zero cost on stable hardware |
| 6 | ✅ **Timed mDNS (60s then stop)** | docs/19 | Low (~15 lines) | Eliminates multicast wakes, safe for Brownout-Safe profile |

### Tier 2: Implement Second (Good value, may need design decisions)

| # | Item | Source | Effort | Notes |
|---|---|---|---|---|
| 7 | ✅ **Reduce GPIO drive strength on SD pins** | docs/20 | Trivial (3 lines) | Pair with 1-bit/5MHz, zero throughput cost |
| 8 | ✅ **Close SSE during uploads** | docs/19/21 | Low (~5 lines) | Eliminates continuous WiFi TX during upload phase |
| 9 | ✅ **Defer LittleFS log flushes during uploads** | docs/21 | Low (~5 lines) | Eliminates internal flash writes during peak-current phase |
| 10 | ✅ **Reduce boot/NTP/connect delays** | docs/19 | Low | 15s→8s stabilization, 5s→2s NTP, conservative margins |
| 11 | ✅ **Remove periodic 3s status snapshot rebuild** | docs/19/21 | Trivial | On-demand rebuild already exists in API handler |

### Tier 3: Design-Heavy or Opt-In

| # | Item | Source | Effort | Notes |
|---|---|---|---|---|
| 12 | **WiFi-off + 40 MHz LISTENING** (opt-in) | docs/19 | Medium-High | Largest daily energy reduction, but WebServer unreachable — strictly opt-in |
| 13 | ✅ **ULP coprocessor for CS_SENSE** | docs/19 | High | Only path to sub-5mA LISTENING, implemented via macro-based ULP program |
| 14 | **OTA on-demand via GUI button** | docs/19 | Low | Security/UX improvement, minimal power impact |
| 15 | ✅ **Conditional TrafficMonitor buffer** | docs/19 | Low | ~2.4KB heap savings, useful for fragmentation |

### Tier 4: Experimental / Measurement-Required

| # | Item | Source | Effort | Notes |
|---|---|---|---|---|
| 16 | **Disable TX AMPDU** | docs/20 | Trivial (sdkconfig) | Uncertain benefit, potential throughput/retry harm |
| 17 | ✅ **Adaptive chunk sizes** | docs/21 | Low | Heap-adaptive: 4KB/2KB/1KB based on max_alloc thresholds |
| 18 | ✅ **`taskYIELD()` pacing** | This doc | Trivial | Added to both SleepHQ and SMB upload loops |
| 19 | **lwIP TCP window tuning** | docs/19 | Medium | High risk of destabilizing concurrent workloads |

### Removed

| Item | Source | Reason |
|---|---|---|
| Reduce internal SPI flash speed | docs/20 | Already at 40 MHz on pico32 |
| `vTaskDelay(1)` per chunk | docs/20 | 10ms stall at 100Hz tick is too aggressive; replaced by #18 |

---

## Part 9: Where the Reviewers Disagree and My Position

| Topic | docs/20 (G31) | docs/21 (C54) | My Position (CO46) |
|---|---|---|---|
| **Overall accuracy of docs/19** | "Technically correct and highly accurate" | "Mostly strong, needs updates" | Closer to docs/21 — mostly correct but needs the specific corrections listed in Part 1 |
| **GPIO drive strength** | High Impact | Experimental | **Low-risk secondary** — between the two, pair with 1-bit/5MHz |
| **Timed mDNS tier** | Recommended for Brownout-Safe default | Opt-in / advanced only | **Safe for Brownout-Safe profile**, not opt-in-only |
| **BOD threshold** | Recommended default | Opt-in protection only | **Default** — protecting SD FTL is more important than avoiding a few extra resets |
| **AMPDU disable** | Medium impact, low effort | Experimental only | **Experimental only** — agree with docs/21 |
| **Micro-yielding** | Recommended | Too blunt, replace with adaptive chunks | **Replace with `taskYIELD()` or adaptive chunks** — agree with docs/21's critique but propose a different alternative |
| **Flash speed reduction** | Recommended | Invalid (already 40 MHz) | **Invalid** — agree with docs/21 |
| **Two-pass hashing** | Not mentioned | Not mentioned | **Still open, Tier 1** — both reviewers missed it |
| **Brownout-recovery boot** | Not mentioned | Best new idea | **Agree — best new idea** |
| **ULP ranking** | Not mentioned | Position #13 (experimental) | **Tier 3 (design-heavy), not Tier 4 (experimental)** — high effort but architecturally unique |

---

## Final Summary

### What `docs/19` got right and should keep
- SD 1-bit / 5 MHz as the single easiest win
- `listen_interval` as still unimplemented
- Two-pass hashing elimination as a high-value optimization
- ULP feasibility analysis
- WiFi-off / 40 MHz as opt-in only

### What `docs/19` should correct
- Label proposed config keys as "PROPOSED — not yet implemented"
- Remove the "WiFi connected + sub-80 MHz" intermediate variant claim
- Soften the timed-mDNS backward-compatibility claim
- Narrow the status-snapshot recommendation (on-demand already exists)

### What `docs/20` got right
- WebServer reachability as a hard constraint
- GPIO drive strength as technically feasible
- BOD threshold increase as valuable (though miscategorized as power saving)

### What `docs/20` should correct
- Remove flash-speed suggestion (already 40 MHz)
- Demote AMPDU disable to experimental
- Replace `vTaskDelay(1)` with a less aggressive pacing strategy
- Reframe BOD as data-integrity protection, not power optimization

### What `docs/21` got right
- Best new idea: brownout-recovery degraded boot
- Deferring LittleFS flushes during uploads
- The general "shed heavy services during uploads" direction (scoped to SSE)
- Correct identification of the flash-speed error in docs/20

### What `docs/21` should correct
- Timed mDNS is safe for a Brownout-Safe profile, not "opt-in / advanced only"
- GPIO drive strength is low-risk secondary, not "experimental"
- ULP belongs in Tier 3 (design-heavy), not Tier 4 (experimental)
- BOD threshold should be default, not opt-in
- "Upload-phase service shedding" should be scoped to SSE, not broad endpoint blocking
- Two-pass hashing elimination is still open and should be in the ranking

---

## Sources Verified During This Review

- `src/main.cpp` — brownout detection (line 250), log flush interval (line 121), PM config (line 473), SSE push (line 963), status snapshot (line 967), upload task flag (line 72)
- `src/SleepHQUploader.cpp` — `computeContentHash()` still present (line 390), two-pass flow confirmed (line 466 calls hash, then upload streams again)
- `src/WiFiManager.cpp` — `applyPowerSettings()` calls `WiFi.setSleep()` without `listen_interval` (line 394), no `esp_wifi_set_config` anywhere in source
- `src/CpapWebServer.cpp` — `handleApiStatus()` already calls `updateStatusSnapshot()` (line 632), SSE state management (lines 593-610), `isUploadInProgress()` checks `uploadTaskRunning` (line 259)
- `src/Logger.cpp` — `dumpSavedLogsPeriodic()` (line 538), `flushBeforeReboot()` (line 742)
- `/root/.platformio/platforms/espressif32/boards/pico32.json` — `f_flash = 40000000L` confirmed
- `/root/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32/include/driver/include/driver/gpio.h` — `gpio_set_drive_capability()` confirmed
- `/root/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32/include/hal/include/hal/gpio_types.h` — `GPIO_DRIVE_CAP_0..3` confirmed
- ESP32 forum community measurements (brownout threshold levels 0-7)
