# Release Notes v0.8.0

## ⚠️ BREAKING CHANGE: Configuration Format

**Configuration file format has changed from JSON to Key-Value format.**

### What Changed:
- **Old format (v0.7.x):** `config.json` (JSON format)
- **New format (v0.8.0+):** `config.txt` (simple Key-Value format)

### Migration Required:
**Your existing `config.json` will NOT work with v0.8.0!** You must create a new `config.txt` file.

**Steps to migrate:**
1. ❌ Delete or rename your old `config.json`
2. ✅ Create a new `config.txt` file in the root of your SD card
3. ✅ Use the Key-Value format: `SETTING_NAME = value` (one per line)
4. ✅ See configuration examples in the package (`config.txt.example*`)

**Example conversion:**
```
Old (config.json):          New (config.txt):
{                           WIFI_SSID = MyNetwork
  "WIFI_SSID": "MyNetwork", WIFI_PASSWORD = mypassword
  "WIFI_PASSWORD": "pwd",   ENDPOINT_TYPE = SMB
  "ENDPOINT_TYPE": "SMB"    ENDPOINT = //192.168.1.10/share
}                           ENDPOINT_USER = username
                            ENDPOINT_PASSWORD = password
```

### Why This Change?
- **More user-friendly** - No JSON syntax errors
- **Simpler format** - Just `KEY = value`, one per line
- **Removed dependency** - No longer requires ArduinoJson library
- **Reduced memory usage** - Lighter parsing and smaller footprint
- **Better error messages** - Clearer validation and troubleshooting

---

## ✨ New Features

### 🎨 Improved Web Interface
- **Web dashboard screenshot** added to documentation
- **Dual upload support highlighted** in Quick Start guide
- Access device at **`http://cpap.local`** (mDNS)
- Real-time upload status and progress monitoring

### 📝 Enhanced Configuration Examples
- **4 config templates** for different use cases:
  - `config.txt.example.simple` - Minimal (6-10 lines, bare essentials)
  - `config.txt.example.smb` - SMB/network share configuration
  - `config.txt.example.sleephq` - SleepHQ cloud upload
  - `config.txt.example.both` - Dual upload (SMB + SleepHQ simultaneously)

### 📖 Comprehensive Documentation Improvements
- **Table of Contents** added to both main README and User Guide
- **"Get Started in 3 Steps"** quick start section
- **Minimal config examples** prominently featured
- Eliminated duplicate content and improved navigation
- Clear separation of end-user vs developer documentation
- Web interface features and screenshots

---

## 🔧 Technical Improvements

### Configuration System Rewrite
- Complete rewrite of config parser (`Config.cpp`)
- Removed ArduinoJson dependency
- Simple line-by-line Key-Value parsing
- Support for comments (`#` and `//`)
- Case-insensitive keys
- Better error reporting and validation

### Web Server Enhancements
- Fixed `/api/config` JSON formatting bug (missing cloud config section)
- Fixed trailing commas in JSON output
- Proper chunked transfer encoding for all API endpoints
- `/config` and `/status` pages now display formatted JSON from API endpoints

### Code Quality
- Reduced codebase by ~322 lines (net reduction despite new features)
- Eliminated ArduinoJson library usage in config handling
- Cleaner, more maintainable parsing logic
- Better memory efficiency

---

## 📁 Configuration File Changes

### New Config File: `config.txt`

**Minimal SMB Example:**
```ini
WIFI_SSID = YourWiFiName
WIFI_PASSWORD = YourWiFiPassword
ENDPOINT_TYPE = SMB
ENDPOINT = //192.168.1.100/cpap_backups
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password
```

**Minimal SleepHQ Example:**
```ini
WIFI_SSID = YourWiFiName
WIFI_PASSWORD = YourWiFiPassword
ENDPOINT_TYPE = CLOUD
CLOUD_CLIENT_ID = your-client-id
CLOUD_CLIENT_SECRET = your-client-secret
```

