# Build Troubleshooting Guide

This guide helps resolve common build issues with the ESP32 CPAP Data Uploader project.

## Quick Fixes

### Clean Build
If you encounter strange build errors, try a clean build:

```bash
# Use the cleanup script (recommended)
./clean.sh
./setup.sh
./build_upload.sh build pico32

# Or manual cleanup
pio run --target clean
rm -rf .pio/build
pio run -e pico32
```

### Update Dependencies
```bash
pio pkg update
pio lib update
```

### Fresh Environment Setup
```bash
# Complete reset
./clean.sh
./setup.sh
./build_upload.sh build pico32-ota
```

## Common Build Errors

### 1. libsmb2 Related Errors

#### Error: "smb2.h: No such file or directory"

**Cause:** libsmb2 library not found or not properly configured.

**Solution:**
```bash
# Run the setup script to clone and configure libsmb2
./scripts/setup_libsmb2.sh

# Or check if it exists and verify configuration
ls components/libsmb2/
grep "lib_extra_dirs" platformio.ini
# Should output: lib_extra_dirs = components
```

#### Error: "undefined reference to smb2_*"

**Cause:** libsmb2 library not being compiled or linked.

**Solution:**
1. Verify `components/libsmb2/library.json` exists
2. Check that `library_build.py` is present
3. Run setup script: `./scripts/setup_libsmb2.sh`
4. Clean and rebuild:
   ```bash
   ./clean.sh
   ./setup.sh
   ./build_upload.sh build pico32
   ```

#### Error: "file format not recognized" when linking

**Cause:** Library compiled with wrong toolchain (x86 instead of xtensa).

**Solution:** This should not happen with the current setup. If it does:
1. Clean everything: `./clean.sh`
2. Fresh setup: `./setup.sh`
3. Rebuild: `./build_upload.sh build pico32`

### 2. Compilation Errors

#### Error: Member initialization order warnings/errors

**Example:**
```
error: 'FileUploader::wifiManager' will be initialized after
error:   'UploadStateManager* FileUploader::stateManager'
```

**Cause:** Constructor initialization list order doesn't match member declaration order in header.

**Solution:** Reorder the initialization list in the `.cpp` file to match the order in the `.h` file.

**Correct order in header:**
```cpp
class FileUploader {
private:
    Config* config;
    UploadStateManager* stateManager;
    WiFiManager* wifiManager;  // Declared after stateManager
};
```

**Correct initialization in constructor:**
```cpp
FileUploader::FileUploader(Config* cfg, WiFiManager* wifi) 
    : config(cfg),
      stateManager(nullptr),   // Initialize in declaration order
      wifiManager(wifi)        // Not before stateManager
{}
```

#### Error: "ESP_PLATFORM" related compilation issues

**Cause:** Trying to use ESP-IDF specific headers in Arduino framework.

**Solution:** The `esp_compat_wrapper.h` should handle this. If you see these errors:
1. Check that the wrapper is being included
2. Verify it doesn't include ESP-IDF specific headers like `esp_system.h`
3. Use standard POSIX headers instead

### 3. Memory/Flash Issues

#### Error: "region `iram0_0_seg' overflowed"

**Cause:** Too much code in IRAM (instruction RAM).

**Solution:**
1. Reduce debug level in `platformio.ini`:
   ```ini
   build_flags = 
       -DCORE_DEBUG_LEVEL=1  ; Change from 3 to 1
   ```
2. Disable unused features (comment out feature flags)
3. Use `IRAM_ATTR` sparingly

#### Error: "section `.dram0.bss' will not fit in region `dram0_0_seg'"

**Cause:** Too many global/static variables.

**Solution:**
1. Move large buffers to heap allocation
2. Use `PROGMEM` for constant data
3. Reduce buffer sizes if possible

#### Warning: "Flash memory size mismatch"

**Cause:** Partition table doesn't match actual flash size.

**Solution:**
1. Check your board's actual flash size (SD WIFI PRO has 4MB)
2. Use appropriate partition scheme in `platformio.ini`:
   ```ini
   # For standard build (3MB app space)
   board_build.partitions = huge_app.csv
   
   # For OTA build (1.5MB app space per partition)
   board_build.partitions = partitions_ota.csv
   ```

### 4. Upload Issues

#### Error: "Failed to connect to ESP32"

**Solution:**
1. Hold BOOT button while uploading
2. Check USB cable (use data cable, not charge-only)
3. Verify correct port:
   ```bash
   pio device list
   ```
4. Try different upload speed:
   ```ini
   upload_speed = 115200  ; Slower but more reliable
   ```
5. Use the build script with specific port:
   ```bash
   ./build_upload.sh upload pico32 /dev/ttyUSB0
   ```

#### Error: "Permission denied" on upload

**Cause:** Serial port requires sudo access.

**Solution:**
```bash
# Use the build script (handles sudo automatically)
./build_upload.sh upload pico32

# Or manual upload with sudo
sudo pio run -e pico32 -t upload
```

#### Error: "A fatal error occurred: MD5 of file does not match"

