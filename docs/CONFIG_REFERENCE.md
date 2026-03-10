# Configuration Reference

All settings are read from `/config.txt` on the SD card at boot. The file uses simple `KEY = VALUE` syntax. Lines starting with `#` are treated as comments. Values may be optionally wrapped in single or double quotes.

---

## 1. Network

| Key | Default | Description |
|---|---|---|
| `WIFI_SSID` | *(required)* | WiFi network name to connect to |
| `WIFI_PASSWORD` | *(empty)* | WiFi password. Supports all characters including `@`, `!`, `#`. After first successful boot the password is migrated to encrypted flash (NVS) and censored in the config file. |
| `HOSTNAME` | `cpap` | mDNS hostname. Device becomes reachable at `http://<hostname>.local`. |

---

## 2. Upload Destination (Endpoint)

| Key | Default | Description |
|---|---|---|
| `ENDPOINT` | *(required)* | Upload destination. SMB share: `//server/share`. Cloud: `https://sleephq.com` (or leave empty when `ENDPOINT_TYPE=CLOUD`). |
| `ENDPOINT_TYPE` | *(auto-detected)* | Comma-separated list of active backends: `SMB`, `CLOUD`, or `SMB,CLOUD`. If omitted, type is inferred from `ENDPOINT` value. |
| `ENDPOINT_USER` | *(empty)* | SMB username. |
| `ENDPOINT_PASSWORD` | *(empty)* | SMB password. Migrated to encrypted flash on first boot. |

---

## 3. SleepHQ Cloud Upload

Only required when `ENDPOINT_TYPE` includes `CLOUD`.

| Key | Default | Description |
|---|---|---|
| `CLOUD_CLIENT_ID` | *(required for cloud)* | OAuth2 client ID from SleepHQ developer settings. |
| `CLOUD_CLIENT_SECRET` | *(required for cloud)* | OAuth2 client secret. Migrated to encrypted flash on first boot. |
| `CLOUD_TEAM_ID` | *(auto-discovered)* | SleepHQ team ID. If omitted, auto-discovered via `GET /api/v1/me` on each upload session. Set explicitly to skip the discovery round-trip. |
| `CLOUD_DEVICE_ID` | `0` | SleepHQ device ID to associate imports with. `0` = let SleepHQ auto-assign. |
| `CLOUD_BASE_URL` | `https://sleephq.com` | SleepHQ API base URL. Only change if using a self-hosted instance. |
| `CLOUD_INSECURE_TLS` | `false` | Set to `true` to skip TLS certificate validation. **Not recommended for production.** |
| `MAX_DAYS` | `365` | Maximum number of past days to upload. DATALOG folders older than this are ignored. |
| `RECENT_FOLDER_DAYS` | `2` | Number of days considered "recent" (today + yesterday by default). Recent folders are re-scanned for changes on every upload cycle. |

---

## 4. Upload Schedule

| Key | Default | Description |
|---|---|---|
| `UPLOAD_MODE` | `smart` | Upload strategy. `smart` = continuous monitoring, upload whenever CPAP is idle. `scheduled` = only upload within the configured time window. |
| `UPLOAD_START_HOUR` | `9` | Start of upload window (0–23, local time). Ignored in smart mode for fresh data. |
| `UPLOAD_END_HOUR` | `21` | End of upload window (0–23, local time). Set equal to `UPLOAD_START_HOUR` for a 24/7 always-open window. |
| `GMT_OFFSET_HOURS` | `0` | Timezone offset from UTC in whole hours (e.g. `11` for AEDT, `-5` for EST). Used for NTP time and upload window calculation. |

> **Tip**: In `scheduled` mode the device holds the SD card only during the upload window, giving the CPAP machine uncontested access at all other times — the safest configuration for avoiding SD card errors.

---

## 5. Upload Timing & Behaviour

| Key | Default | Range | Description |
|---|---|---|---|
| `INACTIVITY_SECONDS` | `62` | 10–3600 | Seconds of SD bus silence required before the device attempts to take SD card control. Increase if your CPAP accesses the card frequently during warm-up. |
| `EXCLUSIVE_ACCESS_MINUTES` | `5` | 1–30 | Maximum minutes the device holds exclusive SD card control per upload session. The session ends early if all work is done. |
| `COOLDOWN_MINUTES` | `10` | 1–60 | Minutes to wait (SD card released) between upload cycles before starting the next inactivity check. |
| `MINIMIZE_REBOOTS` | `true` | `true`/`false` | When `true` (default), the device skips elective soft-reboots after upload sessions and reuses the existing runtime (COOLDOWN → LISTENING loop). Mandatory reboots (watchdog, user-triggered state reset / soft reboot, OTA) still occur. When `false`, the device reboots after every real upload session to restore a clean heap. |

---

## 6. Power Management

Power defaults are optimised for AirSense 11 compatibility (low peak current). Most users should not need to change these.

