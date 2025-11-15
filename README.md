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

### Core Components
- **Config** - Manages configuration from SD card
- **SDCardManager** - Handles SD card sharing with CPAP machine
- **WiFiManager** - Manages WiFi station mode connection
- **FileUploader** - Orchestrates file upload to remote endpoints

### Upload Management
- **UploadStateManager** - Tracks which files/folders have been uploaded using checksums and completion status
- **TimeBudgetManager** - Enforces time limits on SD card access to ensure CPAP machine priority
- **ScheduleManager** - Manages daily upload scheduling with NTP time synchronization

### Upload Backends
- **SMBUploader** - Uploads files to SMB/CIFS shares (Windows shares, NAS, Samba)
- **WebDAVUploader** - Uploads to WebDAV servers (TODO: not yet implemented)
- **SleepHQUploader** - Direct upload to SleepHQ service (TODO: not yet implemented)

## Project Structure
```
├── src/                      # Main application code
│   ├── main.cpp             # Application entry point
│   ├── Config.cpp           # Configuration management
│   ├── SDCardManager.cpp    # SD card control
│   ├── WiFiManager.cpp      # WiFi connection handling
│   ├── FileUploader.cpp     # File upload orchestration
│   ├── UploadStateManager.cpp # Upload state tracking
│   ├── TimeBudgetManager.cpp  # Time budget enforcement
│   ├── ScheduleManager.cpp    # Upload scheduling
│   ├── SMBUploader.cpp        # SMB upload implementation
│   ├── WebDAVUploader.cpp     # WebDAV upload (placeholder)
│   └── SleepHQUploader.cpp    # SleepHQ upload (placeholder)
├── include/                  # Header files
│   ├── pins_config.h        # Pin definitions for SD WIFI PRO
│   ├── Config.h
│   ├── SDCardManager.h
│   ├── WiFiManager.h
│   ├── FileUploader.h
│   ├── UploadStateManager.h
│   ├── TimeBudgetManager.h
│   ├── ScheduleManager.h
│   ├── SMBUploader.h
│   ├── WebDAVUploader.h
│   └── SleepHQUploader.h
├── test/                     # Unit tests
│   ├── test_native/         # Native environment tests
│   └── mocks/               # Mock implementations for testing
├── components/               # ESP-IDF components
│   └── libsmb2/             # SMB2/3 client library (git submodule)
├── .kiro/                    # Kiro IDE configuration
│   └── specs/               # Feature specifications
│       └── file-tracking-and-upload-scheduling/
│           ├── requirements.md
│           ├── design.md
│           └── tasks.md
├── venv/                     # Python virtual environment
├── docs/                     # Documentation
├── platformio.ini           # PlatformIO configuration
└── README.md               # This file
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
  "ENDPOINT": "//192.168.1.100/cpap_backups",
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

### Configuration Fields

#### Network Settings
- **WIFI_SSID**: SSID of the WiFi network to connect to (required)
- **WIFI_PASS**: Password for the WiFi network (required)

#### Endpoint Settings
- **ENDPOINT**: Remote location where files will be uploaded (required)
- **ENDPOINT_TYPE**: Type of endpoint - `SMB`, `WEBDAV`, or `SLEEPHQ`
  - `SMB`: Windows/Samba share - Format: `//server/share` (e.g., `//192.168.1.100/backups`) - ✅ Implemented
  - `WEBDAV`: WebDAV share (e.g., NextCloud) - Format: `http://address/folder` - ⏳ TODO
  - `SLEEPHQ`: Direct upload to SleepHQ - ⏳ TODO
- **ENDPOINT_USER**: Username for the remote endpoint (required)
- **ENDPOINT_PASS**: Password for the remote endpoint (required)

#### Schedule Settings
- **UPLOAD_HOUR**: Hour of day (0-23) when uploads should occur (default: 12 = noon)
  - Uploads happen once per day at this hour
  - Uses NTP-synchronized time

#### Time Budget Settings
- **SESSION_DURATION_SECONDS**: Maximum time (in seconds) to hold SD card access per session (default: 5)
  - Keeps sessions short to ensure CPAP machine can access card when needed
  - System will resume upload in next session if time budget is exhausted
- **MAX_RETRY_ATTEMPTS**: Number of retry attempts before increasing time budget (default: 3)
  - After this many interrupted uploads, time budget is multiplied by retry count
  - Helps handle large files that don't fit in normal time budget

