# Build Troubleshooting Guide

This guide helps resolve common build issues with this ESP32 project.

## Quick Fixes

### Clean Build
If you encounter strange build errors, try a clean build:

```bash
pio run --target clean
rm -rf .pio/build
pio run
```

### Update Dependencies
```bash
pio pkg update
pio lib update
```

## Common Build Errors

### 1. libsmb2 Related Errors

#### Error: "smb2.h: No such file or directory"

**Cause:** libsmb2 library not found or not properly configured.

**Solution:**
```bash
# Check if libsmb2 exists
ls components/libsmb2/

# If missing, clone it
git clone https://github.com/sahlberg/libsmb2.git components/libsmb2

# Verify platformio.ini has lib_extra_dirs
grep "lib_extra_dirs" platformio.ini
# Should output: lib_extra_dirs = components
```

#### Error: "undefined reference to smb2_*"

**Cause:** libsmb2 library not being compiled or linked.

**Solution:**
1. Verify `components/libsmb2/library.json` exists
2. Check that `library_build.py` is present
3. Clean and rebuild:
   ```bash
   pio run --target clean
   pio run
   ```

#### Error: "file format not recognized" when linking

**Cause:** Library compiled with wrong toolchain (x86 instead of xtensa).

**Solution:** This should not happen with the current setup. If it does:
1. Delete the build directory: `rm -rf .pio/build`
2. Ensure you're not modifying the build environment incorrectly
3. Rebuild: `pio run`

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
1. Check your board's actual flash size
2. Use appropriate partition scheme in `platformio.ini`:
   ```ini
   board_build.partitions = huge_app.csv  ; For 4MB flash
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

## Build Environment

### Verify PlatformIO Installation

```bash
pio --version
# Should output: PlatformIO Core, version X.X.X
```

### Check Platform and Framework Versions

```bash
pio platform show espressif32
```

Expected versions:
- Platform: espressif32 @ 6.x.x
- Framework: arduino-esp32 @ 2.x.x

### Verify Toolchain

```bash
pio run -v 2>&1 | grep "xtensa-esp32-elf-gcc"
```

Should show the xtensa toolchain being used.

## Getting Help

If you're still stuck:

1. **Check the documentation:**
   - [LIBSMB2_INTEGRATION.md](LIBSMB2_INTEGRATION.md) - libsmb2 setup
   - [FEATURE_FLAGS.md](FEATURE_FLAGS.md) - Feature configuration

2. **Enable verbose build output:**
   ```bash
   pio run -v
   ```

3. **Check for similar issues:**
   - PlatformIO: https://community.platformio.org/
   - ESP32 Arduino: https://github.com/espressif/arduino-esp32/issues
   - libsmb2: https://github.com/sahlberg/libsmb2/issues

4. **Provide build information when asking for help:**
   ```bash
   pio run -v > build_log.txt 2>&1
   ```

## Useful Commands

```bash
# Clean everything
pio run --target clean

# Clean and rebuild
pio run --target clean && pio run

# Build with verbose output
pio run -v

# Upload and monitor
pio run --target upload && pio device monitor

# Check library dependencies
pio lib list

# Update all packages
pio pkg update

# Show build size
pio run --target size

# Erase flash completely
pio run --target erase
```

## Prevention Tips

1. **Always commit working builds** before making major changes
2. **Test incrementally** - don't change too many things at once
3. **Use version control** - git is your friend
4. **Document custom changes** - future you will thank you
5. **Keep dependencies updated** but test after updates
