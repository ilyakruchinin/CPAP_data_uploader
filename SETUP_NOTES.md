# Setup Notes

## What Was Done

### 1. Project Cleanup
- Removed copied library files from `src/` folder
- Removed local `lib/` folder
- Renamed `include/config.h` to `include/pins_config.h` to avoid naming conflicts

### 2. Library Management
Updated `platformio.ini` to use PlatformIO's package manager with GitHub URLs:
```ini
lib_deps = 
    bblanchon/ArduinoJson@^6.21.3
    https://github.com/dvarrel/AsyncTCP.git
    https://github.com/ESP32Async/ESPAsyncWebServer.git
    https://github.com/FYSETC/SdWiFiBrowser.git
```

### 3. Patch Script
Created `apply_patches.sh` to automatically fix library compatibility issues:
- AsyncTCP const-correctness for status() method
- FSWebServer const AsyncWebParameter pointers

The script is idempotent (safe to run multiple times).

### 4. Backup
Created backup: `backup_20251112_204740.tar.gz` (27MB)
Contains the working state before cleanup.

## Build Results
- **RAM Usage:** 14.2% (46,684 bytes / 327,680 bytes)
- **Flash Usage:** 69.4% (909,933 bytes / 1,310,720 bytes)
- **Build Status:** ✓ SUCCESS

## Workflow

### First Time Setup
```bash
source venv/bin/activate
pio pkg install
./apply_patches.sh
pio run
```

### After Cleaning Project
```bash
source venv/bin/activate
pio pkg install      # Re-downloads libraries
./apply_patches.sh   # Re-applies patches
pio run
```

### Upload to Device
```bash
source venv/bin/activate
pio run -t upload
```

### Monitor Serial Output
```bash
source venv/bin/activate
pio device monitor
```

## Important Notes

1. **Always run `apply_patches.sh` after `pio pkg install`**
   - Libraries are downloaded fresh each time
   - Patches need to be reapplied

2. **Virtual Environment**
   - PlatformIO is installed in `venv/`
   - Always activate before running pio commands

3. **Library Sources**
   - Libraries are managed by PlatformIO
   - Downloaded to `.pio/libdeps/pico32/`
   - Not tracked in git

4. **Pin Configuration**
   - Custom pin definitions in `include/pins_config.h`
   - SdWiFiBrowser uses its own `pins.h`

## Troubleshooting

### Build fails with "duplicate const" error
- Run: `rm -rf .pio/libdeps/pico32/SdWiFiBrowser`
- Then: `pio pkg install && ./apply_patches.sh`

### Libraries not found
- Run: `pio pkg install`
- Then: `./apply_patches.sh`

### Clean rebuild
```bash
pio run --target clean
pio pkg install
./apply_patches.sh
pio run
```