| Key | Default | Options | Description |
|---|---|---|---|
| `CPU_SPEED_MHZ` | `80` | `80`, `160`, `240` | ESP32 CPU clock speed. At the default 80 MHz, DFS is disabled (CPU locked) — no frequency transitions, lowest power. Set to `160` to re-enable DFS (80–160 MHz) for faster TLS handshakes on non-constrained hardware. |
| `WIFI_TX_PWR` | `MID` | `LOWEST` (-1 dBm), `LOW` (2 dBm), `MID` (5 dBm), `HIGH` (8.5 dBm), `MAX` (10 dBm) | WiFi transmit power. `MID` (5 dBm) is the default — sufficient for typical bedroom placement (~3–5 m). Use `LOWEST` or `LOW` only if your router is very close. Increase to `HIGH` or `MAX` if your router is further away or through walls. |
| `WIFI_PWR_SAVING` | `MID` | `NONE`, `MID`, `MAX` | WiFi power-saving mode. `MID` (MIN_MODEM) wakes every DTIM for broadcasts — preserves mDNS while saving ~90 mA idle. `MAX` saves slightly more but may miss mDNS queries. |
| `BROWNOUT_DETECT` | `ENABLED` | `ENABLED`, `RELAXED`, `OFF` | Set to `RELAXED` to temporarily disable the hardware brownout detector during WiFi connection (the highest peak-current phase), re-enabling it afterwards. Set to `OFF` to disable it permanently. **Use only as a last resort** for devices that reset frequently due to power drops. When set to `OFF`, the device will not reset on voltage sags, risking data corruption. A persistent warning banner is shown on the web dashboard if set to `RELAXED` or `OFF`. |

> **Note:** 802.11b is disabled at the firmware level (OFDM only). This is not configurable and eliminates the 370 mA peak TX scenario. Bluetooth is also fully disabled at compile time. Auto light-sleep is enabled — the CPU sleeps between WiFi DTIM intervals in IDLE and COOLDOWN states, reducing idle current to ~2–3 mA. TLS uses hardware-accelerated AES-128-GCM only (ChaCha20 and AES-256 are disabled at compile time for lower CPU load).

---

## 7. Security & Credentials

| Key | Default | Description |
|---|---|---|
| `STORE_CREDENTIALS_PLAIN_TEXT` | `false` | Set to `true` to store credentials in plain text in `config.txt` instead of encrypted NVS flash. **Only for debugging or boards without NVS support.** |

---

## 8. SD Card & System

| Key | Default | Description |
|---|---|---|
| `ENABLE_1BIT_SD_MODE` | `false` | If `true`, the ESP32 will mount the SD card in 1-bit mode instead of 4-bit mode. This reduces bus toggling current during ESP-side uploads, but forces a brief 4-bit compatibility remount before handing the card back to the CPAP machine (which expects a 4-bit negotiated state). Leave `false` (default) for the most reliable, lowest-spike CPAP handoff. Enable only if you want to experiment with ESP-side power reduction and your CPAP does not throw SD errors during handoff. |

---

## 9. Debugging

| Key | Default | Description |
|---|---|---|
| `PERSISTENT_LOGS` | `false` | Set to `true` to periodically flush the in-memory log buffer to the ESP32's internal `LittleFS` partition (4-file rotation: `syslog.0..3.txt`, 32 KB each, 128 KB total). Logs are flushed every **30 seconds**, continuously — including during active uploads. Use the **⬇ Download All Logs** button on the Logs tab to download persisted + current log files to your browser. Logs are written to internal flash only, never to the SD card. **Note:** Regardless of this setting, the log buffer is always flushed to LittleFS before any reboot, and to `/uploader_error.txt` on the SD card if a boot-time failure prevents WiFi connectivity. |
| `DEBUG` | `false` | Set to `true` to enable verbose diagnostics: (1) per-folder `Pre-flight scan` lines in the upload log, and (2) `[res fh= ma= fd=]` heap/file-descriptor stats appended to every log line. Leave `false` in normal operation to keep logs concise. |

---

## Removed / Legacy Keys

The following keys are **no longer used** by the firmware. They will generate a `WARN: Unknown config key` log message if present in `config.txt`.

| Key | Reason |
|---|---|
| `BOOT_DELAY_SECONDS` | **Removed in v0.9.2.** Cold-boot electrical stabilization is now hardcoded to 15 seconds (a chicken-and-egg problem: the delay happens before the SD card is read, so this value could never actually be applied). |
| `SCHEDULE` | **Legacy.** Parsed and stored but not used by any firmware logic. Superseded by `UPLOAD_MODE`, `UPLOAD_START_HOUR`, and `UPLOAD_END_HOUR`. |
| `GMT_OFFSET` | **Removed.** Use `GMT_OFFSET_HOURS`. |
| `SAVE_LOGS` | **Removed.** Use `PERSISTENT_LOGS`. |
| `LOG_TO_SD_CARD` | **Removed.** Use `PERSISTENT_LOGS`. |
| `ENABLE_SD_CMD0_RESET` | **Removed.** Use `ENABLE_1BIT_SD_MODE` for experimental low-power mode instead. |

---

## Credential Security

On first boot with a valid config, the firmware automatically:
1. Migrates `WIFI_PASSWORD`, `ENDPOINT_PASSWORD`, and `CLOUD_CLIENT_SECRET` to encrypted ESP32 NVS (flash).
2. Replaces those values in `config.txt` with `***STORED_IN_FLASH***`.

On subsequent boots, the censored values in `config.txt` are ignored and the real credentials are loaded from NVS. To reset credentials, delete the NVS partition or write new plain-text values back into `config.txt`.
