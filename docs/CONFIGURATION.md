# Configuration Reference

Complete reference for all `config.json` options. Place this file on the SD card root as `/config.json`.

> **Important:** JSON does not support comments. The `_comment` fields in the example files are ignored by the parser but kept for documentation. Ensure there are **no trailing commas** after the last property in any JSON object.

---

## Table of Contents

- [Quick Start Examples](#quick-start-examples)
- [WiFi Settings](#wifi-settings)
- [Upload Endpoint](#upload-endpoint)
- [Upload Schedule](#upload-schedule)
- [Upload Session](#upload-session)
- [SleepHQ Cloud Settings](#sleephq-cloud-settings)
- [Power Management](#power-management)
- [Credential Security](#credential-security)
- [Debugging](#debugging)
- [Complete Option Reference](#complete-option-reference)

---

## Quick Start Examples

### Minimal SMB Configuration

Upload to a Windows share or NAS once daily at noon:

```json
{
  "WIFI_SSID": "MyNetwork",
  "WIFI_PASS": "MyPassword",
  "ENDPOINT": "//192.168.1.100/cpap_backups",
  "ENDPOINT_TYPE": "SMB",
  "ENDPOINT_USER": "username",
  "ENDPOINT_PASS": "password",
  "UPLOAD_HOUR": 12,
  "GMT_OFFSET_HOURS": -8
}
```

### Minimal SleepHQ Cloud Configuration

Upload directly to SleepHQ for sleep data analysis:

```json
{
  "WIFI_SSID": "MyNetwork",
  "WIFI_PASS": "MyPassword",
  "ENDPOINT_TYPE": "CLOUD",
  "CLOUD_CLIENT_ID": "your-client-id",
  "CLOUD_CLIENT_SECRET": "your-client-secret",
  "UPLOAD_HOUR": 14,
  "GMT_OFFSET_HOURS": -8
}
```

### Dual Backend (SMB + SleepHQ)

Upload to both a local NAS and SleepHQ simultaneously:

```json
{
  "WIFI_SSID": "MyNetwork",
  "WIFI_PASS": "MyPassword",
  "ENDPOINT": "//192.168.1.100/cpap_backups",
  "ENDPOINT_TYPE": "SMB,CLOUD",
  "ENDPOINT_USER": "nasuser",
  "ENDPOINT_PASS": "naspassword",
  "CLOUD_CLIENT_ID": "your-client-id",
  "CLOUD_CLIENT_SECRET": "your-client-secret",
  "UPLOAD_HOUR": 14,
  "GMT_OFFSET_HOURS": -8
}
```

### Frequent Uploads with Day Limit

Upload every 60 minutes, only the last 30 days of data:

```json
{
  "WIFI_SSID": "MyNetwork",
  "WIFI_PASS": "MyPassword",
  "ENDPOINT_TYPE": "CLOUD",
  "CLOUD_CLIENT_ID": "your-client-id",
  "CLOUD_CLIENT_SECRET": "your-client-secret",
  "UPLOAD_INTERVAL_MINUTES": 60,
  "MAX_DAYS": 30,
  "GMT_OFFSET_HOURS": -8
}
```

See also: `docs/config.json.example`, `docs/config.json.cloud-example`, `docs/config.json.dual-example`

---

## WiFi Settings

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `WIFI_SSID` | string | **Yes** | — | WiFi network name (SSID). Must be a 2.4 GHz network; ESP32 does not support 5 GHz. |
| `WIFI_PASS` | string | **Yes** | — | WiFi password. Automatically migrated to secure flash storage on first boot (see [Credential Security](#credential-security)). |

---

## Upload Endpoint

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `ENDPOINT_TYPE` | string | **Yes** | `"SMB"` (legacy) | Upload backend(s) to use. Supported values: `"SMB"`, `"CLOUD"`, `"WEBDAV"`, or comma-separated combinations like `"SMB,CLOUD"`. |
| `ENDPOINT` | string | Conditional | — | Server address. **Required** for SMB and WebDAV. Not used for cloud-only. |
| `ENDPOINT_USER` | string | No | `""` | Username for SMB/WebDAV authentication. |
| `ENDPOINT_PASS` | string | Conditional | — | Password for SMB/WebDAV authentication. Automatically migrated to secure flash storage on first boot. |

### ENDPOINT_TYPE Values

| Value | Description | Requires |
|-------|-------------|----------|
| `SMB` | Upload to SMB/CIFS share (Windows, NAS, Samba) | `ENDPOINT`, `ENDPOINT_USER`, `ENDPOINT_PASS` |
| `CLOUD` | Upload to SleepHQ cloud service | `CLOUD_CLIENT_ID`, `CLOUD_CLIENT_SECRET` |
| `WEBDAV` | Upload to WebDAV server (Nextcloud, ownCloud) | `ENDPOINT`, `ENDPOINT_USER`, `ENDPOINT_PASS` |
| `SMB,CLOUD` | Upload to both SMB and SleepHQ | Both sets of fields |
| `WEBDAV,CLOUD` | Upload to both WebDAV and SleepHQ | Both sets of fields |

> **Note:** Multiple backends upload the same file to each active backend. If one backend fails, the file is **not** marked as uploaded and will be retried for all backends on the next session. If cloud import creation fails, the cloud backend is skipped for the session but SMB/WebDAV uploads continue.

### SMB Endpoint Format

The `ENDPOINT` field for SMB uses UNC path format:

```
//server-ip/share-name
//server-ip/share-name/subfolder
```

Examples:
- `//192.168.1.100/cpap_backups` — Root of a share
- `//192.168.1.100/data/cpap/resmed` — Subfolder within a share
- `//mynas.local/cpap` — Using mDNS hostname

### WebDAV Endpoint Format

The `ENDPOINT` field for WebDAV uses a full URL:

```
https://cloud.example.com/remote.php/dav/files/user/cpap
```

---

## Upload Schedule

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `UPLOAD_HOUR` | integer | No | `12` | Hour of day (0-23) to start the daily upload. Uses local time based on `GMT_OFFSET_HOURS`. Ignored when `UPLOAD_INTERVAL_MINUTES` is set. |
| `GMT_OFFSET_HOURS` | integer | No | `0` | Timezone offset from UTC in hours. Examples: PST = `-8`, EST = `-5`, UTC = `0`, CET = `+1`, AEST = `+10`. |
| `UPLOAD_INTERVAL_MINUTES` | integer | No | `0` | Upload every N minutes instead of once daily. Set to `0` to use the daily schedule (`UPLOAD_HOUR`). When set, `UPLOAD_HOUR` is ignored. Minimum effective value: `1`. |
| `MAX_DAYS` | integer | No | `0` | Only upload DATALOG folders from the last N days. Set to `0` to upload all data. Requires NTP time sync; if time is unavailable, all folders are processed. **Affects all backends** (SMB, Cloud, WebDAV). |
| `RECENT_FOLDER_DAYS` | integer | No | `2` | Re-check completed DATALOG folders within the last N days for changed files. Files are compared by MD5 checksum; only changed files are re-uploaded. Useful for catching data that grows during an active sleep session (e.g., today's BRP, PLD files). Minimum: `0` (no re-checking), maximum: `7`. |

### How Scheduling Works

**Daily mode** (default): The device uploads once per day at `UPLOAD_HOUR` in local time. After a successful upload, the schedule is marked complete for the day. NTP time synchronization is required.

**Interval mode**: When `UPLOAD_INTERVAL_MINUTES > 0`, the device uploads every N minutes regardless of time of day. Each interval triggers a full scan for new or changed files. This mode is useful for:
- Near-real-time data during active sleep sessions
- Testing and development
- Environments where daily scheduling is insufficient

**Budget exhaustion retry**: If an upload session runs out of time (hits `SESSION_DURATION_SECONDS`), the device automatically schedules a retry after `2 × SESSION_DURATION_SECONDS` to continue uploading remaining files.

---

## Upload Session

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `SESSION_DURATION_SECONDS` | integer | No | `300` | Maximum duration (in seconds) of each upload session. The device will pause and retry later if this limit is reached. Longer sessions upload more files but occupy the SD card longer. |
| `MAX_RETRY_ATTEMPTS` | integer | No | `3` | Maximum number of retry attempts for a single DATALOG folder before skipping it. Resets when the folder is successfully uploaded or when upload state is cleared. |
| `BOOT_DELAY_SECONDS` | integer | No | `30` | Delay (in seconds) after boot before the first upload attempt. Allows the CPAP machine to initialize and begin writing data. |
| `SD_RELEASE_INTERVAL_SECONDS` | integer | No | `2` | How often (in seconds) the device temporarily releases the SD card during an upload session, giving the CPAP machine priority access. Lower values are more polite to the CPAP machine but slow uploads. |
| `SD_RELEASE_WAIT_MS` | integer | No | `500` | How long (in milliseconds) the device waits with the SD card released before reclaiming it. During this time the CPAP machine can read/write. |

### Session Behavior

Each upload session:

1. Takes control of the SD card
2. Scans for new or changed files (DATALOG folders newest-first, then root files and SETTINGS)
3. Uploads files to all active backends
4. Periodically releases the SD card (every `SD_RELEASE_INTERVAL_SECONDS`) to allow CPAP access
5. Stops if `SESSION_DURATION_SECONDS` is reached
6. Releases the SD card

If the session runs out of time with files remaining, a retry is automatically scheduled.

---

## SleepHQ Cloud Settings

These options are only relevant when `ENDPOINT_TYPE` includes `CLOUD` or `SLEEPHQ`.

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `CLOUD_CLIENT_ID` | string | **Yes** | — | SleepHQ OAuth client ID. Obtain from your SleepHQ account API settings. |
| `CLOUD_CLIENT_SECRET` | string | **Yes** | — | SleepHQ OAuth client secret. Automatically migrated to secure flash storage on first boot. |
| `CLOUD_TEAM_ID` | string | No | auto-discovered | SleepHQ team ID. If omitted, automatically discovered via the `/api/v1/me` endpoint after authentication. Set this to skip the discovery step. |
| `CLOUD_DEVICE_ID` | integer | No | `0` | Device ID sent to SleepHQ with import creation. |
| `CLOUD_BASE_URL` | string | No | `"https://sleephq.com"` | SleepHQ API base URL. Only change for testing or self-hosted instances. |
| `CLOUD_INSECURE_TLS` | boolean | No | `false` | Skip TLS certificate validation. **Not recommended for production.** Use only for testing with proxies or custom servers. |

### Getting SleepHQ Credentials

1. Log in to [sleephq.com](https://sleephq.com)
2. Navigate to your account/API settings
3. Create or find your OAuth client credentials
4. Copy the `client_id` and `client_secret` into your `config.json`

### How Cloud Upload Works

1. **Authentication**: OAuth password grant using `client_id` and `client_secret`
2. **Team discovery**: Automatically finds your team ID (or uses configured `CLOUD_TEAM_ID`)
3. **Import creation**: Creates an import session on first file upload (lazy — no import if nothing changed)
4. **File upload**: Each file is uploaded via multipart POST with a content hash for deduplication
5. **Import processing**: After all files are uploaded, the import is submitted for server-side processing

**Content hash**: SleepHQ uses `MD5(file_content + filename)` for server-side deduplication. If a file has already been uploaded with the same hash, SleepHQ skips it automatically.

**TLS**: The firmware embeds the GTS Root R4 (Google Trust Services) root CA certificate for secure TLS validation. If SleepHQ changes its CA provider, set `CLOUD_INSECURE_TLS: true` as a temporary fallback.

---

## Power Management

These settings reduce current consumption, useful when the CPAP machine's USB port provides limited power.

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `CPU_SPEED_MHZ` | integer | No | `240` | CPU frequency in MHz. Valid values: `80`, `160`, `240`. Lower values reduce power but slow performance. |
| `WIFI_TX_PWR` | string | No | `"high"` | WiFi transmission power. Values: `"high"`, `"mid"`, `"low"`. Lower power reduces range but saves current. |
| `WIFI_PWR_SAVING` | string | No | `"none"` | WiFi power saving mode. Values: `"none"`, `"mid"`, `"max"`. Higher levels save more power but may increase latency. |

### Power Consumption Impact

| Setting | Change | Estimated Savings |
|---------|--------|-------------------|
| CPU 240 → 160 MHz | Moderate slowdown | ~20-30 mA |
| CPU 240 → 80 MHz | Significant slowdown | ~40-60 mA |
| WiFi TX high → mid | Reduced range | ~10-15 mA |
| WiFi TX high → low | Significantly reduced range | ~15-20 mA |
| WiFi power save mid | Slight latency increase | ~30-50 mA |
| WiFi power save max | Notable latency increase | ~50-80 mA |

### Recommended Profiles

**Maximum performance** (default):
```json
{
  "CPU_SPEED_MHZ": 240,
  "WIFI_TX_PWR": "high",
  "WIFI_PWR_SAVING": "none"
}
```

**Balanced** (good for most CPAP USB ports):
```json
{
  "CPU_SPEED_MHZ": 160,
  "WIFI_TX_PWR": "mid",
  "WIFI_PWR_SAVING": "mid"
}
```

**Low power** (for constrained power supplies):
```json
{
  "CPU_SPEED_MHZ": 80,
  "WIFI_TX_PWR": "low",
  "WIFI_PWR_SAVING": "max"
}
```

---

## Credential Security

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `STORE_CREDENTIALS_PLAIN_TEXT` | boolean | No | `false` | When `false` (default), credentials are migrated from `config.json` to ESP32 flash memory (NVS) on first boot and replaced with `***STORED_IN_FLASH***`. When `true`, credentials remain in plain text on the SD card. |

### How Secure Storage Works

**First boot (secure mode):**
1. Device reads plain text credentials from `config.json`
2. Stores them in ESP32 flash memory (NVS namespace: `cpap_creds`)
3. Rewrites `config.json` with `***STORED_IN_FLASH***` placeholders
4. All subsequent boots load credentials from flash

**Affected credentials:**
- `WIFI_PASS`
- `ENDPOINT_PASS`
- `CLOUD_CLIENT_SECRET`

**Updating credentials after migration:**
1. Edit `config.json` and replace the `***STORED_IN_FLASH***` value with the new plain-text credential
2. Reboot the device
3. The device detects the new credential, migrates it to flash, and re-censors the file
4. Only the changed credential is updated; others remain in flash

**Security properties:**
- Protected against physical SD card access
- Protected against web interface credential exposure (always censored in HTTP responses)
- Not protected against flash memory dumps (requires physical device access + tools)

---

## Debugging

| Option | Type | Required | Default | Description |
|--------|------|----------|---------|-------------|
| `LOG_TO_SD_CARD` | boolean | No | `false` | Write log messages to `/debug_log.txt` on the SD card. **For debugging only** — can conflict with CPAP machine SD card access. Disable for normal operation. |

### Other Debugging Tools

- **Serial monitor**: Connect via USB and run `pio device monitor` or `./monitor.sh` to see real-time logs
- **Web interface**: Access `http://<device-ip>/` to view status, logs, and trigger manual uploads (requires `ENABLE_TEST_WEBSERVER` build flag)
- **Verbose logging**: Enable `-DENABLE_VERBOSE_LOGGING` build flag for detailed diagnostics (adds ~10-15KB to firmware)

---

## Complete Option Reference

| Option | Type | Default | Section |
|--------|------|---------|---------|
| `WIFI_SSID` | string | — | [WiFi](#wifi-settings) |
| `WIFI_PASS` | string | — | [WiFi](#wifi-settings) |
| `ENDPOINT` | string | — | [Endpoint](#upload-endpoint) |
| `ENDPOINT_TYPE` | string | `"SMB"` | [Endpoint](#upload-endpoint) |
| `ENDPOINT_USER` | string | `""` | [Endpoint](#upload-endpoint) |
| `ENDPOINT_PASS` | string | — | [Endpoint](#upload-endpoint) |
| `UPLOAD_HOUR` | integer | `12` | [Schedule](#upload-schedule) |
| `GMT_OFFSET_HOURS` | integer | `0` | [Schedule](#upload-schedule) |
| `UPLOAD_INTERVAL_MINUTES` | integer | `0` | [Schedule](#upload-schedule) |
| `MAX_DAYS` | integer | `0` | [Schedule](#upload-schedule) |
| `RECENT_FOLDER_DAYS` | integer | `2` | [Schedule](#upload-schedule) |
| `SESSION_DURATION_SECONDS` | integer | `300` | [Session](#upload-session) |
| `MAX_RETRY_ATTEMPTS` | integer | `3` | [Session](#upload-session) |
| `BOOT_DELAY_SECONDS` | integer | `30` | [Session](#upload-session) |
| `SD_RELEASE_INTERVAL_SECONDS` | integer | `2` | [Session](#upload-session) |
| `SD_RELEASE_WAIT_MS` | integer | `500` | [Session](#upload-session) |
| `CLOUD_CLIENT_ID` | string | — | [Cloud](#sleephq-cloud-settings) |
| `CLOUD_CLIENT_SECRET` | string | — | [Cloud](#sleephq-cloud-settings) |
| `CLOUD_TEAM_ID` | string | auto | [Cloud](#sleephq-cloud-settings) |
| `CLOUD_DEVICE_ID` | integer | `0` | [Cloud](#sleephq-cloud-settings) |
| `CLOUD_BASE_URL` | string | `"https://sleephq.com"` | [Cloud](#sleephq-cloud-settings) |
| `CLOUD_INSECURE_TLS` | boolean | `false` | [Cloud](#sleephq-cloud-settings) |
| `CPU_SPEED_MHZ` | integer | `240` | [Power](#power-management) |
| `WIFI_TX_PWR` | string | `"high"` | [Power](#power-management) |
| `WIFI_PWR_SAVING` | string | `"none"` | [Power](#power-management) |
| `STORE_CREDENTIALS_PLAIN_TEXT` | boolean | `false` | [Security](#credential-security) |
| `LOG_TO_SD_CARD` | boolean | `false` | [Debugging](#debugging) |