**Dual Upload (SMB + SleepHQ):**
```ini
WIFI_SSID = YourWiFiName
WIFI_PASSWORD = YourWiFiPassword
ENDPOINT_TYPE = SMB,CLOUD
ENDPOINT = //192.168.1.100/cpap_backups
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password
CLOUD_CLIENT_ID = your-client-id
CLOUD_CLIENT_SECRET = your-client-secret
```

**All other settings are optional and have smart defaults!**

---

## 🐛 Bug Fixes

- Fixed `/api/config` endpoint not sending cloud configuration section
- Fixed invalid JSON output (trailing commas) in web API responses
- Fixed web interface `/config` page stuck on "Loading..."
- Updated all documentation references from `config.json` to `config.txt`
- Fixed inconsistent config example file naming

---

## 📚 Documentation Updates

**Updated Files:**
- `README.md` - Complete restructure with TOC, removed duplication
- `release/README.md` - Added TOC, "Get Started in 3 Steps" section
- `docs/DEVELOPMENT.md` - Updated all config references
- `docs/FEATURE_FLAGS.md` - Updated configuration examples
- `docs/02-ARCHITECTURE.md` - Updated config file references
- `docs/03-REQUIREMENTS.md` - Removed backward compatibility requirement
- All other documentation files updated to reference `config.txt`

**New Documentation:**
- `docs/config.txt.example.simple` - Minimal configuration template
- `docs/config.txt.example.smb` - SMB-focused template (renamed from `config.txt.example`)
- Web interface screenshot and feature documentation

---

## 🔄 Migration Guide

### For Users Upgrading from v0.7.x:

1. **Backup your current setup** (optional but recommended)
   - Copy your existing `config.json` somewhere safe
   - Note your current settings

2. **Flash v0.8.0 firmware** using the included upload scripts

3. **Create new `config.txt`** on your SD card
   - Use one of the provided templates as a starting point
   - Copy your settings from old `config.json` to new `config.txt`
   - Follow the Key-Value format: `SETTING_NAME = value`

4. **Insert SD card** and power on
   - Device will automatically detect and use `config.txt`
   - Credentials will be migrated to secure storage (if enabled)

5. **Verify operation**
   - Check web interface at `http://cpap.local`
   - View logs to confirm configuration loaded correctly
   - Test upload functionality

### Settings That Haven't Changed:
All setting **names** remain the same, only the **file format** changed:
- WiFi settings (SSID, password, hostname)
- Upload destination (endpoint type, SMB/cloud credentials)
- Upload behavior (mode, schedule, timing)
- Power management
- Timezone offset

---

## ⚙️ System Requirements

**Unchanged from v0.7.x:**
- ESP32-PICO-D4 (SD WIFI PRO)
- 4MB Flash memory
- 2.4GHz WiFi network
- ResMed Series 9, 10, or 11 CPAP machine
- SMB/CIFS share OR SleepHQ account

---

## 📊 Build Information

**Firmware Sizes:**
- OTA build: ~1.31 MB (flash: 41.7%, RAM: 16.3%)
- Standard build: ~1.31 MB (flash: 41.7%, RAM: 16.3%)

**Build Environments:**
- `pico32` - Standard build (3MB app space, no OTA)
- `pico32-ota` - OTA-enabled build (1.5MB app space, web updates)

---

## 🙏 Acknowledgments

Thank you to all users who provided feedback and testing!

---

## 📝 Known Issues

None reported for v0.8.0.

---

## 🔗 Links

- **Download:** [v0.8.0 Release](../../releases/tag/v0.8.0)
- **User Guide:** [release/README.md](README.md)
- **Developer Guide:** [docs/DEVELOPMENT.md](../docs/DEVELOPMENT.md)
- **Issue Tracker:** [GitHub Issues](../../issues)

---

**Upgrade today and enjoy the simpler, more user-friendly configuration format!**
