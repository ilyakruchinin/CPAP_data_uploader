# libsmb2 Setup Guide

## Quick Start

### One-Time Setup

Run the automated setup script:

```bash
./scripts/setup_libsmb2.sh
```

Or manually:

```bash
mkdir -p components
git clone https://github.com/sahlberg/libsmb2.git components/libsmb2
pio run
```

### Build and Upload

```bash
pio run              # Build
pio run -t upload    # Upload to device
pio device monitor   # Monitor serial output
```

## Overview

This project uses libsmb2 as an ESP-IDF component to provide SMB/CIFS file upload functionality. The library is integrated into the Arduino framework project via PlatformIO's ESP-IDF component support.

## Prerequisites

- PlatformIO installed
- Git installed
- ESP32 Arduino framework (already configured in platformio.ini)

## Installation Steps

### 1. Clone libsmb2 into components directory

From the project root directory, run:

```bash
mkdir -p components
git clone https://github.com/sahlberg/libsmb2.git components/libsmb2
```

### 2. Verify directory structure

After cloning, you should have:

```
project_root/
├── components/
│   └── libsmb2/
│       ├── include/
│       │   └── smb2/
│       │       ├── smb2.h
│       │       └── libsmb2.h
│       ├── lib/
│       ├── library.json
│       ├── library_build.py
│       └── lib/esp_compat_wrapper.h
├── src/
├── include/
└── platformio.ini
```

### 3. Build the project

PlatformIO will automatically detect and build libsmb2:

```bash
pio run
```

Expected output:
- RAM: ~13.6% (44KB)
- Flash: ~26.1% (821KB)
- Status: SUCCESS

## Configuration

Create `config.json` on your SD card:

```json
{
  "WIFI_SSID": "YourNetwork",
  "WIFI_PASS": "YourPassword",
  "ENDPOINT": "//192.168.1.100/backups",
  "ENDPOINT_TYPE": "SMB",
  "ENDPOINT_USER": "username",
  "ENDPOINT_PASS": "password",
  "UPLOAD_HOUR": 12,
  "SESSION_DURATION_SECONDS": 5,
  "MAX_RETRY_ATTEMPTS": 3,
  "GMT_OFFSET_SECONDS": 0,
  "DAYLIGHT_OFFSET_SECONDS": 0
}
```

### Endpoint Format Examples

- Windows share: `//192.168.1.100/ShareName`
- Samba/NAS: `//nas.local/backups`
- With subfolder: `//server/share/subfolder`

## Troubleshooting

### Build fails with "smb2/smb2.h not found"

**Solution:** 
- Run `./scripts/setup_libsmb2.sh`
- Verify `components/libsmb2/` exists
- Check that `lib_extra_dirs = components` is in platformio.ini

### Linker errors about undefined SMB functions

**Solution:** 
1. Clean the build: `pio run -t clean`
2. Rebuild: `pio run`
3. Verify `components/libsmb2/library.json` exists

### SMB connection fails

**Solution:**
- Check network connectivity
- Verify SMB share is accessible from your network
- Test credentials manually
- Check firewall settings on the SMB server

### Out of memory during compilation

**Solution:** The project uses `huge_app.csv` partition scheme to provide 3MB for application code. If you still run out of space:
1. Disable SMB support by removing `-DENABLE_SMB_UPLOAD` from platformio.ini
2. Use WebDAV instead (smaller footprint)

## Binary Size Impact

With SMB support enabled:
- libsmb2 library: ~200-250KB
- SMBUploader wrapper: ~10-20KB
- Total overhead: ~220-270KB

This is acceptable given the 3MB application partition.

## Disabling SMB Support

To build without SMB support (e.g., for WebDAV-only builds):

1. Remove `-DENABLE_SMB_UPLOAD` from `platformio.ini` build_flags
2. The SMBUploader code will be excluded via conditional compilation
3. Binary size will be reduced by ~220-270KB

## Alternative: WebDAV

If SMB proves problematic or binary size is a concern, consider using WebDAV instead:
- HTTP-based protocol (simpler)
- Native Arduino HTTPClient support
- Smaller binary footprint (~50KB)
- Works with NextCloud, ownCloud, etc.

## References

- libsmb2 GitHub: https://github.com/sahlberg/libsmb2
- PlatformIO ESP-IDF Components: https://docs.platformio.org/en/latest/frameworks/espidf.html
- ESP32 Arduino + ESP-IDF: https://docs.espressif.com/projects/arduino-esp32/en/latest/esp-idf_component.html

