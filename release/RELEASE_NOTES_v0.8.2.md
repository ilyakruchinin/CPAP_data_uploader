# CPAP Data Uploader v0.8.2 Release Notes

## Overview
v0.8.2 is a bug-fix release that resolves two critical issues reported in v0.8.1:
- Configuration parsing failure for SMB endpoints
- SD card read errors on some AirSense 11 devices

## Issues Fixed

### 🔧 Configuration Parsing Fix
**Problem:** SMB endpoint paths starting with `//` (e.g., `//192.168.1.121/CpapData`) were incorrectly parsed as comments and truncated to empty strings, causing "ENDPOINT is empty" validation errors.

**Solution:** Removed support for `//` style comments in configuration files. The parser now only recognizes `#` for comments, preserving SMB UNC paths and URLs that begin with `//`.

**Files Changed:**
- `src/Config.cpp`: Removed `//` comment stripping logic from `trimComment()`

### 🔧 SD Card Bus Conflict Fix
**Problem:** The ESP32's internal pull-up resistor on the CS_SENSE pin (GPIO 33) was interfering with the CPAP machine's SD card bus signaling, causing "read error" notifications on some AirSense 11 devices.

**Solution:** Changed CS_SENSE pin initialization from `INPUT_PULLUP` to `INPUT` (floating) to rely on external pull-ups and prevent electrical interference with the CPAP machine.

**Files Changed:**
- `src/main.cpp`: Changed `pinMode(CS_SENSE, INPUT)`
- `src/SDCardManager.cpp`: Changed `pinMode(CS_SENSE, INPUT)`
- `src/TrafficMonitor.cpp`: Changed `pinMode(_pin, INPUT)`

## Technical Details

### Configuration Parser Changes
The configuration file format now supports only hash (`#`) comments:
```ini
# This is a valid comment
ENDPOINT = //192.168.1.121/CPAP_Backups  # This will work correctly

# Previously, this line would be truncated:
# ENDPOINT = //192.168.1.121/CPAP_Backups  # Would become empty
```
