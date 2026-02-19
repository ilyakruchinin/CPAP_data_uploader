# Development Guide

This document is for developers who want to build, modify, or contribute to the ESP32 CPAP Data Uploader project.

## Table of Contents
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Development Setup](#development-setup)
- [Building](#building)
- [Testing](#testing)
- [Release Process](#release-process)
- [Contributing](#contributing)

---

## Architecture

### Core Components

- **Config** - Manages configuration from SD card (`config.txt`) and secure credential storage
- **SDCardManager** - Handles SD card sharing with CPAP machine
- **WiFiManager** - Manages WiFi station mode connection
- **FileUploader** - Orchestrates file upload to remote endpoints

### Upload Management

- **UploadStateManager** - Tracks which files/folders have been uploaded using checksums
- **ScheduleManager** - Manages upload scheduling with NTP time synchronization
- **UploadFSM** - Finite state machine controlling the upload lifecycle (IDLE → LISTENING → ACQUIRING → UPLOADING → RELEASING → COOLDOWN)

### Upload Backends

- **SMBUploader** - Uploads files to SMB/CIFS shares (Windows, NAS, Samba)
- **WebDAVUploader** - Uploads to WebDAV servers (TODO: placeholder)
- **SleepHQUploader** - Direct upload to SleepHQ cloud service via REST API with OAuth authentication

### Supporting Components

- **Logger** - Circular buffer logging system with web API access
- **WebServer** - Optional web server for development/testing

### Power Management (v0.4.3+)

The system includes configurable power management to reduce current consumption during active use:

- **CPU Frequency Scaling** - Configurable from 80-240MHz via `CPU_SPEED_MHZ`
- **WiFi TX Power Control** - Adjustable transmission power via `WIFI_TX_PWR` (high/mid/low)
- **WiFi Power Saving** - Modem sleep modes via `WIFI_PWR_SAVING` (none/mid/max)

Power settings are applied automatically during startup and maintain full web server functionality. The implementation uses type-safe enums with validation and fallback to safe defaults.

### Design Principles

1. **Dependency Injection** - Components receive dependencies via constructor
2. **Single Responsibility** - Each class has one clear purpose
3. **Testability** - Core logic is unit tested
4. **Feature Flags** - Backends are conditionally compiled
5. **Resource Management** - Careful memory and flash usage
6. **Security by Default** - Credentials stored securely in flash memory unless explicitly disabled

---

## Project Structure

```
├── src/                      # Main application code
│   ├── main.cpp             # Application entry point
│   ├── Config.cpp           # Configuration management
│   ├── SDCardManager.cpp    # SD card control
│   ├── WiFiManager.cpp      # WiFi connection handling
│   ├── FileUploader.cpp     # File upload orchestration
│   ├── UploadStateManager.cpp # Upload state tracking
│   ├── ScheduleManager.cpp    # Upload scheduling
│   ├── SMBUploader.cpp        # SMB upload implementation
│   ├── WebServer.cpp      # Web server (optional)
│   ├── Logger.cpp             # Circular buffer logging
│   ├── WebDAVUploader.cpp     # WebDAV upload (placeholder)
│   └── SleepHQUploader.cpp    # SleepHQ cloud upload (OAuth, multipart, TLS)
├── include/                  # Header files
│   ├── pins_config.h        # Pin definitions for SD WIFI PRO
│   └── *.h                  # Component headers
├── test/                     # Unit tests
│   ├── test_config/
│   ├── test_credential_migration/
│   ├── test_logger_circular_buffer/
│   ├── test_native/
│   ├── test_schedule_manager/
│   ├── test_upload_state_manager/
│   └── mocks/               # Mock implementations
├── components/               # ESP-IDF components (not in git)
│   └── libsmb2/             # SMB2/3 client library (cloned by setup script)
├── scripts/                  # Build and release scripts
│   ├── setup_libsmb2.sh     # Setup SMB library
│   └── prepare_release.sh   # Create release packages
├── release/                  # Release package files
│   ├── upload.sh            # macOS/Linux upload script
│   ├── upload.bat           # Windows upload script
│   └── README.md            # End-user documentation
├── docs/                     # Developer documentation
├── setup.sh                 # Quick project setup
├── build_upload.sh          # Quick build and upload
├── monitor.sh               # Quick serial monitor
└── platformio.ini           # PlatformIO configuration
```

---

## Development Setup

### Prerequisites

- Python 3.7 or later
- Git
- USB drivers for ESP32 (CH340 or CP210x)

### Quick Setup

Run the automated setup script:

```bash
./setup.sh
```

This will:
1. Create Python virtual environment
2. Install PlatformIO and esptool
3. Clone and configure libsmb2 component
4. Install all dependencies

### Manual Setup

If you prefer manual setup:

```bash
# 1. Create Python venv
python3 -m venv venv

# 2. Install dependencies
source venv/bin/activate
pip install -r requirements.txt

# 3. Setup libsmb2 component (required for SMB upload)
./scripts/setup_libsmb2.sh

# 4. Install library dependencies
pio pkg install
```

### Project Cleanup

To clean build artifacts and start fresh:

```bash
./clean.sh
```

This removes:
- Build artifacts (`.pio/build`, `.pio/libdeps`)
- Python virtual environment (`venv/`)
- libsmb2 component (`components/libsmb2/`)
- Temporary files (`*.tmp`, `*.log`)
- Firmware binaries

### IDE Setup

The project works with:
- **PlatformIO IDE** (VS Code extension) - Recommended
- **Command line** - All features available via CLI

---

## Configuration

### Power Management Settings

The system supports configurable power management through `config.txt`:

```ini
CPU_SPEED_MHZ = 160        # CPU frequency: 80-240MHz (default: 240)
WIFI_TX_PWR = mid          # WiFi TX power: "high"/"mid"/"low" (default: "high")  
WIFI_PWR_SAVING = mid      # WiFi power save: "none"/"mid"/"max" (default: "none")
```

**Implementation Details:**
- Settings are validated with fallback to safe defaults
- Applied automatically during startup in `main.cpp`
- Uses type-safe enums internally (`WifiTxPower`, `WifiPowerSaving`)
- WiFi settings applied after successful connection

**Power Consumption Impact:**
- CPU 240→80MHz: ~40-60mA reduction
- WiFi TX high→low: ~15-20mA reduction
- WiFi power save max: ~50-80mA reduction

**Development Notes:**
- Enum values avoid Arduino macro conflicts (e.g., `POWER_HIGH` vs `HIGH`)
- Helper methods convert between string config and enum values
- WiFiManager provides methods for dynamic power mode switching

### Cloud Upload (SleepHQ)

The system supports direct upload to SleepHQ cloud service via REST API. This can be used standalone or alongside SMB upload for dual-backend operation.

#### Enabling Cloud Upload

1. **Build flag:** Uncomment `-DENABLE_SLEEPHQ_UPLOAD` in `platformio.ini`
2. **Configuration:** Add cloud fields to `config.txt` on the SD card

#### Cloud-Only Configuration

```json
{
  "WIFI_SSID": "MyNetwork",
  "WIFI_PASS": "wifi_password",
  "ENDPOINT_TYPE": "CLOUD",
  "CLOUD_CLIENT_ID": "your-sleephq-client-id",
  "CLOUD_CLIENT_SECRET": "your-sleephq-client-secret",
  "GMT_OFFSET_HOURS": -8
}
```

#### Dual-Backend Configuration (SMB + Cloud)

Upload to both a local NAS and SleepHQ simultaneously:

```json
{
  "WIFI_SSID": "MyNetwork",
  "WIFI_PASS": "wifi_password",
  "ENDPOINT": "//192.168.1.100/share/cpap",
  "ENDPOINT_TYPE": "SMB,CLOUD",
  "ENDPOINT_USER": "smbuser",
  "ENDPOINT_PASS": "smbpass",
  "CLOUD_CLIENT_ID": "your-sleephq-client-id",
  "CLOUD_CLIENT_SECRET": "your-sleephq-client-secret",
  "GMT_OFFSET_HOURS": -8
}
```

#### Cloud Configuration Fields

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `CLOUD_CLIENT_ID` | Yes (for cloud) | — | SleepHQ OAuth client ID |
| `CLOUD_CLIENT_SECRET` | Yes (for cloud) | — | SleepHQ OAuth client secret (auto-migrated to flash) |
| `CLOUD_TEAM_ID` | No | auto-discovered | SleepHQ team ID (discovered via `/api/v1/me` if omitted) |
| `CLOUD_DEVICE_ID` | No | `0` | SleepHQ device ID (sent with import creation) |
| `CLOUD_BASE_URL` | No | `https://sleephq.com` | API base URL |
| `CLOUD_INSECURE_TLS` | No | `false` | Skip TLS certificate validation (not recommended) |
| `MAX_DAYS` | No | `365` | Only upload DATALOG folders from the last N days (supported range: 1-366) |

#### Upload Flow

The cloud upload follows the SleepHQ import lifecycle:

```
Session Start
  ├─ OAuth authenticate (client_id + client_secret)
  ├─ Discover team_id (if not configured)
  ├─ Create import session
  │
  ├─ For each file:
  │   ├─ Compute content_hash = MD5(file_content + filename)
  │   ├─ Multipart POST: name, path, content_hash, file
  │   └─ Track bytes transferred
  │
  └─ Process import (triggers SleepHQ server-side processing)
```

#### TLS Security

- **Default:** ISRG Root X1 (Let's Encrypt) root CA certificate is embedded in firmware for TLS validation of `sleephq.com`
- **Insecure fallback:** Set `CLOUD_INSECURE_TLS: true` to disable certificate validation (useful for testing with proxies, not recommended for production)
- The embedded certificate expires June 4, 2035

#### Content Hash

SleepHQ uses `content_hash` for server-side deduplication. The hash is computed as:

```
content_hash = MD5(file_content + filename)
```

Where `filename` is the bare filename without path (e.g., `BRP.edf`).

#### MAX_DAYS Filtering

To limit uploads to recent data only (useful for initial setup with large historical data):

```json
{
  "MAX_DAYS": 30
}
```

This skips DATALOG folders older than 30 days. The filter compares the folder name (YYYYMMDD format) against the current date minus `MAX_DAYS`. Requires NTP time sync; if time is unavailable, all folders are processed. This setting affects **all backends** (SMB and Cloud).

#### Credential Security

`CLOUD_CLIENT_SECRET` follows the same secure storage pattern as other credentials:
- Automatically migrated from `config.txt` to ESP32 flash (NVS) on first boot
- Replaced with `***STORED_IN_FLASH***` in `config.txt`
- Loaded from NVS on subsequent boots
- Protected from SD card physical access

#### Memory Impact

Cloud upload adds approximately:
- **Flash:** +110KB over base (with ISRG Root X1 CA cert)
- **RAM:** +264 bytes static; upload buffers (4KB) allocated during file transfer
- **Dual-backend (SMB + Cloud):** Flash ~40%, RAM ~14.9% of available

---

## Building

### Quick Build & Upload Script

The `build_upload.sh` script supports both firmware types and separate build/upload steps:

```bash
# Build and upload standard firmware (default)
./build_upload.sh both pico32

# Build and upload OTA firmware
./build_upload.sh both pico32-ota

# Build only (no sudo required)
./build_upload.sh build pico32
./build_upload.sh build pico32-ota

# Upload only (requires sudo, must build first)
./build_upload.sh upload pico32
./build_upload.sh upload pico32-ota

# Upload to specific port
./build_upload.sh upload pico32 /dev/ttyUSB0

# Show help
./build_upload.sh --help
```

**Benefits of separate steps:**
- Build without sudo (faster development)
- Upload only when needed (saves time)
- Specify custom serial ports
- Better error isolation

### Build Targets

The project supports two firmware configurations:

#### Standard Build (`pico32`)
```bash
pio run -e pico32
```
- **3MB app space** (maximum firmware size)
- **No OTA support** - updates require physical access
- **Best for**: Development, maximum feature space

#### OTA-Enabled Build (`pico32-ota`)
```bash
pio run -e pico32-ota
```
- **1.5MB app space** per partition (2 partitions for OTA)
- **Web-based firmware updates** via `/ota` interface
- **Best for**: Production deployments requiring remote updates

### Manual Build

```bash
source venv/bin/activate
pio run -e pico32              # Build standard firmware
pio run -e pico32-ota          # Build OTA firmware
sudo pio run -e pico32 -t upload    # Upload (requires sudo)
```

### Build Targets

```bash
pio run -e pico32              # Build standard firmware
pio run -e pico32-ota          # Build OTA firmware
pio run -e pico32 -t upload    # Upload standard firmware
pio run -e pico32-ota -t upload # Upload OTA firmware
pio run -e pico32 -t clean     # Clean build
pio run -e pico32 -t size      # Show memory usage
pio run -e pico32 -t erase     # Erase flash
```

### Build Configuration

Edit `platformio.ini` to configure:

**Feature Flags:**
```ini
build_flags = 
    -DENABLE_SMB_UPLOAD          ; Enable SMB/CIFS upload
    ; -DENABLE_WEBDAV_UPLOAD     ; Enable WebDAV (TODO)
    ; -DENABLE_SLEEPHQ_UPLOAD    ; Enable Cloud/SleepHQ upload (HTTPS + OAuth)
    -DENABLE_WEBSERVER      ; Enable web server
```

Multiple upload backends can be enabled simultaneously. Use `ENDPOINT_TYPE` in `config.txt` to select active backends at runtime (e.g., `"SMB"`, `"CLOUD"`, or `"SMB,CLOUD"`).

**Logging:**
```ini
build_flags =
    -DLOG_BUFFER_SIZE=32768      ; 32KB log buffer (default: 2KB)
    -DCORE_DEBUG_LEVEL=3         ; ESP32 core debug level (0-5)
    -DENABLE_VERBOSE_LOGGING     ; Enable debug logs (compiled out by default)
```

**Debug Logging:** By default, `LOG_DEBUG()` and `LOG_DEBUGF()` macros are compiled out (zero overhead). Enable with `-DENABLE_VERBOSE_LOGGING` to see detailed diagnostics including progress updates, state details, and troubleshooting information. Saves ~10-15KB flash and ~35-75ms per upload session when disabled.

**SD Card Logging:** For advanced debugging, logs can be written to SD card by setting `LOG_TO_SD_CARD: true` in `config.txt`. 

⚠️ **WARNING: SD card logging is for debugging only and can prevent the CPAP machine from reliably accessing the SD card. Only enable it temporarily for troubleshooting, and only when `UPLOAD_MODE` is `"scheduled"` with an upload window outside normal therapy times. Disable it immediately afterward.**

When enabled:
- Logs are written to `/debug_log.txt` on the SD card
- All log messages (including serial and buffer logs) are also written to the file
- File is opened in append mode for each log message
- If file creation fails, SD logging is automatically disabled
- Can interfere with or block CPAP machine SD card access

### Memory Usage

Current build sizes (with SMB enabled):

#### Standard Build (`pico32`)
- **Flash:** ~33.9% (1,066,637 / 3,145,728 bytes) - 3MB app partition
- **RAM:** ~14.5% (47,600 / 327,680 bytes) - Static allocation

#### OTA Build (`pico32-ota`)
- **Flash:** ~77.8% (1,223,321 / 1,572,864 bytes) - 1.5MB app partition
- **RAM:** ~14.9% (48,776 / 327,680 bytes) - Static allocation

**Dynamic Memory Analysis:** Upload state now uses v2 line-based snapshots with separate files per backend (`.upload_state.v2.smb`/`.cloud`) plus append-only journals (`.log`) with fixed-size in-memory structures. This avoids large `DynamicJsonDocument` allocations and reduces heap churn/fragmentation during frequent state updates.

---

## Testing

### Unit Tests

Run all tests:
```bash
source venv/bin/activate
pio test -e native
```

Run specific test suite:
```bash
pio test -e native -f test_config
pio test -e native -f test_schedule_manager
pio test -e native -f test_upload_state_manager
```

### Test Coverage

- `test_config`: 33 tests - Configuration parsing and validation  
- `test_credential_migration`: 6 tests - Secure credential storage
- `test_logger_circular_buffer`: Logger circular buffer tests
- `test_native`: 9 tests - Mock infrastructure
- `test_upload_state_manager`: 42 tests - Upload state tracking
- `test_schedule_manager`: 22 tests - Scheduling and NTP

### Hardware Testing

See [Hardware Testing Checklist](#hardware-testing-checklist) below.

---

## Release Process

### Creating a Release Package

```bash
./scripts/prepare_release.sh
```

This creates a timestamped zip file in `release/` containing:
- `firmware.bin` - Precompiled firmware
- `upload.sh` - macOS/Linux upload script
- `upload.bat` - Windows upload script
- `README.md` - End-user documentation

### Windows Release

For Windows releases, download `esptool.exe` from [espressif/esptool releases](https://github.com/espressif/esptool/releases) and place it in `release/` before running `prepare_release.sh`.

### Version Tagging

Use semantic versioning (MAJOR.MINOR.PATCH):

```bash
git tag -a v0.3.0 -m "Release v0.3.0: Description"
git push origin v0.3.0
```

---

## Hardware Testing Checklist

### Prerequisites

- [ ] SD WIFI PRO dev board connected with SD WIFI PRO inserted
- [ ] `config.txt` created on SD card
- [ ] WiFi network available
- [ ] SMB share accessible and writable (if using SMB)
- [ ] SleepHQ API credentials (if using Cloud upload)
- [ ] Internet access (required for NTP server and Cloud upload)

### Test Procedure

1. **Flash Firmware**
   ```bash
   ./build_upload.sh
   ```

2. **Monitor Serial Output**
   ```bash
   ./monitor.sh
   ```

3. **Verify Startup**
   - [ ] Config loaded successfully
   - [ ] WiFi connected
   - [ ] NTP time synchronized
   - [ ] SMB connection established (if SMB enabled)
   - [ ] Cloud OAuth authentication successful (if Cloud enabled)
   - [ ] Team ID discovered or loaded from config (if Cloud enabled)

4. **Test Upload**
   - [ ] Files uploaded to SMB share (if SMB enabled)
   - [ ] Files uploaded to SleepHQ (if Cloud enabled)
   - [ ] `.upload_state.v2.smb`/`.cloud` and `.upload_state.v2.smb.log`/`.cloud.log` created on SD card (depending on enabled backends)
   - [ ] No errors in serial output

5. **Test Web Interface** (if enabled)
   - [ ] Access `http://<device-ip>/`, you can get the device IP from the serial port after a reset.
   - [ ] Trigger manual upload
   - [ ] View logs
   - [ ] Check status — verify `cloud_configured` field
   - [ ] Check config — verify cloud fields (secret should show `***STORED_IN_FLASH***`)

6. **Test Cloud Upload** (if Cloud enabled)
   - [ ] Verify import created in serial log (`[SleepHQ] Import created: <id>`)
   - [ ] Verify files uploaded with content hash
   - [ ] Verify import processed (`[SleepHQ] Import <id> submitted for processing`)
   - [ ] Check SleepHQ web interface for imported data
   - [ ] Verify `CLOUD_CLIENT_SECRET` censored in `config.txt` after first boot

### Common Issues

See [BUILD_TROUBLESHOOTING.md](BUILD_TROUBLESHOOTING.md) for detailed troubleshooting.

---

## Contributing

### Code Style

- Follow existing code style
- Use meaningful variable names
- Add comments for complex logic (comments are code too)

### Adding Features

1. Write tests first (TDD approach)
2. Implement feature
3. Update documentation
4. Test on hardware

### Adding Upload Backends

To add a new backend (e.g., FTP):

1. Create `include/FTPUploader.h` and `src/FTPUploader.cpp`
2. Wrap with feature flag: `#ifdef ENABLE_FTP_UPLOAD`
3. Add flag to `platformio.ini`
4. Update `FileUploader` to instantiate new backend
5. Add tests
6. Document in `FEATURE_FLAGS.md`

### Pull Request Process

1. Fork the repository
2. Create feature branch
3. Make changes with tests
4. Ensure all tests pass
5. Update documentation
6. Submit pull request

---

## Useful Commands

```bash
# Development
./setup.sh                     # Initial setup
./clean.sh                     # Clean all build artifacts
./build_upload.sh build pico32 # Build standard firmware (no sudo)
./build_upload.sh build pico32-ota # Build OTA firmware (no sudo)
./build_upload.sh upload pico32    # Upload standard firmware (requires sudo)
./build_upload.sh upload pico32-ota # Upload OTA firmware (requires sudo)
./build_upload.sh both pico32      # Build and upload standard
./build_upload.sh both pico32-ota  # Build and upload OTA
./monitor.sh                   # Serial monitor

# PlatformIO
pio run -e pico32              # Build standard firmware
pio run -e pico32-ota          # Build OTA firmware
sudo pio run -e pico32 -t upload    # Upload standard (requires sudo)
sudo pio run -e pico32-ota -t upload # Upload OTA (requires sudo)
pio run -t clean               # Clean
pio test -e native             # Run tests
pio device list                # List serial ports
pio device monitor             # Serial monitor

# Release
./scripts/prepare_release.sh   # Create release package

# Cleanup
./clean.sh                     # Clean build artifacts and venv
```

---

## Credential Security

### Overview

The system supports two credential storage modes:

1. **Secure Mode (Default)** - Credentials stored in ESP32 NVS (Non-Volatile Storage)
2. **Plain Text Mode** - Credentials stored in `config.txt` on SD card

### Preferences Library Usage

The system uses the ESP32 Preferences library (a high-level wrapper around NVS) for secure credential storage.

**Namespace:** `cpap_creds`

**Stored Credentials:**
- `wifi_pass` - WiFi password
- `endpoint_pass` - Endpoint (SMB/WebDAV) password
- `cloud_secret` - Cloud (SleepHQ) OAuth client secret

**Key Methods:**

```cpp
// Initialize Preferences
bool Config::initPreferences() {
    if (!preferences.begin(PREFS_NAMESPACE, false)) {
        LOG("ERROR: Failed to initialize Preferences");
        return false;
    }
    return true;
}

// Store credential
bool Config::storeCredential(const char* key, const String& value) {
    size_t written = preferences.putString(key, value);
    return written > 0;
}

// Load credential
String Config::loadCredential(const char* key, const String& defaultValue) {
    return preferences.getString(key, defaultValue);
}

// Close Preferences
void Config::closePreferences() {
    preferences.end();
}
```

### Migration Process

When secure mode is enabled (default), the system automatically migrates credentials on first boot:

1. **Detection:** System checks if credentials in `config.txt` are censored
2. **Migration:** If plain text detected, credentials are:
   - Stored in NVS using Preferences library
   - Verified by reading back from NVS
   - Censored in `config.txt` (replaced with `***STORED_IN_FLASH***`)
3. **Subsequent Boots:** Credentials loaded directly from NVS

**Migration Flow:**

```
Boot → Load config.txt
  ├─ STORE_CREDENTIALS_PLAIN_TEXT = true?
  │    └─ YES → Use plain text (no migration)
  │
  └─ NO/ABSENT → Check if censored
       ├─ YES → Load from NVS
       └─ NO → Migrate to NVS + Censor config.txt
```

### Error Handling

**Preferences Initialization Failure:**
- System logs error
- Falls back to plain text mode
- Continues operation with credentials from `config.txt`

**NVS Write Failure:**
- System logs detailed error
- Keeps credentials in plain text
- Does not censor `config.txt`

**NVS Read Failure:**
- System logs warning
- Attempts to use `config.txt` values
- May trigger re-migration if plain text available

### Web Interface Protection

The WebServer component respects credential storage mode:

```cpp
void CpapWebServer::handleApiConfig() {
    // ...
    if (config) {
        // Check if credentials are stored in secure mode
        bool credentialsSecured = config->areCredentialsInFlash();
        
        json += "\"wifi_ssid\":\"" + escapeJson(config->getWifiSSID()) + "\",";
        json += "\"wifi_password\":\"***HIDDEN***\",";
        // ...
        json += "\"credentials_secured\":" + String(credentialsSecured ? "true" : "false");
    }
    // ...
}
```

### Security Considerations

**Protected Against:**
- Physical SD card access (credentials not in `config.txt`)
- Web interface credential exposure (censored in responses)
- Serial log exposure (credentials never logged)

**Not Protected Against:**
- Flash memory dumps (requires physical device access + tools)
- Malicious firmware (can read NVS)
- JTAG/SWD debug access (hardware debugging interface)

**Best Practices:**
- Use secure mode for production deployments
- Physically secure the device
- Change credentials if device is lost/stolen
- Use plain text mode only for development

### Performance Impact

**Memory Usage:**
- Preferences object: ~20 bytes
- Storage mode flags: 2 bytes
- Total additional RAM: ~22 bytes

**Boot Time:**
- Migration (first boot only): +100-200ms
- Subsequent boots: No measurable impact

**Flash Wear:**
- NVS writes: Only during migration or credential changes
- Expected writes: 1-10 over device lifetime
- ESP32 flash endurance: 100,000 write cycles
- Conclusion: Not a concern

### Testing Credential Security

**Unit Tests:**
```bash
# Run Config tests (includes Preferences operations)
pio test -e native -f test_config
```

**Hardware Tests:**

1. **Test Secure Mode (Default):**
   ```bash
   # 1. Create config.txt with plain text credentials
   # 2. Set STORE_CREDENTIALS_PLAIN_TEXT = false or omit
   # 3. Flash and boot device
   # 4. Check serial output for migration messages
   # 5. Verify config.txt shows ***STORED_IN_FLASH***
   # 6. Verify WiFi connects and uploads work
   # 7. Check web interface shows censored values
   ```

2. **Test Plain Text Mode:**
   ```bash
   # 1. Set STORE_CREDENTIALS_PLAIN_TEXT = true
   # 2. Flash and boot device
   # 3. Verify credentials remain in config.txt
   # 4. Verify web interface shows actual values
   ```

3. **Test Migration:**
   ```bash
   # 1. Start with plain text config
   # 2. Change STORE_CREDENTIALS_PLAIN_TEXT to false
   # 3. Reboot device
   # 4. Verify migration occurs
   # 5. Verify system continues to work
   ```

## Architecture Decisions

### Why Preferences Library?

- High-level wrapper around ESP32 NVS
- Simple key-value API
- Automatic namespace management
- Built into ESP32 Arduino Core
- No external dependencies

### Why libsmb2?

- Full SMB2/3 protocol support
- Mature, well-tested library
- Acceptable binary footprint (~220-270KB)
- Active maintenance

### Why Exclusive Access FSM?

CPAP machines need regular SD card access. The FSM-based exclusive access model ensures:
- SD card bus inactivity is confirmed before taking control
- Upload time is bounded by configurable exclusive access minutes
- Cooldown periods between upload cycles give the CPAP machine uninterrupted access
- Uploads resume automatically across multiple cycles

### Why Circular Buffer Logging?

- Fixed memory usage (configurable)
- No SD card writes (reduces wear)
- Thread-safe for dual-core ESP32

### Why Feature Flags?

- Minimize binary size
- Enable only needed backends
- Faster compilation
- Cleaner code separation

### Why Embedded Root CA Certificate?

The SleepHQ cloud uploader embeds the ISRG Root X1 (Let's Encrypt) root CA certificate directly in firmware:
- Avoids dependency on external certificate stores
- ESP32 has no system CA bundle by default
- ISRG Root X1 covers `sleephq.com` and most modern HTTPS sites
- Certificate expires 2035 — well beyond expected device lifetime
- Optional `CLOUD_INSECURE_TLS` fallback for development/testing

### Why Import Lifecycle?

SleepHQ requires an import session workflow (create → upload files → process):
- Server-side deduplication via `content_hash` per file
- Batch processing after all files are uploaded
- `content_hash = MD5(file_content + filename)` matches SleepHQ's expected format
- Import is created at session start and processed at session end
- If the session ends abnormally, partial uploads are still valid on the server

---

## Performance Considerations

### Flash Usage

- Base firmware: ~800KB
- SMB backend: +220-270KB
- Cloud/SleepHQ backend: +110KB (includes ISRG Root X1 CA cert)
- WebDAV backend: +50-80KB (estimated)
- Dual-backend (SMB + Cloud): ~1,258KB total
- **Standard build**: 3MB available (huge_app partition)
- **OTA build**: 1.5MB available per partition

### RAM Usage

- Static allocation: ~47KB
- Log buffer: 32KB (configurable)
- SMB buffers: ~32KB during upload
- Total available: 320KB

### Upload Performance

- Actual rate: Varies by network and share, during tests the transfer achieved 130KB/s
- Upload time bounded by EXCLUSIVE_ACCESS_MINUTES config parameter
- Multiple upload cycles separated by COOLDOWN_MINUTES

---

## References

- [BUILD_TROUBLESHOOTING.md](BUILD_TROUBLESHOOTING.md) - Build issues
- [FEATURE_FLAGS.md](FEATURE_FLAGS.md) - Backend selection
- [LIBSMB2_INTEGRATION.md](LIBSMB2_INTEGRATION.md) - SMB integration details
- [PlatformIO Docs](https://docs.platformio.org/)
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
- [libsmb2 GitHub](https://github.com/sahlberg/libsmb2)

