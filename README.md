# ESP32 CPAP Data Uploader

Automatically upload CPAP therapy data from your SD card to network storage or the Cloud (SleepHQ).

**Machine support is currently limited to ResMed SD card formats only** (Series 9, Series 10, Series 11).

**Features:**
- Automatic uploads to Windows shares, NAS, Samba servers or the Cloud (**SleepHQ**)
- **"Smart" upload mode** (uploads for recent data start automatically within minutes of therapy end)
  - <u>Automatic detection of CPAP therapy session ending</u> (based on SD card activity)
  - **You get your last night of sleep data within a few minutes after taking your mask off**
- Scheduled upload mode: predictable upload window with timezone support
- **Over-The-Air (OTA) firmware updates** via web interface
- **Local Network Discovery (mDNS)**: Access the device via `http://cpap.local` (configurable hostname)
- Secure credential storage in ESP32 flash memory (optional)
- Respects CPAP machine access to SD card (only "holds" the SD card for the bare minimum required time)
  - Quick file uploads with TLS connection reuse and exclusive file access (no time budget sharing with CPAP machine)
- Tracks uploaded files (no duplicates)
- Smart empty folder handling (waits 7 days before marking folders complete)
- <u>Web interface</u> for monitoring and testing (responsive, runs as a separate task on another core)
- Automatic retry mechanism with progress tracking
- Automatic directory creation on remote shares for SMB protocol

## For End Users

**Want to use this?** Download the latest release package with precompiled firmware and easy upload tools.

### Known issues
~~- Sometimes the CPAP machine will complain about SD card not working. This is likely caused by CPAP machine attempting to access the SD card at the same time as our firmware. When this happens just reinsert the SD card.~~ (should be fixed now)

👉 [Download Latest Release](../../releases) (includes upload tools for Windows, Mac, and Linux)

See the [User Guide](release/README.md) for complete setup and usage instructions.


## Hardware

