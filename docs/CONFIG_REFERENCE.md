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

---

## 6. Power Management

| Key | Default | Options | Description |
|---|---|---|---|
| `CPU_SPEED_MHZ` | `240` | `80`, `160`, `240` | ESP32 CPU clock speed. Lower values reduce heat and power draw at the cost of slower uploads. |
| `WIFI_TX_PWR` | `HIGH` | `HIGH`, `MID`, `LOW` | WiFi transmit power. Reduce if the device is physically close to the router. |
| `WIFI_PWR_SAVING` | `NONE` | `NONE`, `MID`, `MAX` | WiFi power-saving mode. `NONE` = best throughput. `MAX` = lowest power but increases latency. |

---

## 7. Security & Credentials

| Key | Default | Description |
|---|---|---|
| `STORE_CREDENTIALS_PLAIN_TEXT` | `false` | Set to `true` to store credentials in plain text in `config.txt` instead of encrypted NVS flash. **Only for debugging or boards without NVS support.** |

---

## 8. SD Card & System

| Key | Default | Description |
|---|---|---|
| `ENABLE_SD_CMD0_RESET` | `true` | If `true`, bit-bangs a `CMD0` (GO_IDLE_STATE) frame on the SD bus immediately before returning control to the CPAP machine. This forces the CPAP to cleanly remount the SD card instead of throwing a timeout or RCA-mismatch error after the ESP32 releases control. Disable only if your CPAP model is known to reject CMD0 pulses. |

---

## 9. Debugging

| Key | Default | Description |
|---|---|---|
| `SAVE_LOGS` | `false` | Set to `true` to periodically flush the in-memory log buffer to the ESP32's internal `LittleFS` partition (rotating `syslog.A.txt` / `syslog.B.txt`). Logs are flushed every **5 seconds**, continuously — including during active uploads — and always immediately before any reboot. Use the **⬇ Download Saved Logs** button on the Logs tab to download the persisted files to your browser. Logs are written to internal flash only, never to the SD card. The legacy key name `LOG_TO_SD_CARD` is accepted as a deprecated alias. |
| `DEBUG` | `false` | Set to `true` to enable verbose diagnostics: (1) per-folder `Pre-flight scan` lines in the upload log, and (2) `[res fh= ma= fd=]` heap/file-descriptor stats appended to every log line. Leave `false` in normal operation to keep logs concise. |

---

## Removed / Legacy Keys

The following keys are **no longer used** by the firmware. They will generate a `WARN: Unknown config key` log message if present in `config.txt`.

| Key | Reason |
|---|---|
| `BOOT_DELAY_SECONDS` | **Removed in v0.9.2.** Cold-boot electrical stabilization is now hardcoded to 15 seconds (a chicken-and-egg problem: the delay happens before the SD card is read, so this value could never actually be applied). |
| `SCHEDULE` | **Legacy.** Parsed and stored but not used by any firmware logic. Superseded by `UPLOAD_MODE`, `UPLOAD_START_HOUR`, and `UPLOAD_END_HOUR`. |

---

## Credential Security

On first boot with a valid config, the firmware automatically:
1. Migrates `WIFI_PASSWORD`, `ENDPOINT_PASSWORD`, and `CLOUD_CLIENT_SECRET` to encrypted ESP32 NVS (flash).
2. Replaces those values in `config.txt` with `***STORED_IN_FLASH***`.

On subsequent boots, the censored values in `config.txt` are ignored and the real credentials are loaded from NVS. To reset credentials, delete the NVS partition or write new plain-text values back into `config.txt`.