**Solution:**
1. Clean build and try again
2. Reduce upload speed
3. Check power supply (USB port may not provide enough current)

### 5. Runtime Issues

#### Device boots but doesn't connect to WiFi

**Check:**
1. Serial monitor output: `pio device monitor`
2. Config file exists on SD card: `config.json`
3. WiFi credentials are correct
4. WiFi network is 2.4GHz (ESP32 doesn't support 5GHz)

#### SD card not detected

**Check:**
1. SD card is formatted as FAT32
2. SD card is properly inserted
3. Pin connections (see `include/pins_config.h`)
4. Try different SD card (some cards are incompatible)

### 6. Firmware Type Issues

#### Error: "Firmware too large for partition"

**Cause:** Trying to build firmware that exceeds partition size limits.

**Solutions:**
1. **For OTA build (`pico32-ota`)** - 1.5MB limit:
   ```bash
   # Disable verbose logging to save space
   # In platformio.ini, comment out:
   # -DENABLE_VERBOSE_LOGGING
   
   # Reduce log buffer size
   -DLOG_BUFFER_SIZE=16384  ; Reduce from 32KB to 16KB
   ```

2. **Switch to standard build** if OTA not needed:
   ```bash
   ./build_upload.sh build pico32  # 3MB limit instead of 1.5MB
   ```

#### Choosing Between Firmware Types

**Use Standard Build (`pico32`) when:**
- Development and testing
- Maximum feature space needed
- OTA updates not required
- Firmware size approaching 1.5MB

**Use OTA Build (`pico32-ota`) when:**
- Production deployment
- Remote update capability needed
- Firmware size under 1.5MB
- Device physically inaccessible

**Current sizes:**
- Standard build: ~1.07MB (fits in both)
- OTA build: ~1.22MB (fits in both, but closer to OTA limit)

## Build Environment

### Verify Setup

```bash
# Check if setup completed successfully
./setup.sh

# Verify PlatformIO installation
source venv/bin/activate
pio --version
# Should output: PlatformIO Core, version X.X.X
```

### Check Platform and Framework Versions

```bash
source venv/bin/activate
pio platform show espressif32
```

Expected versions:
- Platform: espressif32 @ 6.x.x
- Framework: arduino-esp32 @ 3.x.x

### Verify Toolchain

```bash
source venv/bin/activate
pio run -e pico32 -v 2>&1 | grep "xtensa-esp32-elf-gcc"
```

Should show the xtensa toolchain being used.

## Getting Help

If you're still stuck:

1. **Check the documentation:**
   - [LIBSMB2_INTEGRATION.md](LIBSMB2_INTEGRATION.md) - libsmb2 setup
   - [FEATURE_FLAGS.md](FEATURE_FLAGS.md) - Feature configuration

2. **Enable verbose build output:**
   ```bash
   source venv/bin/activate
   pio run -e pico32 -v
   ```

3. **Try different firmware type:**
   ```bash
   # If pico32-ota fails, try standard build
   ./build_upload.sh build pico32
   
   # If pico32 fails, try OTA build
   ./build_upload.sh build pico32-ota
   ```

4. **Provide build information when asking for help:**
   ```bash
   source venv/bin/activate
   pio run -e pico32 -v > build_log.txt 2>&1
   ```

5. **Check for similar issues:**
   - PlatformIO: https://community.platformio.org/
   - ESP32 Arduino: https://github.com/espressif/arduino-esp32/issues
   - libsmb2: https://github.com/sahlberg/libsmb2/issues

## Useful Commands

```bash
# Clean and setup
./clean.sh                     # Clean all build artifacts and components
./setup.sh                     # Fresh environment setup (includes libsmb2)

# Build commands
./build_upload.sh build pico32     # Build standard firmware
./build_upload.sh build pico32-ota # Build OTA firmware
./build_upload.sh upload pico32    # Upload standard firmware
./build_upload.sh upload pico32-ota # Upload OTA firmware

# PlatformIO commands (after source venv/bin/activate)
pio run -e pico32 --target clean   # Clean standard build
pio run -e pico32-ota --target clean # Clean OTA build
pio run -e pico32               # Build standard firmware
pio run -e pico32-ota           # Build OTA firmware

# Build with verbose output
pio run -e pico32 -v
pio run -e pico32-ota -v

# Upload and monitor
sudo pio run -e pico32 -t upload && pio device monitor
sudo pio run -e pico32-ota -t upload && pio device monitor

# Check library dependencies
pio lib list

# Update all packages
pio pkg update

# Show build size
pio run -e pico32 --target size
pio run -e pico32-ota --target size

# Erase flash completely
pio run -e pico32 --target erase
```

## Prevention Tips

1. **Always commit working builds** before making major changes
2. **Test incrementally** - don't change too many things at once
3. **Use version control** - git is your friend
4. **Document custom changes** - future you will thank you
5. **Keep dependencies updated** but test after updates
6. **Use the provided scripts** - `./setup.sh`, `./clean.sh`, and `./build_upload.sh` handle most common tasks
7. **Choose the right firmware type** - Use `pico32-ota` for production, `pico32` for development