**Required:** [SD WIFI PRO](https://www.fysetc.com/products/fysetc-upgrade-sd-wifi-pro-with-card-reader-module-run-wireless-by-esp32-chip-web-server-reader-uploader-3d-printer-parts) adapter
- ESP32-PICO-D4 microcontroller
- 4MB Flash memory
- SD card interface (compatible with CPAP SD cards)
- WiFi 2.4GHz

## Supported CPAP Machines (ResMed only)

- **Series 9** (e.g. S9 AutoSet, Lumis)
- **Series 10** (e.g. AirSense, AirCurve)
- **Series 11** (e.g. Elite, AutoSet)

Other brands/models are not currently supported.

## For Developers

**Want to build or contribute?** See the [Development Guide](docs/DEVELOPMENT.md) for:
- Architecture overview
- Build instructions (libsmb2 automatically set up)
- Testing procedures
- Contributing guidelines

## Quick Start

### 1. Get the Hardware
Purchase the [SD WIFI PRO](https://www.fysetc.com/products/fysetc-upgrade-sd-wifi-pro-with-card-reader-module-run-wireless-by-esp32-chip-web-server-reader-uploader-3d-printer-parts).

### 2. Download Firmware
Download the latest release package from the [Releases page](../../releases).

### 3. Upload Firmware
Follow the instructions in the release package to upload firmware to your device.

### 4. Configure
Create a `config.txt` file on your SD card with your WiFi and network share settings.
See examples in [config.txt Examples](docs/).

### 5. Done!
Insert the SD card and power on. The device will automatically upload new CPAP data daily.

## Configuration Example

Create `config.txt` on your SD card:

Simple:
```ini
WIFI_SSID = YourNetworkName
WIFI_PASSWORD = YourNetworkPassword
HOSTNAME = cpap

ENDPOINT_TYPE = CLOUD
CLOUD_CLIENT_ID = your-sleephq-client-id
CLOUD_CLIENT_SECRET = your-sleephq-client-secret

GMT_OFFSET_HOURS = 0
```

Advanced:
```ini
WIFI_SSID = YourNetworkName
WIFI_PASSWORD = YourNetworkPassword
HOSTNAME = cpap

# Use both SMB and Cloud
ENDPOINT_TYPE = SMB,CLOUD

# SMB Settings
ENDPOINT = //192.168.1.100/cpap_backups
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password

# Cloud Settings
CLOUD_CLIENT_ID = your-sleephq-client-id
CLOUD_CLIENT_SECRET = your-sleephq-client-secret

# Upload Behavior
UPLOAD_MODE = smart
UPLOAD_START_HOUR = 8
UPLOAD_END_HOUR = 22
INACTIVITY_SECONDS = 125
EXCLUSIVE_ACCESS_MINUTES = 5
COOLDOWN_MINUTES = 10

# Timezone
GMT_OFFSET_HOURS = 0

# Power Management
CPU_SPEED_MHZ = 240
WIFI_TX_PWR = high
WIFI_PWR_SAVING = none
```

### Power Management Settings
Some devices might not be able to provide the card with enough power. In those cases it is useful to reduce the power consumption which comes at the cost of performance.

- **CPU_SPEED_MHZ** (80-240, default: 240): CPU frequency in MHz. Lower values reduce power consumption but will slow performance.
- **WIFI_TX_PWR** ("high"/"mid"/"low", default: "high"): WiFi transmission power. Lower power reduces range but saves current.
- **WIFI_PWR_SAVING** ("none"/"mid"/"max", default: "none"): WiFi power saving mode. Higher levels save more power but may increase response latency.

**Power Savings Examples:**
- CPU 240→160MHz: ~20-30mA reduction
- WiFi TX high→mid: ~10-15mA reduction  
- WiFi power save "mid": ~30-50mA reduction
- WiFi power save "max": ~50-80mA reduction

**Recommended for low power:**
```ini
CPU_SPEED_MHZ = 160
WIFI_TX_PWR = mid
WIFI_PWR_SAVING = mid
```

**Syntax Notes:** 
- Lines starting with `#` or `//` are comments.
- Spaces around `=` are optional.
- Keys are case-insensitive.

See the [User Guide](release/README.md) for complete configuration reference.

### Credential Security

By default, the system stores WiFi and endpoint passwords securely in ESP32 flash memory (NVS) and censors them in `config.txt`. This protects your credentials if someone accesses your SD card or web interface.

**Secure Mode (Default - Recommended):**
- Set `STORE_CREDENTIALS_PLAIN_TEXT = false` or omit the line
- Credentials automatically migrated to flash memory on first boot
- `config.txt` updated with `***STORED_IN_FLASH***` placeholders
- Web interface shows censored values

**Plain Text Mode (Development/Debugging):**
- Set `STORE_CREDENTIALS_PLAIN_TEXT = true`
- Credentials remain visible in `config.txt`
- Web interface shows actual values

**Migration Process:**
1. First boot with secure mode: System reads plain text credentials from `config.txt`
2. Stores them securely in ESP32 flash memory (NVS)
3. Updates `config.txt` with censored placeholders
4. Subsequent boots: Loads credentials from flash memory

**Security Considerations:**
- ✅ Protected against SD card physical access
- ✅ Protected against web interface credential exposure
- ⚠️ Not protected against flash memory dumps (requires physical device access)
- ⚠️ Migration is one-way (cannot retrieve credentials back to plain text)

For production use, always use secure mode (default). Use plain text mode only for development or debugging.

### Updating Configuration

If you need to change WiFi credentials, endpoint settings, or other configuration after initial setup:

**Method: Update Config File and Restart**
1. Update `config.txt` on SD card with new settings
2. **Important**: Enter credentials in plain text (not censored)
3. Power cycle the device (unplug and plug back in)
4. Device will automatically:
   - **Prioritize new credentials** from `config.txt` over stored ones
   - Connect to new WiFi network if credentials changed
   - Apply new endpoint settings immediately
   - Migrate new credentials to secure storage (if enabled)
   - Update `config.txt` with censored placeholders

**How It Works:**
- **Individual credential priority**: Each credential (WiFi, endpoint) is handled independently
- **Config file priority**: Plain text credentials in `config.txt` always take precedence over stored ones
- **Automatic detection**: Device detects new credentials and uses them immediately
- **Partial updates supported**: Update just WiFi password, just endpoint password, or both
- **Secure migration**: New credentials are automatically migrated to flash memory

**Example Update Process:**
```ini
# Before: config.txt has mixed state (user wants to update WiFi only)
WIFI_SSID = NewNetwork
WIFI_PASSWORD = ***STORED_IN_FLASH***
ENDPOINT = //server/share
ENDPOINT_PASSWORD = ***STORED_IN_FLASH***

# Update: Replace only WiFi password with new plain text credential
WIFI_SSID = NewNetwork
WIFI_PASSWORD = newwifipassword123
ENDPOINT = //server/share
ENDPOINT_PASSWORD = ***STORED_IN_FLASH***

# After restart: Device uses new WiFi password, keeps stored endpoint password
# WiFi password gets migrated and re-censored in config file
```

**Important Notes:**
- Always use plain text credentials when updating (not `***STORED_IN_FLASH***`)
- Device automatically detects and prioritizes config file credentials
- Upload state and progress are preserved across configuration changes
- Check serial output for configuration loading status and connection results

**Troubleshooting:**
- If WiFi connection fails, verify credentials and network availability
- If device doesn't detect changes, ensure credentials are in plain text format
- Use `"STORE_CREDENTIALS_PLAIN_TEXT": true` to keep credentials visible for debugging
- Serial output shows detailed credential loading and migration process

## Upload Modes

- **smart** (**recommended**): detects therapy end via SD activity + inactivity threshold, then uploads recent data as soon as possible.
- **scheduled**: runs uploads inside the configured upload window.

In both modes, older backlog folders are uploaded during the configured upload window.



## Build Targets

The project supports two build configurations:

### **Standard Build** (`pico32`)
```bash
pio run -e pico32
```
- **3MB app space** (maximum firmware size)
- **No OTA support** - updates require physical access
- **Best for**: Development, maximum feature space, or when OTA is not needed

### **OTA-Enabled Build** (`pico32-ota`)  
```bash
pio run -e pico32-ota
```
- **1.5MB app space** per partition (2 partitions for OTA)
- **Web-based firmware updates** via `/ota` interface
- **Best for**: Production deployments requiring remote updates

### **Size Comparison:**
| Build Type | Firmware Size | Available Space | OTA Support |
|------------|---------------|-----------------|-------------|
| `pico32`   | ~1.08MB      | 3MB            | ❌          |
| `pico32-ota` | ~1.24MB    | 1.5MB          | ✅          |

Both builds include the same core functionality - the only difference is OTA capability and partition layout.

## Firmware Updates

### **OTA Updates** (pico32-ota build only)

The OTA-enabled build supports **Over-The-Air (OTA) updates** through the web interface:

1. **Access the web interface** at `http://[device-ip]/ota`
2. **Choose update method:**
   - **File Upload**: Select and upload a `.bin` firmware file
   - **URL Download**: Enter a URL to download firmware from
3. **Monitor progress** - Update takes 1-2 minutes
4. **Device restarts automatically** when complete

**⚠️ Important**: Ensure stable WiFi connection and do not power off during updates.

### **Manual Updates** (both builds)

For devices without OTA support or when OTA fails:
1. Download firmware from [Releases page](../../releases)
2. Use esptool.py or PlatformIO to upload via USB/Serial
3. Follow instructions in the release package

## How It Works

1. **Device reads configuration** from `config.txt` on SD card
2. **Connects to WiFi** and synchronizes time with internet
3. **Waits for upload eligibility based on mode**
   - **Smart mode:** starts shortly after therapy ends (activity detection)
   - **Scheduled mode:** uploads during configured window
4. **Uploads required CPAP data** to your network share or the Cloud (SleepHQ)
   - Takes exclusive control of SD card (default 5 minutes). **Only accesses the card when NOT in use** by the CPAP machine (no therapy running, automatic detection)
   - Uploads `DATALOG/` folders and `SETTINGS/` files
   - Uploads root files **if present**: `STR.edf`, `Identification.crc`, `Identification.tgt` (ResMed 9/10), `Identification.json` (ResMed 11)
   - Tracks what's been uploaded (no duplicates)
   - Releases SD card for CPAP machine use
5. **Repeats** automatically (periodically, daily if in "scheduled" mode)

The device respects your CPAP machine's need for SD card access by only accessing the card when it's not used by CPAP, keeping upload sessions short and releasing control immediately after each session.

## Capacity & Longevity

**Memory:** Currently the firmware with SMB upload and web server enabled can track upload state for **10+ years** of daily CPAP usage (3,000+ folders) before exhausting available RAM.

**Storage:** Based on typical CPAP data size (~2.7 MB per day), the 8GB SD WIFI PRO card can store approximately **8+ years** (3,000+ days) of CPAP data before filling up.

## Project Status

**Current Version:** see "Releases" section for version information

**Status:** ✅ Production Ready + Power Management
- Hardware tested and validated
- Integration tested with real CPAP data
- All unit tests passing
- SMB/CIFS and Cloud upload (SleepHQ) fully implemented
- Web interface remains responsive during uploads, runs on a separate task/different core
- Automatic retry mechanism with progress tracking
- Automatic directory creation verified and working (SMB)
- FreeRTOS tasks for true concurrent web server operation during uploads
- Configurable power management for reduced current consumption

**Supported Upload Methods:**
- ✅ SMB/CIFS (Windows shares, NAS, Samba)
- ✅ SleepHQ direct upload
- ⏳ WebDAV (planned)

## Future Improvements
- Add WebDAV upload support

## Support & Documentation

- **User Guide:** [release/README.md](release/README.md) - Setup and usage instructions
- **Developer Guide:** [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) - Build and contribute
- **Troubleshooting:** See user guide or developer guide
- **Issues:** Report bugs or request features via GitHub issues

## License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

**What this means:**
- ✅ You can use this software for free
- ✅ You can modify the source code
- ✅ You can distribute modified versions
- ⚠️ **Any distributed versions (modified or not) must remain free and open source**
- ⚠️ Modified versions must also be licensed under GPL-3.0

This project uses libsmb2 (LGPL-2.1), which is compatible with GPL-3.0.

See [LICENSE](LICENSE) file for full terms.

## Legal & Trademarks

- **SleepHQ** is a trademark of its respective owner. This project is an unofficial client and is not affiliated with, endorsed by, or associated with SleepHQ.
  - This project uses the officially published [SleepHQ API](https://sleephq.com/api-docs) and does not rely on any non-official methods.
  - This project is **not intended to compete** with the official [Magic Uploader](https://shop.sleephq.com/products/magic-uploader-pro). We strongly encourage users to support the platform by purchasing the official solution, which comes with vendor support and requires no technical setup (flashing).
- **ResMed** is a trademark of ResMed. This software is not affiliated with ResMed.
- All other trademarks are the property of their respective owners.

### Disclaimer & No Warranty

**USE AT YOUR OWN RISK.**

This project (including source code, pre-compiled binaries, and documentation) is provided "as is" and **without any warranty of any kind**, express or implied.

**By using this software, you acknowledge and agree that:**
1.  **You are solely responsible** for the safety and operation of your CPAP machine and data.
2.  The authors and contributors **guarantee nothing** regarding the reliability, safety, or suitability of this software.
3.  **We are not liable** for any damage to your CPAP machine, SD card, loss of therapy data, or any other direct or indirect damage resulting from the use of this project.
4.  **Warranty Implication:** Using third-party accessories or software with your medical device may void its warranty. You accept this risk entirely.

This software interacts directly with medical device hardware and file systems. While every effort has been made to ensure safety, bugs or hardware incompatibilities can occur.

**GPL-3.0 License Disclaimer:**
> THERE IS NO WARRANTY FOR THE PROGRAM, TO THE EXTENT PERMITTED BY APPLICABLE LAW. EXCEPT WHEN OTHERWISE STATED IN WRITING THE COPYRIGHT HOLDERS AND/OR OTHER PARTIES PROVIDE THE PROGRAM "AS IS" WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE ENTIRE RISK AS TO THE QUALITY AND PERFORMANCE OF THE PROGRAM IS WITH YOU. SHOULD THE PROGRAM PROVE DEFECTIVE, YOU ASSUME THE COST OF ALL NECESSARY SERVICING, REPAIR OR CORRECTION.

See the [LICENSE](LICENSE) file for the full legal text.

---

**Made for CPAP users who want automatic, reliable data backups.**