#### Timezone Settings
- **GMT_OFFSET_SECONDS**: Timezone offset from GMT in seconds (default: 0)
  - Example: -28800 for PST (UTC-8), 3600 for CET (UTC+1)
- **DAYLIGHT_OFFSET_SECONDS**: Daylight saving time offset in seconds (default: 0)
  - Example: 3600 for DST (adds 1 hour)

## How It Works

### Upload Flow

1. **Startup**
   - Device reads `config.json` from SD card
   - Connects to WiFi network
   - Synchronizes time with NTP server
   - Loads upload state from `.upload_state.json` (tracks what's been uploaded)

2. **Scheduled Upload Window**
   - Waits until configured `UPLOAD_HOUR` (once per day)
   - Checks if it's time to upload using NTP-synchronized time

3. **Upload Session**
   - Takes exclusive control of SD card (CPAP machine must wait)
   - Starts time-budgeted session (default: 5 seconds)
   - Uploads files in priority order:
     - **DATALOG folders** (newest first) - therapy data
     - **Root files** (identification.json, identification.crc, SRT.edf)
     - **SETTINGS files** (CurrentSettings.json, CurrentSettings.crc)
   - Releases SD card control after session or budget exhaustion

4. **Smart File Tracking**
   - **DATALOG folders**: Tracks completion status (all files uploaded = complete)
   - **Root/SETTINGS files**: Tracks checksums (only uploads if changed)
   - Saves progress to `.upload_state.json` after each session

5. **Budget Management**
   - If time budget exhausted mid-upload:
     - Saves progress
     - Waits 2x session duration (gives CPAP priority)
     - Resumes upload in same day's window
   - If all files uploaded:
     - Marks upload complete for the day
     - Waits until next day's scheduled time

6. **Retry Logic**
   - Tracks retry attempts for folders that don't complete
   - After `MAX_RETRY_ATTEMPTS`, increases time budget
   - Helps handle large files that need more time

### SD Card Sharing

The device respects CPAP machine access to the SD card:
- Only takes control when needed for uploads
- Keeps sessions short (configurable, default 5 seconds)
- Releases control immediately after session
- Waits between sessions to give CPAP priority
- CPAP machine can access card anytime device doesn't have control

### File Structure on SD Card

```
/
├── config.json              # Your configuration (you create this)
├── .upload_state.json       # Upload tracking (auto-created)
├── identification.json      # CPAP identification
├── identification.crc       # Checksum
├── SRT.edf                  # Summary data
├── DATALOG/                 # Therapy data folders
│   ├── 20241114/           # Date-named folders (YYYYMMDD)
│   │   ├── file1.edf       # Therapy session data
│   │   └── file2.edf
│   └── 20241113/
│       └── file1.edf
└── SETTINGS/                # Settings folder
    ├── CurrentSettings.json
    └── CurrentSettings.crc
```

### Remote Folder Structure

Files are uploaded maintaining the same structure:
```
SMB Share: //server/share/
├── identification.json
├── identification.crc
├── SRT.edf
├── DATALOG/
│   ├── 20241114/
│   │   ├── file1.edf
│   │   └── file2.edf
│   └── 20241113/
│       └── file1.edf
└── SETTINGS/
    ├── CurrentSettings.json
    └── CurrentSettings.crc
```

## Development Status

### ✅ Implemented (v0.2.0)
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
- ✅ Unit tests for core components (Config, UploadStateManager, TimeBudgetManager, ScheduleManager)
- ✅ Comprehensive error handling and logging

### ⏳ In Progress
- 🔄 Hardware testing and validation
- 🔄 Integration testing on real CPAP data

### 📋 TODO (Future Releases)
- ⏳ WebDAV upload implementation (placeholder created)
- ⏳ SleepHQ upload implementation (placeholder created)
- ⏳ Status LED indicators
- ⏳ Low power mode when idle
- ⏳ Web interface for configuration and monitoring
- ⏳ OTA (Over-The-Air) firmware updates

## Testing

### Unit Tests

Run unit tests in native environment:
```bash
pio test -e native
```

Tests cover:
- Configuration parsing and validation
- Upload state management (checksums, folder tracking, retry logic)
- Time budget enforcement and transmission rate calculation
- Schedule management and NTP time handling

### Hardware Testing Checklist

Before testing on hardware, ensure:

1. **Hardware Setup**
   - [ ] ESP32 board connected
   - [ ] SD card with CPAP data structure
   - [ ] SD card reader/writer connected

2. **Configuration**
   - [ ] `config.json` created on SD card root
   - [ ] WiFi credentials configured
   - [ ] SMB share details configured
   - [ ] Upload schedule configured

3. **Network Setup**
   - [ ] WiFi network available
   - [ ] SMB share accessible and writable
   - [ ] NTP server accessible (pool.ntp.org)
   - [ ] No firewall blocking SMB ports (445, 139)

4. **Flash and Monitor**
   ```bash
   pio run -e pico32 -t upload
   pio device monitor -e pico32
   ```

5. **Verify**
   - [ ] SD card detected
   - [ ] Config loaded successfully
   - [ ] WiFi connected
   - [ ] NTP time synchronized
   - [ ] SMB connection established
   - [ ] Files uploaded to SMB share
   - [ ] `.upload_state.json` created on SD card

See [Hardware Testing Guide](#hardware-testing-guide) below for detailed testing procedures.

## Hardware Testing Guide

### Quick Start Test

1. **Prepare Test SD Card**
   ```
   /config.json              # Your configuration
   /DATALOG/
     20241114/
       test.edf              # Small test file (1-2 KB)
   ```

2. **Set Upload Time to Now**
   In `config.json`, set `UPLOAD_HOUR` to current hour for immediate testing:
   ```json
   {
     "UPLOAD_HOUR": 14,  // Set to current hour
     ...
   }
   ```

3. **Flash and Monitor**
   ```bash
   pio run -e pico32 -t upload && pio device monitor -e pico32
   ```

4. **Watch Serial Output**
   Look for:
   - "Configuration loaded successfully"
   - "WiFi connected"
   - "Time synchronized successfully"
   - "Upload Window Active"
   - "Upload session completed successfully"

5. **Verify Upload**
   - Check SMB share for uploaded files
   - Check SD card for `.upload_state.json`

### Integration Test Scenarios

#### Test 1: Complete Upload Flow
- **Goal**: Verify full upload cycle
- **Setup**: Small DATALOG folder with 2-3 files
- **Expected**: All files uploaded, folder marked complete in state file

#### Test 2: Time Budget Exhaustion
- **Goal**: Verify budget enforcement and retry
- **Setup**: Large files, short session duration (2 seconds)
- **Expected**: Session interrupted, waits 2x duration, resumes

#### Test 3: Checksum Change Detection
- **Goal**: Verify root/SETTINGS file tracking
- **Setup**: Upload once, modify identification.json, upload again
- **Expected**: Only changed file re-uploaded

#### Test 4: Retry Logic
- **Goal**: Verify retry count and budget multiplier
- **Setup**: Very large file, short budget, max_retry_attempts=2
- **Expected**: After 2 retries, budget increases

#### Test 5: Schedule Enforcement
- **Goal**: Verify once-per-day uploads
- **Setup**: Set UPLOAD_HOUR to past hour
- **Expected**: Waits until next day's scheduled time

### Troubleshooting

**SD Card Not Detected**
- Check wiring and voltage (3.3V)
- Verify SD card is formatted (FAT32)
- Check serial output for error messages

**WiFi Connection Fails**
- Verify SSID and password in config.json
- Check WiFi network is 2.4GHz (ESP32 doesn't support 5GHz)
- Ensure network is in range

**NTP Sync Fails**
- Check internet connectivity
- Try different NTP server in ScheduleManager.cpp
- Verify firewall allows NTP (UDP port 123)

**SMB Connection Fails**
- Verify SMB share is accessible from network
- Test credentials from another device
- Check firewall allows SMB ports (445, 139)
- Ensure share has write permissions

**Upload Fails Mid-Session**
- Check available space on SMB share
- Verify network stability
- Increase SESSION_DURATION_SECONDS if files are large
- Check serial output for specific error messages

## References

### Hardware
- [SD WIFI PRO](https://www.fysetc.com/products/fysetc-upgrade-sd-wifi-pro-with-card-reader-module-run-wireless-by-esp32-chip-web-server-reader-uploader-3d-printer-parts) - FYSETC SD WIFI PRO hardware used in this project
- [SD WIFI PRO GitHub](https://github.com/FYSETC/SD-WIFI-PRO) - Official hardware repository with schematics and documentation

### Reference Firmware
- [SdWiFiBrowser](https://github.com/FYSETC/SdWiFiBrowser) - Basic WiFi file browser firmware for SD WIFI PRO
- [ESP3D](https://github.com/luc-github/ESP3D) - Advanced 3D printer firmware with WebDAV/FTP support

These projects provided hardware specifications and reference implementations but are not used directly in this project.
