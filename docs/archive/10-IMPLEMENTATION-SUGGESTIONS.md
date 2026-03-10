# Implementation Notes for v0.11.1

Date: 2026-03-01
Based on: GitHub Issues [#29](https://github.com/amanuense/CPAP_data_uploader/issues/29), [#42](https://github.com/amanuense/CPAP_data_uploader/issues/42), power research (`docs/06-*`, `docs/07-*`), and previous reviews (`docs/08-*`, `docs/09-*`).

This document presents independently validated findings and documents the v0.11.1 implementation, with the overriding design principle: **minimise power draw at all costs by default**, with config overrides for users on less constrained hardware.

> **Status:** All items in Priority 1–4 have been implemented and verified (build passes).

---

## 1. Validation of Previous Reviews

### 1.1 doc-08 (POST-FEEDBACK-REVIEW) — Largely Correct

| Finding | Verdict |
|:--------|:--------|
| WiFi race condition (Core 0 vs Core 1) | **CONFIRMED.** Log evidence is clear. `tryCoordinatedWifiCycle()` on Core 0 and `connectStation()` on Core 1 fight over WiFi state. This is the highest-priority stability fix. |
| DFS hardcoding (80–160 MHz) | **CONFIRMED.** `main.cpp:400-404` hardcodes `max_freq_mhz = 160`, independent of `CPU_SPEED_MHZ`. The user cannot disable DFS via config. |
| `GMT_OFFSET` key not recognised | **CONFIRMED.** `Config.cpp:191` only parses `GMT_OFFSET_HOURS`. The `GMT_OFFSET` key is silently ignored. |
| CS_SENSE GPIO 33 + PCNT correct | **CONFIRMED.** GPIO 33 matches the board schematic. Not a concern. |
| Recommendation to re-introduce `WiFi.setSleep(WIFI_PS_NONE)` during uploads | **REJECTED.** See §1.2 below. |
| Recommendation to throttle web server during uploads | **NOT RECOMMENDED.** The user's observation that closing the browser helps is better explained by the WiFi race condition. Fixing that race should resolve the web UI interference. Keep existing 3s polling. |

### 1.2 doc-09 (ANOTHER-REVIEW) — Partially Correct, Some Overstatements

| Finding | Verdict |
|:--------|:--------|
| Reporter tested v0.11.0 (not just v0.9.2) | **CORRECT.** The reporter confirmed testing v0.11.0 with no new logs. The v0.9.2 logs were from an earlier test. However, reports have been inconsistent (config errors, copy/paste issues), so we should not over-index on this specific failure mode. |
| "DFS Boost is the Smoking Gun" for SMB vs CLOUD | **OVERSTATED.** ESP32 has **hardware AES and SHA accelerators**. TLS bulk encryption runs on the HW engine at APB clock rate (80 MHz), NOT CPU clock. The CPU overhead of TLS is primarily mbedtls protocol framing and buffer management — significant but not "100% CPU pinned at 160 MHz". DFS transitions are a contributing factor but not the sole cause. See §2 for a more nuanced analysis. |
| `WiFi.setSleep(WIFI_PS_NONE)` during CLOUD uploads | **REJECTED.** This increases average power draw from ~22 mA to ~120 mA. That is a 5× increase. AP compatibility with MIN_MODEM is an AP configuration issue (enable UAPSD), not a firmware problem. |
| Contiguous heap exhaustion for TLS | **CORRECT.** TLS needs ~40–50 KB contiguous for handshake. Heap fragmentation over a session can prevent reconnection. |

### 1.3 doc-07 (POWER-REDUCTION-SUMMARY) — Good Foundation

The Phase 1 and Phase 2 recommendations were implemented correctly in v0.11.0 with two exceptions:
1. DFS `max_freq_mhz` was hardcoded to 160 instead of being tied to `CPU_SPEED_MHZ`
2. Bluetooth disable at compile time is ineffective in the Arduino framework (see §5)

**Phase 3** (Auto Light-sleep + 5 dBm TX) was deferred and should now be implemented. See §6 and §7.

---

## 2. Why SMB Works but CLOUD Fails on Singapore AS11

This is the central question. Both protocols use the same WiFi radio at the same TX power. The difference is **not** a single smoking gun but a combination of factors that compound on the weak Singapore AS11 power supply.

### 2.1 Comparison Table

| Aspect | SMB | CLOUD |
|:-------|:----|:------|
| **Transport** | Plaintext TCP (port 445) | TLS 1.2 over TCP (port 443) |
| **CPU load per chunk** | Minimal — `memcpy` to lwIP buffer | Moderate — mbedtls record framing, HW AES/SHA setup, DMA management |
| **Heap usage** | ~8 KB fixed buffer | ~40–50 KB for TLS context + 4 KB upload buffer |
| **Handshake cost** | SMB negotiate + session setup (~2 KB, ~200 ms) | TLS handshake (~300–400 ms at 80 MHz, heavy RSA/ECC math in software) |
| **Connections per session** | 1 persistent per folder | Multiple: OAuth, team discovery, import create, N file uploads, import process — each may require reconnection |
| **Retry behaviour on failure** | `recoverWiFiAfterSmbTransportFailure()` — contained, single-core | `tryCoordinatedWifiCycle()` — races with main loop on Core 1 |
| **Total WiFi TX time** | Low — small protocol overhead | High — TLS record headers, certificate chains, JSON API payloads |
| **DFS interaction** | CPU mostly idle between SD reads → stays at 80 MHz | mbedtls processing keeps CPU busier → more frequent DFS boosts to 160 MHz |

### 2.2 Root Cause Ranking

**1. WiFi Race Condition (CRITICAL):** When a CLOUD upload fails mid-stream (which happens more often due to the factors below), `SleepHQUploader` calls `tryCoordinatedWifiCycle()` on Core 0. Simultaneously, the main loop on Core 1 detects `!isConnected()` and calls `connectStation()`. The two cores corrupt the WiFi/lwIP state machine. Because the Task WDT for Core 0 is disabled during uploads, the device can end up stranded offline for hours without rebooting — exactly matching the reported 6-hour offline event (Issue #29).

SMB does not trigger this because `SMBUploader` uses a more contained recovery path (`recoverWiFiAfterSmbTransportFailure()`) that doesn't race with the main loop as aggressively.

**2. DFS Frequency Transitions (HIGH):** With `max_freq_mhz = 160`, the DFS governor boosts the CPU whenever mbedtls is processing a TLS record. Each 80→160 MHz transition causes a PLL relock with a transient current spike. During a CLOUD session with many TLS records, these transitions are frequent. SMB generates far fewer transitions because its CPU load is minimal. On the Singapore AS11's weak 3.3V supply, cumulative transient spikes degrade WiFi RF stability.

**3. Heap Fragmentation (MEDIUM):** Each TLS handshake allocates ~40–50 KB, then releases it. Over multiple reconnections (auth, team discovery, import, file uploads), the heap fragments. When `max_alloc` drops below ~36 KB, TLS reconnection fails entirely. SMB's 8 KB buffer causes negligible fragmentation.

**4. Session Length and Complexity (MEDIUM):** A CLOUD session involves 4+ HTTPS requests before any file upload starts. Each request is a potential failure point. SMB opens one connection and streams files. More failure points = more recovery attempts = more WiFi cycling = more compound stress.

---

## 3. TLS Cipher Suite Optimisation

### 3.1 Current State

The firmware does **not** configure any cipher suite preference. `WiFiClientSecure` uses mbedtls defaults, which include ChaCha20-Poly1305. The server (`sleephq.com`) advertises **client cipher preference**, meaning the ESP32 chooses the cipher.

### 3.2 ESP32 Hardware Acceleration

The ESP32-PICO-D4 has dedicated hardware accelerators for:
- **AES** (128/256-bit, all modes including GCM) — runs at APB clock, independent of CPU frequency
- **SHA** (SHA-1, SHA-256, SHA-384, SHA-512) — hardware engine

It does **NOT** have hardware acceleration for:
- **ChaCha20** — pure software implementation, 3–5× slower than HW AES
- **RSA** — software-only (but server uses ECDSA certs, so RSA key exchange is not used)
- **ECC/ECDHE** — software-only (but x25519 is fast in software)

### 3.3 Cipher Ranking for ESP32 (Lowest CPU First)

Based on the server's supported ciphers and ESP32's hardware:

| Priority | Cipher | Why |
|:---------|:-------|:----|
| 1 (best) | `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256` | HW AES-128 + HW SHA-256 + GCM (single-pass auth+encrypt). ECDHE x25519 is fast. ECDSA matches server cert. |
| 2 | `TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256` | HW AES-128 + HW SHA-256. CBC requires separate HMAC pass (slightly more work than GCM). |
| 3 | `TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384` | HW AES-256 (more rounds than 128) + HW SHA-384. |
| 4 | `TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256` | Same bulk cipher but RSA key exchange is heavier than ECDSA verify. |
| **Avoid** | `TLS_ECDHE_*_WITH_CHACHA20_POLY1305_SHA256` | **No HW acceleration.** ChaCha20 is designed for CPUs *without* AES hardware. On ESP32 it is 3–5× slower than HW AES, generating more CPU load and more DFS transitions. |

### 3.4 Implementation

Disable ChaCha20 at compile time in `sdkconfig.defaults`:
```
CONFIG_MBEDTLS_CHACHA20_C=n
CONFIG_MBEDTLS_CHACHAPOLY_C=n
```

This forces mbedtls to only offer AES-based ciphers in the ClientHello, guaranteeing the server selects an AES-GCM suite (which uses the hardware accelerator). No code changes needed — purely a build configuration change.

Additionally, to prefer AES-128 over AES-256 (fewer rounds, less computation):
```
CONFIG_MBEDTLS_AES_256_C=n
```

This is more aggressive — it disables AES-256 entirely. Since sleephq.com supports AES-128-GCM, this is safe. AES-128 provides 128-bit security which is more than adequate. This reduces the TLS handshake and bulk encryption workload.

**Risk:** If sleephq.com ever drops AES-128 support (extremely unlikely), uploads would fail until we re-enable AES-256. The `CONFIG_MBEDTLS_AES_256_C=n` option should be considered optional/aggressive.

---

## 4. WiFi Race Condition Fix

### 4.1 The Problem

`SleepHQUploader.cpp` calls `tryCoordinatedWifiCycle()` from Core 0 when an upload fails. The main loop on Core 1 independently detects `!wifiManager.isConnected()` and calls `connectStation()`. Both paths call into the ESP-IDF WiFi driver concurrently without synchronisation, corrupting internal state.

### 4.2 The Fix

In the main loop (`main.cpp`), guard the WiFi reconnection path:
```cpp
if (!wifiManager.isConnected() && !uploadTaskRunning) {
    // Only attempt reconnection when upload task is NOT active
    ...
}
```

The upload task already manages its own WiFi recovery. The main loop must yield WiFi control entirely when an upload is in progress.

---

## 5. Bluetooth Disable — Implementation Validation

### 5.1 Finding

The firmware map (`firmware.map`) reveals that **`libbtdm_app.a` is fully linked into the binary** — dozens of BT controller objects (lc_task, ld_acl, hci, dbg, etc.) are present. This happens because the Arduino-ESP32 framework uses **precompiled SDK libraries** that were built with BT enabled. Our `CONFIG_BT_ENABLED=n` in `sdkconfig.defaults` only affects our own code's conditional compilation — it does NOT prevent the framework's precompiled `libbt.a` from pulling in the entire BTDM controller binary blobs.

### 5.2 What Actually Works

The runtime call `esp_bt_controller_mem_release(ESP_BT_MODE_BTDM)` at line 172 of `main.cpp` **does** release the BT controller's reserved DRAM regions back to the heap. This is our only effective mechanism within the Arduino framework.

### 5.3 Why Heap Improvement Was Not Noticeable

The `esp_bt_controller_mem_release()` releases BSS/data segments marked with `__attribute__((section(".btdm_bss")))` and similar. The actual amount freed depends on the framework version but is typically ~28 KB. However:
- This memory may be fragmented across multiple DRAM regions
- It may not form a single large contiguous block
- The freed regions may not be adjacent to existing free heap blocks

### 5.4 Recommendation

Add diagnostic logging to verify the actual impact:
```cpp
uint32_t heapBefore = ESP.getFreeHeap();
uint32_t maxAllocBefore = ESP.getMaxAllocHeap();
esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
uint32_t heapAfter = ESP.getFreeHeap();
uint32_t maxAllocAfter = ESP.getMaxAllocHeap();
LOGF("BT memory release: free %u→%u (+%u), max_alloc %u→%u",
     heapBefore, heapAfter, heapAfter - heapBefore,
     maxAllocBefore, maxAllocAfter);
```

To achieve **true** BT elimination would require migrating from the Arduino framework to ESP-IDF native, where `CONFIG_BT_ENABLED=n` actually strips BT from the link. This is a large architectural change and not recommended for v0.11.1.

---

## 6. Default TX Power: Reduce to 5 dBm

### 6.1 Rationale

From `docs/07-POWER-REDUCTION-SUMMARY.md` §5.1:

| TX Power | Estimated Peak Current |
|:---------|:----------------------|
| 19.5 dBm (old default) | ~250 mA |
| 8.5 dBm (current default) | ~120–150 mA |
| **5.0 dBm (proposed default)** | **~100–120 mA** |

The device sits inside a CPAP machine in a bedroom, typically 1–10 metres from a WiFi router. 5 dBm (3.2 mW) is adequate for this scenario. The ~20–30 mA peak reduction is significant for the Singapore AS11's marginal power supply.

### 6.2 Implementation

Change the default in `Config.cpp`:
- `wifiTxPower` default: `POWER_MID` (8.5 dBm) → **`POWER_LOW` (5.0 dBm)**

Users with routers further away can override with `WIFI_TX_PWR = MID` (8.5 dBm) or `HIGH` (11 dBm).

### 6.3 WiFi Protocol — No Further Reduction Needed

Currently: `WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N` (802.11b disabled).

Disabling 802.11g (leaving only 802.11n) is technically possible but risky — many routers send management frames at 802.11g rates, and the ESP32's association may fail if 802.11g is not available for fallback. The 802.11b disable is the critical win (eliminates 370 mA peak). Going further provides marginal benefit with meaningful risk.

No additional protocol restrictions recommended.

---

## 7. Auto Light-Sleep with GPIO Wakeup (Phase 3)

### 7.1 Impact

From Espressif measurements:
- Modem-sleep + DFS: ~20–25 mA average idle
- **Auto Light-sleep: ~2–3 mA average idle**

This is a **10× reduction** in idle power consumption.

### 7.2 Feasibility

- **GPIO 33 (CS_SENSE)** is an RTC GPIO (`RTC_GPIO8`), so it CAN trigger wakeup from light sleep.
- **PCNT** is clock-gated during light sleep. Counter value is preserved in the PCNT register, but new edges are not counted while asleep. However, GPIO wakeup fires before PCNT needs to count.
- **WiFi DTIM wakeup** is handled automatically by the ESP-IDF WiFi driver in auto light-sleep mode. mDNS and web server requests will still work (with slight latency increase).

### 7.3 Implementation Approach

1. Set `pm_config.light_sleep_enable = true` (currently `false` — Phase 3 placeholder already exists in `main.cpp:403`)
2. Configure GPIO 33 as a wakeup source: `gpio_wakeup_enable(GPIO_NUM_33, GPIO_INTR_ANY_EDGE)` + `esp_sleep_enable_gpio_wakeup()`
3. In **LISTENING** state, acquire a PM lock (`esp_pm_lock_acquire()`) to prevent light sleep while actively monitoring bus activity via PCNT
4. In **IDLE** and **COOLDOWN** states, release the PM lock — auto light-sleep engages between DTIM intervals

### 7.4 State Behaviour

| FSM State | Light Sleep | Rationale |
|:----------|:------------|:----------|
| IDLE | **Enabled** | Maximum power saving. GPIO wakeup on CS_SENSE detects CPAP activity. DTIM wakeup preserves WiFi. |
| LISTENING | **Disabled** (PM lock held) | PCNT must count edges continuously. CPU must stay awake. |
| ACQUIRING | **Disabled** | Active SD card operations. |
| UPLOADING | **Disabled** | Active network I/O. |
| RELEASING | **Disabled** | Active SD card operations. |
| COOLDOWN | **Enabled** | Similar to IDLE. |
| MONITORING | **Disabled** | Live data for web UI. |

### 7.5 Risk

- mDNS response latency increases slightly (up to one DTIM interval, typically 100–300 ms)
- Web UI page load may feel marginally slower on first request after sleep
- Both are acceptable for a device that primarily needs to upload data once per day

---

## 8. DFS and CPU Frequency

### 8.1 Lock CPU at 80 MHz by Default (No DFS)

With `CPU_SPEED_MHZ = 80` as default and `max_freq_mhz = min_freq_mhz = 80`:
- **Zero** frequency transitions → zero PLL relock transients
- **Lowest** steady-state CPU power draw
- TLS handshake takes ~2× longer at 80 MHz vs 160 MHz (~400 ms vs ~200 ms) — acceptable
- HW AES/SHA engines run at APB clock (80 MHz) regardless — no penalty for bulk encryption

### 8.2 Implementation (✅ Done)

`main.cpp` now uses the user's configured CPU speed for the DFS ceiling:
```cpp
int targetCpuMhz = config.getCpuSpeedMhz(); // Default 80
esp_pm_config_esp32_t pm_config = {
    .max_freq_mhz = targetCpuMhz,  // Was hardcoded to 160
    .min_freq_mhz = 80,
    .light_sleep_enable = true      // Phase 3 — now enabled
};
```

When `CPU_SPEED_MHZ = 80` (default), this sets `max = min = 80`, effectively disabling DFS.
When a user sets `CPU_SPEED_MHZ = 160`, DFS operates normally between 80–160 MHz.

### 8.3 Config Option for Non-Constrained Hardware

Users with Australian-made AS11 or AS10 can set:
```ini
CPU_SPEED_MHZ = 160
```
This re-enables DFS (80–160 MHz) for faster TLS handshakes.

---

## 9. FreeRTOS Tick Rate

### 9.1 With CPU Locked at 80 MHz (No DFS)

The primary reason for `CONFIG_FREERTOS_HZ=1000` was finer DFS idle-detection granularity. With DFS disabled (CPU locked at 80 MHz), this reason disappears.

At 80 MHz, each tick ISR costs more CPU time proportionally. Reducing to 100 Hz cuts ISR overhead by 90%.

No firmware operation requires sub-10 ms scheduling:
- **TrafficMonitor**: Hardware PCNT, 100 ms sample rate → 10 ticks at 100 Hz ✓
- **Upload retries**: 500 ms delays → 50 ticks ✓
- **Main loop delays**: 10–100 ms → 1–10 ticks ✓
- **WiFi driver**: Uses its own hardware timers, independent of FreeRTOS tick rate ✓

### 9.2 With Auto Light-Sleep Enabled

ESP-IDF's tickless idle mode (`CONFIG_FREERTOS_USE_TICKLESS_IDLE`) suppresses tick ISRs during idle, allowing longer sleep periods. With tickless idle, the configured tick rate is irrelevant during sleep — the system sleeps until the next scheduled wakeup.

### 9.3 Implementation (✅ Done)

`sdkconfig.defaults` now contains:
```
CONFIG_FREERTOS_HZ=100
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
```

---

## 10. GMT_OFFSET Config Key Alias (✅ Done)

`GMT_OFFSET` is now accepted as a backward-compatible alias for `GMT_OFFSET_HOURS` in `Config.cpp`:
```cpp
} else if (key == "GMT_OFFSET_HOURS" || key == "GMT_OFFSET") {
    gmtOffsetHours = value.toInt();
```

This prevents silent misconfiguration for users migrating from older config formats.

---

## 11. MIN_MODEM Power Saving — Keep As-Is

The user's position: "Issues with AP are a problem of AP configuration, not ESP firmware."

MIN_MODEM remains the default. Users experiencing disconnects on APs with UAPSD disabled should:
1. Enable UAPSD on their AP (preferred)
2. Or set `WIFI_PWR_SAVING = NONE` in config.txt (user's choice to increase power draw)

No firmware change needed for this.

---

## 12. Implementation Plan (Prioritised)

### Priority 1 — Critical Stability Fixes

| # | Item | File(s) | Status |
|:--|:-----|:--------|:-------|
| 1 | **Fix WiFi race condition** — guard main loop reconnection with `!uploadTaskRunning` | `main.cpp` | ✅ Done |
| 2 | **Fix DFS hardcoding** — use `config.getCpuSpeedMhz()` for `max_freq_mhz` | `main.cpp` | ✅ Done |
| 3 | **Add `GMT_OFFSET` alias** | `Config.cpp` | ✅ Done |

### Priority 2 — Power Reduction (Default Changes)

| # | Item | File(s) | Status |
|:--|:-----|:--------|:-------|
| 4 | **Default TX power to 5 dBm** (was 8.5 dBm) | `Config.cpp` | ✅ Done |
| 5 | **Reduce FreeRTOS tick to 100 Hz** (was 1000 Hz) | `sdkconfig.defaults` | ✅ Done |
| 6 | **Disable ChaCha20 in mbedtls** — force HW-accelerated AES ciphers only | `sdkconfig.defaults` | ✅ Done |
| 7 | **Add BT memory release diagnostic logging** | `main.cpp` | ✅ Done |

### Priority 3 — Auto Light-Sleep (Phase 3)

| # | Item | File(s) | Status |
|:--|:-----|:--------|:-------|
| 8 | **Enable auto light-sleep** — set `light_sleep_enable = true` | `main.cpp` | ✅ Done |
| 9 | **Configure GPIO 33 wakeup** — `gpio_wakeup_enable()` + `esp_sleep_enable_gpio_wakeup()` | `main.cpp` | ✅ Done |
| 10 | **Add PM lock for active FSM states** — prevent light sleep during bus monitoring, SD I/O, and uploads | `main.cpp` | ✅ Done |
| 11 | **Enable tickless idle** — `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` | `sdkconfig.defaults` | ✅ Done |

### Priority 4 — Aggressive TLS Optimization

| # | Item | File(s) | Status |
|:--|:-----|:--------|:-------|
| 12 | **Disable AES-256 in mbedtls** — force AES-128 only (lower computation) | `sdkconfig.defaults` | ✅ Done |

> **AES-256 Re-enablement:** If SleepHQ ever drops AES-128 support (extremely unlikely), TLS connections will fail. To restore AES-256, change `CONFIG_MBEDTLS_AES_256_C=n` to `CONFIG_MBEDTLS_AES_256_C=y` in `sdkconfig.defaults` and rebuild.

---

## 13. Expected Power Profile After Implementation

### Idle (WiFi Connected, No Upload)

| Configuration | Average Current |
|:-------------|:----------------|
| v0.10.x (240 MHz, no sleep, 19.5 dBm) | ~120–130 mA |
| v0.11.0 (80 MHz + DFS 160, MIN_MODEM, 8.5 dBm) | ~20–25 mA |
| **v0.11.1 (80 MHz locked, MIN_MODEM, 5 dBm)** | **~18–22 mA** |
| **v0.11.1 + light-sleep (80 MHz, auto light-sleep, 5 dBm)** | **~2–3 mA** |

### Peak (During WiFi TX)

| Configuration | Peak Current |
|:-------------|:-------------|
| v0.10.x (802.11b, 19.5 dBm) | **370 mA** |
| v0.11.0 (802.11n, 8.5 dBm) | ~120–150 mA |
| **v0.11.1 (802.11n, 5 dBm)** | **~100–120 mA** |

### DFS Transition Spikes

| Configuration | Transient Spikes |
|:-------------|:-----------------|
| v0.11.0 (DFS 80↔160 MHz) | Frequent PLL relock transients during TLS |
| **v0.11.1 (80 MHz locked, no DFS)** | **Zero** |
