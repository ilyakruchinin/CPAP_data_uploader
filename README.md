# SD WIFI PRO auto uploader

This project intends to create a simple solution to upload new files daily to a remote endpoint using SD WIFI PRO hardware.
The intended usage is backups of CPAP machine data stored in the SD card, the data in the card is read only.
TODO: add information on how this data can be used with Oscar or SleepHQ

## Requirements
- Standalone device which connects to the network specified in the CONFIG file stored in the root of the SD card and uploads new files based on the schedule listed in the SD card.
- Files in the SD card are read only to prevent data loss.
- Files in remote endpoint are write only.


## Hardware
- ESP32 PICO D4 ([SD WIFI PRO](https://www.fysetc.com/products/fysetc-upgrade-sd-wifi-pro-with-card-reader-module-run-wireless-by-esp32-chip-web-server-reader-uploader-3d-printer-parts))
- 8GB built-in Flash
- SD 7.0 compatible interface

## Available Firmware references (included in docs folder)
These references will be deleted once we no longer need them.

TODO: we need to get rid of the suff that we don't need. we might need to add new dependencies and get rid of the old dependencies listed here as firmware options. we want to keep the FTP server and WebDav support from ESP3D but we need the station mode.

This project includes reference code from two firmware options:

### 1. SdWiFiBrowser (SWD 0.x)
Basic WiFi file browser with:
- File upload/download via web interface
- WiFi AP and STA modes
- Simple web server

### 2. ESP3D v3.0 (docs/SD-WIFI-PRO/ESDP3D_3.0)
Advanced firmware with:
- WebDAV support
- FTP server
- Web UI with printer control
- OLED screen support
- Serial port printer control

## Current Setup
This project uses **SdWiFiBrowser** libraries as a starting point.

## Project Structure
```
├── src/                  # Main application code
│   └── main.cpp         # Application entry point
├── include/             # Header files
│   └── pins_config.h    # Pin definitions for SD WIFI PRO
├── venv/                # Python virtual environment
├── docs/                # Documentation and reference firmware
├── platformio.ini       # PlatformIO configuration
├── apply_patches.sh     # Script to patch library compatibility issues
└── README.md           # This file
```

## Libraries (managed by PlatformIO)
Libraries are automatically downloaded from GitHub:
- **AsyncTCP** - Async TCP library for ESP32
- **ESPAsyncWebServer** - Async web server
- **SdWiFiBrowser** - WiFi file browser with:
  - config.h/cpp - Configuration management
  - network.h/cpp - WiFi handling  
  - FSWebServer.h/cpp - Web server
  - sdControl.h/cpp - SD card control
- **ArduinoJson** - JSON library

## Setup
1. Activate Python virtual environment: `source venv/bin/activate`
2. Install dependencies: `pio pkg install`
3. Apply library patches: `./apply_patches.sh`
4. Build: `pio run`
5. Upload: `pio run -t upload`
6. Access via browser at `http://192.168.4.1` (AP mode) or assigned IP (STA mode)

## Quick Build
```bash
source venv/bin/activate
pio pkg install
./apply_patches.sh
pio run -t upload
```

## Known Issues

### Library Compatibility Patches
The `apply_patches.sh` script automatically fixes const-correctness issues in:
- AsyncTCP library (status() method)
- SdWiFiBrowser FSWebServer (AsyncWebParameter pointers)

These patches are required due to API changes in ESPAsyncWebServer 3.x.

**Note:** Run `./apply_patches.sh` after every `pio pkg install` or when cleaning the project.

## Monitor
`pio device monitor`

## Pin Configuration
See `include/pins_config.h` for pin definitions specific to SD WIFI PRO hardware.



# Runtime usage

## CONFIG FILE
TODO: add information about contents and usage of configuration file
- MODE: AP|STA
  While in AP mode, the device presents a WEBDAV interface to download the files.
  While in STAtion mode, the device connects to the provided network and attempts to upload all new files to the specified remote endpoint
- WIFI_SSID: SSID of the AP network or the network to join
- WIFI_PASS: password of the network to join
- SCHEDULE: TODO
- ENDPOINT: remote location where to drop the files, must provide write permissions.
- ENDPOINT_TYPE: SMB|WEBDAV|SLEEPHQ
  (TODO) SMB is a windows share, Format: //address/folder. example //10.0.0.5/respaldos/cpap_data
  (TODO) WEBDAV requires a webdav share such as NextCloud, Format: http://address/folder
  (TODO) SLEEPHQ upload the files directly to sleep HQ, this option requires extra setup explained in SLEEPHQ section
- ENDPOINT_USER: User required for the remote endpoint
- ENDPOINT_PASS: password required for the remote endpoint

## SLEEPHQ compatibility
not yet supported