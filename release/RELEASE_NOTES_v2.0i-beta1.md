# CPAP AutoSync v2.0i-beta1

**Cumulative release — includes all changes from alpha1 through alpha5 plus new beta1 features.**
OTA update from any v2.0i-alpha is supported — no USB flash required.

> ⚠️ **Upgrading from v1.x?** USB flash required — OTA updates from v1.x are **not supported**. The framework, partition table, and SDK have all changed. See [How to Flash](#-how-to-flash-web-flasher) below.

---

## What's New in beta1

### Configurable Log Flushing During Uploads

**Problem:** By default, periodic log flushes to internal flash are paused while an upload is running. This prevents SPI flash writes from overlapping with SD reads and TLS traffic — the safest setting for power-constrained setups. However, it means that if the device reboots unexpectedly during a long upload, the persisted syslog may show a `LOG NOTICE` gap where some log lines were skipped.

**New option:** `FLUSH_LOGS_DURING_UPLOAD = true` in `config.txt` enables continuous 10-second log flushing even during active uploads. This eliminates log gaps entirely at the cost of slightly higher power draw and flash wear during upload sessions.

- Default behaviour (`false`) is unchanged — no action needed for existing users
- When enabled, logs flush every 10 seconds during uploads, just like during all other FSM states
- The `LOG NOTICE` marker in persisted syslogs now includes instructions for enabling this option

### Friendlier Connection & Log Messages

**Reconnect overlay:** The "Device is unreachable or rebooting" banner has been replaced with a calmer "Connection lost — reconnecting…" message. The new text explains common causes (phone/browser pausing the page, device temporarily busy, restart) and reassures that recovery is usually automatic.

**Mobile wake-up handling:** When a phone wakes a sleeping browser tab, the page no longer immediately shows the disconnection overlay. Instead, it resets the failure counter, fires an immediate status poll, and restarts the poll timer — giving the device a few seconds to respond before declaring it offline.

**Faster offline detection:** Added a 5-second timeout (`AbortController`) to all API requests. Previously, `fetch()` had no timeout, so when WiFi was off each request could hang for 30–60 seconds before failing. Combined with a reduced failure threshold (3 consecutive failures instead of 5), the overlay now appears within ~10–15 seconds instead of ~90 seconds.

**Log gap wording:** The persisted syslog marker formerly labelled `LOG GAP` is now `LOG NOTICE` with friendlier language — "some detailed log lines were skipped" instead of "bytes lost" / "buffer overflow".

### Multi-Tab Detection Improvements

**False positive on refresh fixed:** Pull-to-refresh (or F5) on mobile could briefly trigger a false "Multiple tabs/browsers detected" warning. Root cause: during a page refresh, the old and new page contexts overlap momentarily on the BroadcastChannel with different instance IDs but the same tab ID. The peer tracker now ignores peers with the same tab ID, eliminating the false positive while preserving real duplicate-tab detection.

### OTA Partition Size Fix

**Problem:** OTA uploads could fail with a 500 error on devices with non-standard partition layouts because the firmware assumed a hardcoded 1.5 MB OTA partition size.

**Fix:** The firmware now queries `esp_ota_get_next_update_partition()` at runtime to determine the actual partition size. This is used for both validation and as the default size for chunked uploads.

---

## Cumulative Changes (alpha1–alpha5)

### Platform Upgrade (alpha1)

- Migrated to **pioarduino** (Arduino 3.3.x / ESP-IDF 5.5.2) with hybrid compile
- Dynamic Frequency Scaling at 160 MHz (80–160 MHz range), locked at 80 MHz default
- Bluetooth removed at compile time (~30 KB saved)
- mbedTLS tuned: hardware AES-128-GCM only, ChaCha20 and AES-256 disabled
- 802.11b disabled (OFDM only) — eliminates 370 mA peak TX scenario
- Auto light-sleep enabled (CPU sleeps between WiFi DTIM intervals in IDLE/COOLDOWN)
- Coordinated WiFi cycling to prevent connection storms during uploads
- Renamed `STORE_CREDENTIALS_PLAIN_TEXT` → `MASK_CREDENTIALS` (inverted semantics; default: plaintext)

### Multi-Tab Detection & SSE Ownership (alpha4–5, beta1)

- **Per-tab 4-hex-character IDs** stored in `sessionStorage`, attached to all API requests
- **Per-instance 8-hex-character IDs** generated per page load to distinguish tabs across browser restarts
- **Three-layer detection**: BroadcastChannel (same-browser), server-side recent-tab sightings via `/api/status`, and SSE single-owner lease
- **Throttling**: Status polling slows 3s → 15s, SSE stops, heavy log fetches blocked when duplicates detected; auto-resumes when cleared (~15–30s decay)
- **SSE single-owner lease**: Only one tab at a time can hold the live log stream; competing tabs get a 409 response
- **Refresh-safe**: Same-tid peers filtered in BroadcastChannel to prevent false positives on page refresh

### Upload-Safe Log Loading (alpha4–5)

- **Server-side guard**: `/api/logs/full` serves only circular buffer (~16 KB) during active uploads — prevents WDT crashes from TCP buffer starvation
- **Client-side awareness**: Logs tab detects upload state and skips heavy NAND log fetch; falls back to circular buffer + SSE live streaming
- **Multi-tab blocks backfill**: Heavy log fetches refused when duplicate tabs detected
- **Reduced payload**: Latest rotation file only (~48 KB total vs ~140 KB); full history via "Download All Logs"
- **Auto-resume**: Deferred backfill triggers automatically when upload finishes or multi-tab contention clears

### Watchdog Hardening (alpha4–5)

- WDT feeding before/after TLS handshakes (can take 14+ seconds at low heap)
- **SO_SNDTIMEO 10s** socket send timeout prevents indefinite blocking
- Per-write WDT feeds between all TLS writes (headers, preamble, footer, flush)
- WDT feed in WiFi reconnection wait loop

### Reliability Fixes (alpha2–3)

- Cloud: Fixed duplicate Content-Length headers in OTA upload response
- Cloud: OTA network error treated as success when upload reaches 100% (ESP restarts before client receives response)
- SMB: Fixed ~32 KB boundary failures with TCP drain, PDU allocation error detection
- SMB: Unconditional TLS buffer cleanup before SMB phase
- System: Fixed PCNT re-check blocking manual (web UI-triggered) uploads — force-triggered uploads now skip silence re-check
- System: Fixed stale System tab graphs and status badge sticking on "REBOOTING"

### Brownout & Power Management (alpha1)

- `BROWNOUT_DETECT = RELAXED` mode: temporarily disables brownout detector during WiFi reconnection only, re-enables afterwards
- `BROWNOUT_DETECT = OFF` mode: permanent disable with persistent web UI warning banner
- GPIO drive strength reduced on SD pins for lower di/dt spikes
- 4-bit SDIO remount handoff for reliable CPAP SD card re-enumeration

### Web UI Polish (alpha1–beta1)

- Redesigned header: "CPAP AutoSync" branding with animated SVG logo
- Live heap/CPU charts on System tab
- SSE throttled (not paused) during uploads — live logs remain near-real-time
- Immediate RAM-buffer catch-up when returning to Logs tab
- Connection overlay: calmer wording, mobile wake-up handling, faster offline detection
- 5-second fetch timeout on all API requests (prevents hung UI on WiFi loss)

---

<a name="-how-to-flash-web-flasher"></a>
## 🔄 How to Flash (Web Flasher)

**For users upgrading from v1.x or new installations:**

1. **Prepare the hardware:**
   - Connect the SD WIFI PRO to the development board with switches set to:
     - Switch 1: OFF
     - Switch 2: ON

2. **Use a desktop Chromium-based browser** (Chrome, Edge, or Opera — Firefox/Safari not supported)

3. **Steps:**
   - Extract the release ZIP to a folder on your computer
   - Open https://esptool.spacehuhn.com/ in Chrome, Edge, or Opera
   - Connect the ESP32 board by USB
   - Click **Connect** and choose the serial port:
     - **Windows:** `USB Serial (COM5)`, `USB-SERIAL CH340 (COMx)`, or `Silicon Labs CP210x USB to UART Bridge (COMx)`
     - **macOS:** `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART`
     - **Linux:** `/dev/ttyUSB0` or `/dev/ttyACM0`
     - *Tip: If unsure, click Connect first, then plug in the board — the new port is usually the right one*
   - Delete any existing rows if needed, then click **Add** once
   - Make sure the address is **`0x0`**
   - Select **`firmware-ota.bin`** (the complete image for first-time flashing)
   - **⚠️ DO NOT SKIP THIS STEP:** Click **Erase** — this is mandatory for clean state
   - Click **Program**

**For users upgrading from any v2.0i-alpha:**

1. Open the web interface at `http://cpap.local` (or `http://<device-ip>/`)
2. Go to the **OTA** tab
3. Upload `firmware-ota-upgrade.bin`
4. Wait for the device to restart (~30 seconds)

**🚨 CRITICAL: After a full USB flash (not OTA), BEFORE inserting the ESP32 into your CPAP machine:**

You **must** update `config.txt` on your SD card with your actual WiFi credentials. The **Erase** step wipes all previously stored passwords from the ESP32's flash memory (including any credentials that were migrated to NVS in previous versions). If you skip this, the device will fail to connect with "SSID is empty" or authentication errors.

**Important:** Use `firmware-ota.bin` (not `firmware-ota-upgrade.bin`) when doing a full reflash. The web flasher performs a complete erase automatically.

---

## Files Changed (beta1 only)

- `include/Config.h` — Added `flushLogsDuringUpload` field and getter
- `src/Config.cpp` — Constructor default, `FLUSH_LOGS_DURING_UPLOAD` key parsing, getter
- `src/main.cpp` — Log flush guard respects `FLUSH_LOGS_DURING_UPLOAD` config
- `src/Logger.cpp` — Updated LOG NOTICE text with config hint
- `include/web_ui.h` — Reconnect overlay text, 5s fetch timeout, mobile wake-up handling, same-tid peer filter, reduced failure threshold
- `src/OTAManager.cpp` — Runtime OTA partition size query
- `src/CpapWebServer.cpp` — Tab ID/instance tracking, SSE single-owner lease, recent-tab sightings
- `docs/CONFIG_REFERENCE.md` — Added `FLUSH_LOGS_DURING_UPLOAD` documentation
