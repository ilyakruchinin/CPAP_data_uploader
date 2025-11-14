# SD WIFI PRO auto uploader

This project intends to create a simple solution to upload new files daily to a remote endpoint using SD WIFI PRO hardware.
The intended usage is backups of CPAP machine data stored in the SD card, the data in the card is read only.
TODO: add information on how this data can be used with Oscar or SleepHQ

## Requirements
- Standalone device which connects to the network specified in the CONFIG file stored in the root of the SD card and uploads new files based on the schedule listed in the SD card.
- Files in the SD card are read only to prevent data loss.
- Files in remote endpoint are write only.


## Hardware
- ESP32-PICO-V3-02 ([SD WIFI PRO](https://www.fysetc.com/products/fysetc-upgrade-sd-wifi-pro-with-card-reader-module-run-wireless-by-esp32-chip-web-server-reader-uploader-3d-printer-parts))
- 8GB built-in Flash
- SD 7.0 compatible interface

## Architecture

This project uses a clean, class-based architecture with explicit dependency injection:

- **Config** - Manages configuration from SD card
- **SDCardManager** - Handles SD card sharing with CPAP machine
- **WiFiManager** - Manages WiFi station mode connection
- **FileUploader** - Handles file upload to remote endpoints

## Project Structure
```
├── src/                  # Main application code
│   ├── main.cpp         # Application entry point
│   ├── Config.cpp       # Configuration management
│   ├── SDCardManager.cpp # SD card control
│   ├── WiFiManager.cpp  # WiFi connection handling
│   └── FileUploader.cpp # File upload logic
├── include/             # Header files
│   ├── pins_config.h    # Pin definitions for SD WIFI PRO
│   ├── Config.h
│   ├── SDCardManager.h
│   ├── WiFiManager.h
│   └── FileUploader.h
├── venv/                # Python virtual environment
├── docs/                # Documentation and reference firmware
├── platformio.ini       # PlatformIO configuration
└── README.md           # This file
```

## Libraries (managed by PlatformIO)
- **ArduinoJson** - JSON library for config file parsing
- **SD_MMC** - Built-in ESP32 SDIO library (4-bit mode)
- **WiFi** - Built-in ESP32 WiFi library
- **FS** - Built-in ESP32 filesystem library
- **libsmb2** - SMB2/3 client library (ESP-IDF component, optional - only needed if SMB upload is enabled)

## Upload Backend Configuration

This project supports multiple upload backends through compile-time feature flags. Only enable the backend(s) you need to minimize binary size.

### Available Backends

| Backend | Status | Binary Size | Feature Flag |
|---------|--------|-------------|--------------|
| SMB/CIFS | ✅ Implemented | +220-270KB | `-DENABLE_SMB_UPLOAD` |
| WebDAV | ⏳ TODO | +50-80KB (est.) | `-DENABLE_WEBDAV_UPLOAD` |
| SleepHQ | ⏳ TODO | +40-60KB (est.) | `-DENABLE_SLEEPHQ_UPLOAD` |

### Enabling/Disabling Backends

Edit `platformio.ini` and uncomment the desired feature flag(s):

```ini
build_flags = 
    -DCORE_DEBUG_LEVEL=3
    -Icomponents/libsmb2/include
    -DENABLE_SMB_UPLOAD          ; Enable SMB/CIFS upload support
    ; -DENABLE_WEBDAV_UPLOAD     ; Enable WebDAV upload support (TODO)
    ; -DENABLE_SLEEPHQ_UPLOAD    ; Enable SleepHQ direct upload (TODO)
```

**Note:** You can enable multiple backends simultaneously. The active backend is selected at runtime via the `ENDPOINT_TYPE` setting in `config.json`.

### SMB Upload Setup

**Quick Setup:**
```bash
./scripts/setup_libsmb2.sh
```

**Manual Setup:**
```bash
mkdir -p components
git clone https://github.com/sahlberg/libsmb2.git components/libsmb2
pio run
```

See [docs/LIBSMB2_INTEGRATION.md](docs/LIBSMB2_INTEGRATION.md) for detailed integration documentation.

**To disable SMB support** (reduces binary size by ~220KB), comment out `-DENABLE_SMB_UPLOAD` in `platformio.ini`.

## Setup
1. Activate Python virtual environment: `source venv/bin/activate`
2. Install dependencies: `pio pkg install`
3. Build: `pio run`
4. Upload: `pio run -t upload`

## Quick Build
```bash
source venv/bin/activate
pio pkg install
pio run -t upload
```

## Monitor
`pio device monitor`

## Pin Configuration
See `include/pins_config.h` for pin definitions specific to SD WIFI PRO hardware.



# Runtime usage

## CONFIG FILE

Create a `config.json` file in the root of your SD card with the following format:

```json
{
  "WIFI_SSID": "YourNetworkName",
  "WIFI_PASS": "YourNetworkPassword",
  "SCHEDULE": "daily",
  "ENDPOINT": "http://your-server.com/upload",
  "ENDPOINT_TYPE": "WEBDAV",
  "ENDPOINT_USER": "username",
  "ENDPOINT_PASS": "password"
}
```

### Configuration Fields

- **WIFI_SSID**: SSID of the WiFi network to connect to (required)
- **WIFI_PASS**: Password for the WiFi network (required)
- **SCHEDULE**: Upload schedule (e.g., "daily", "hourly") - TODO: implement scheduling logic
- **ENDPOINT**: Remote location where files will be uploaded (required)
- **ENDPOINT_TYPE**: Type of endpoint - `WEBDAV`, `SMB`, or `SLEEPHQ`
  - `WEBDAV`: WebDAV share (e.g., NextCloud) - Format: `http://address/folder` - TODO: implement
  - `SMB`: Windows/Samba share - Format: `//address/share` (e.g., `//10.0.0.5/backups`) - ✅ Implemented
  - `SLEEPHQ`: Direct upload to SleepHQ - TODO: implement
- **ENDPOINT_USER**: Username for the remote endpoint
- **ENDPOINT_PASS**: Password for the remote endpoint

## How It Works

1. **Startup**: Device reads config from SD card
2. **WiFi Connection**: Connects to specified WiFi network in station mode
3. **File Detection**: Periodically checks SD card for new files
4. **SD Card Sharing**: Respects CPAP machine access - only reads when CPAP is not using the card
5. **Upload**: Uploads new files to configured endpoint
6. **Monitoring**: Continuously monitors for new files and maintains WiFi connection

## Development Status

### Implemented
- ✅ SD card sharing with CPAP machine
- ✅ Configuration file loading from SD card
- ✅ WiFi station mode connection
- ✅ Class-based architecture with dependency injection
- ✅ SMB upload implementation (libsmb2-based)
- ✅ File tracking with checksums (which files have been uploaded)
- ✅ Schedule-based upload logic (NTP-synchronized daily uploads)
- ✅ Time-budgeted SD card access (respects CPAP machine access needs)
- ✅ Upload state persistence across reboots
- ✅ Retry logic with adaptive time budgets
- ✅ Feature flags for compile-time backend selection

### TODO
- ⏳ WebDAV upload implementation (placeholder created)
- ⏳ SleepHQ upload implementation (placeholder created)
- ⏳ Status LED indicators
- ⏳ Low power mode when idle
- ⏳ Web interface for configuration and monitoring

## References

### Hardware
- [SD WIFI PRO](https://www.fysetc.com/products/fysetc-upgrade-sd-wifi-pro-with-card-reader-module-run-wireless-by-esp32-chip-web-server-reader-uploader-3d-printer-parts) - FYSETC SD WIFI PRO hardware used in this project
- [SD WIFI PRO GitHub](https://github.com/FYSETC/SD-WIFI-PRO) - Official hardware repository with schematics and documentation

### Reference Firmware
- [SdWiFiBrowser](https://github.com/FYSETC/SdWiFiBrowser) - Basic WiFi file browser firmware for SD WIFI PRO
- [ESP3D](https://github.com/luc-github/ESP3D) - Advanced 3D printer firmware with WebDAV/FTP support

These projects provided hardware specifications and reference implementations but are not used directly in this project.
