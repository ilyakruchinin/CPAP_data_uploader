# CPAP AutoSync v3.0i-gamma1 — Changelog

## ⚠️ Critical Upgrade Notice
**USB flash required** — OTA updates from v1.x are **not supported**. The framework, partition table, and SDK have changed. [Follow the flashing guide in the README / Web Flasher].

**Upgrading from v2.0i-alpha or v2.0i-beta?** OTA update is supported (**_but discouraged_**) — use the OTA tab in the web interface.

---

## 🚀 What's New in v3.0i-gamma1

This release brings massive improvements to memory efficiency, upload speeds, and the core SD card polling logic to prevent CPAP "SD Card Errors."

### Memory & Performance Optimizations 
- **Custom FATFS Compilation:** Discovered that SD DMA buffers were severely fragmenting the heap during mounts. By compiling a custom `libfatfs.a` with `CONFIG_FATFS_SECTOR_512=y` (down from 4096 bytes) and limiting `max_files=2`, we recovered **~18KB of contiguous heap** during SD access.
- **TLS Pre-Warming Removed:** The "pre-warming" hack was completely removed. Because the heap is now naturally contiguous enough, mbedTLS can reliably allocate its 36KB handshake buffer anywhere in the lifecycle.
- **300% Faster SMB Uploads:** Thanks to the newfound memory headroom, the dynamic SMB upload buffer was quadrupled from 2KB to **8KB**, massively increasing local transfer speeds.
- **Tuned Memory Warnings:** Lowered low-memory warning thresholds from 50KB to 20KB across all uploaders to prevent false-positive log spam.

### Intelligent SD Polling (Anti-SD Card Error)
- **Early Suppression:** Fixed a major issue where the ESP32 would aggressively steal the SD card every 62 seconds during active CPAP therapy, causing the CPAP to buffer excessively and eventually throw an "SD Card Error". Now, once an upload succeeds or finds nothing to do, the device suppresses all further SD probing until *new* physical CPAP activity is detected.
- **Smart Mode Schedule Edge-Trigger:** Fixed a bug where Smart Mode could become permanently paralyzed in a suppressed state. The FSM now edge-triggers to instantly clear suppression the moment your daily schedule window opens, ensuring historical backlog folders are never missed.

### Bulletproof PCNT Activity Detection
- **Hardware Drain Fix:** Eliminated "false positive" activity loops caused by the ESP32 releasing the SD card bus. The physical PCNT hardware accumulator is now synchronously drained whenever the ESP releases control back to the CPAP.
- **Cooldown Activity Pipeline:** Fixed a critical blindspot where CPAP bus activity was completely missed during the 1-minute `COOLDOWN` state because the PCNT hardware was being suspended. The hardware now stays awake, and an `_activityLatch` safely captures and holds any transitional CPAP activity until the FSM can evaluate it.

---

## What's Fixed (v2 cumulative)

### WiFi Reason 208 (PMF Compatibility) — beta2
- **Problem:** WiFi 6 routers with WPA3-transitional mode caused immediate connection failures with reason 208 (`ASSOC_COMEBACK_TIME_TOO_LONG`)
- **Root cause:** ESP-IDF 5.x sets PMF (Protected Management Frames / 802.11w) capable by default; some routers send an association comeback time exceeding the ESP-IDF threshold
- **Fix:** Automatic PMF fallback retry — if first attempt fails with reason 208, firmware disables PMF and reconnects. Users with compatible routers are unaffected
- **Logs:** Added human-readable labels for ESP-IDF 5.x-specific disconnect reason codes (206–209)

---

## What's New (Cumulative: alpha1–beta2)

### Platform Upgrade (alpha1)
- Migrated to **pioarduino** (Arduino 3.3.x / ESP-IDF 5.5.2) with hybrid compile
- Dynamic Frequency Scaling, Bluetooth removal (~30KB saved), mbedTLS tuning
- Coordinated WiFi cycling to prevent connection storms

### Multi-Tab Detection & SSE Ownership (alpha4–5, beta1)
- **Per-tab + per-instance IDs** for reliable detection across tabs, browsers, and devices
- **Three-layer detection**: BroadcastChannel, server-side recent-tab sightings, SSE single-owner lease
- **Throttling**: Polling slows 3s→15s, SSE stops, heavy log fetches blocked when duplicates detected; auto-resumes when cleared (~15–30s decay)
- **Refresh-safe**: Page refresh no longer triggers false "multiple tabs" warning

### Upload-Safe Log Loading (alpha4–5)
- `/api/logs/full` serves only circular buffer (~16KB) during uploads — prevents WDT crashes from TCP buffer starvation
- Reduced payload: latest rotation file only (~48KB total vs ~140KB). Full history via "Download All Logs"

### Watchdog Hardening (alpha4–5)
- WDT feeding before/after TLS handshakes (can take 14+ seconds at low heap)
- **SO_SNDTIMEO 10s** socket send timeout prevents indefinite blocking
- Per-write WDT feeds between all TLS writes (headers, preamble, footer, flush)

### Configurable Log Flushing During Uploads (beta1)
- New config option: `FLUSH_LOGS_DURING_UPLOAD = true` enables continuous 10-second log flushing even during active uploads
- Default (`false`) retains existing power-safe behaviour — no action needed
- Eliminates `LOG NOTICE` gaps in persisted syslogs for users who want complete upload-time logging

### Friendlier Connection & Log Messages (beta1)
- Reconnect overlay rewritten: "Connection lost — reconnecting…" with calmer explanation covering phone sleep, temporary busy, and restarts
- **5-second fetch timeout** on all API requests — offline detection now takes ~10–15s instead of ~90s on WiFi loss
- Mobile wake-up handling: page no longer flashes the disconnection overlay when a sleeping tab resumes
- Log gap marker reworded from `LOG GAP` / "bytes lost" to `LOG NOTICE` / "lines skipped" with config hint

### Reliability Fixes (alpha2–3, beta1)
- Cloud: Fixed duplicate Content-Length headers, OTA network error display
- SMB: Fixed ~32KB boundary failures with TCP drain, PDU allocation error detection, unconditional TLS buffer cleanup before SMB phase
- System: Fixed PCNT blocking manual uploads, stale System tab graphs
- OTA: Fixed hardcoded partition size — now queries actual OTA partition at runtime

<a name="flashing-guide"></a>
## 🔄 How to Flash (Web Flasher)

**For users upgrading from earlier releases or new installations:**

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

**For users upgrading from any v2.0i-alpha or v2.0i-beta1:**

1. Open the web interface at `http://cpap.local` (or `http://<device-ip>/`)
2. Go to the **OTA** tab
3. Upload `firmware-ota-upgrade.bin`
4. Wait for the device to restart (~30 seconds)

**🚨 CRITICAL: After a full USB flash (not OTA), BEFORE inserting the ESP32 into your CPAP machine:**

You **must** update `config.txt` on your SD card with your actual WiFi credentials. The **Erase** step wipes all previously stored passwords from the ESP32's flash memory (including any credentials that were migrated to NVS in previous versions). If you skip this, the device will fail to connect with "SSID is empty" or authentication errors.

**Important:** Use `firmware-ota.bin` (not `firmware-ota-upgrade.bin`) when doing a full reflash. The web flasher performs a complete erase automatically.
