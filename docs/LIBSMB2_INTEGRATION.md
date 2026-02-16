# libsmb2 Integration Guide

This document describes how libsmb2 was integrated into this ESP32 Arduino project for SMB/CIFS network file uploads.

## Overview

libsmb2 is a client library for accessing SMB2/SMB3 network shares. This integration allows the ESP32 to upload files directly to Windows shares, NAS devices, or Samba servers.

## Prerequisites

- PlatformIO
- ESP32 development board
- libsmb2 library cloned in `components/libsmb2/`

## Installation Steps

### 1. Clone libsmb2 Library

```bash
git clone https://github.com/sahlberg/libsmb2.git components/libsmb2
```

### 2. Project Structure

The integration requires the following files:

```
project_root/
├── components/
│   └── libsmb2/
│       ├── include/          # libsmb2 headers
│       ├── lib/              # libsmb2 source files
│       ├── library.json      # PlatformIO library manifest
│       ├── library_build.py  # Build configuration script
│       └── lib/
│           └── esp_compat_wrapper.h  # ESP32 compatibility layer
├── platformio.ini            # Updated with lib_extra_dirs
└── src/
    └── SMBUploader.cpp       # Your SMB uploader implementation
```

## Integration Files

### 1. ESP32 Compatibility Wrapper

**File:** `components/libsmb2/lib/esp_compat_wrapper.h`

This header provides ESP32-specific compatibility for libsmb2:

```c
/* ESP32 Arduino compatibility wrapper for libsmb2 */
#ifndef ESP_COMPAT_WRAPPER_H
#define ESP_COMPAT_WRAPPER_H

/* Include standard headers needed for libsmb2 on ESP32 */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/uio.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* Define random functions for ESP32 */
#define smb2_random esp_random
#define smb2_srandom(seed) /* ESP32 RNG doesn't need seeding */

/* Define login_num for getlogin_r */
#define login_num ENXIO

/* Declare esp_random if not already declared */
#ifdef __cplusplus
extern "C" {
#endif

uint32_t esp_random(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_COMPAT_WRAPPER_H */
```

**Purpose:**
- Provides necessary system headers for ESP32
- Maps libsmb2's random functions to ESP32's `esp_random()`
- Defines compatibility macros for POSIX functions

### 2. Library Manifest

**File:** `components/libsmb2/library.json`

```json
{
  "name": "libsmb2",
  "version": "6.1.0",
  "description": "SMB2/3 client library for ESP32",
  "keywords": "smb, smb2, smb3, cifs, network",
  "repository": {
    "type": "git",
    "url": "https://github.com/sahlberg/libsmb2.git"
  },
  "authors": [
    {
      "name": "Ronnie Sahlberg",
      "email": "ronniesahlberg@gmail.com"
    }
  ],
  "license": "LGPL-2.1",
  "frameworks": "arduino",
  "platforms": "espressif32",
  "build": {
    "srcFilter": [
      "+<lib/*.c>"
    ],
    "includeDir": "include",
    "libArchive": false,
    "extraScript": "library_build.py"
  }
}
```

**Key Settings:**
- `srcFilter`: Compiles all `.c` files in the `lib/` directory
- `includeDir`: Makes `include/` available to other code
- `libArchive: false`: Builds as source (not pre-compiled archive)
- `extraScript`: Runs custom build configuration

### 3. Build Configuration Script

**File:** `components/libsmb2/library_build.py`

```python
Import("env")
import os

# Get the library source directory from the environment
lib_source_dir = env.get("PROJECT_SRC_DIR", "").replace("/src", "/components/libsmb2")

# Add include paths relative to library root
env.Append(CPPPATH=[
    os.path.join(lib_source_dir, "include"),
    os.path.join(lib_source_dir, "include/smb2"),
    os.path.join(lib_source_dir, "include/esp"),
    os.path.join(lib_source_dir, "lib"),
])

# Add compile definitions
env.Append(CPPDEFINES=[
    "HAVE_CONFIG_H",
    "NEED_READV",
    "NEED_WRITEV",
    "NEED_GETLOGIN_R",
    "NEED_RANDOM",
    "NEED_SRANDOM",
    ("_U_", ""),
])

# Add C flags
env.Append(CCFLAGS=[
    "-Wno-implicit-function-declaration",
    "-Wno-builtin-declaration-mismatch",
    "-include", os.path.join(lib_source_dir, "lib/esp_compat_wrapper.h"),
])
```

**Purpose:**
- Adds all necessary include paths for libsmb2 headers
- Defines required preprocessor macros for ESP32 platform
- Forces inclusion of the compatibility wrapper
- Suppresses warnings for compatibility functions

### 4. PlatformIO Configuration

**File:** `platformio.ini`

Add the following to your environment configuration:

```ini
[env:pico32]
platform = espressif32
board = pico32
framework = arduino

build_flags = 
    -DCORE_DEBUG_LEVEL=3
    -Icomponents/libsmb2/include
    -DENABLE_SMB_UPLOAD

lib_deps = 
    bblanchon/ArduinoJson@^6.21.3

; Extra library directories
lib_extra_dirs = components
```

**Key Settings:**
- `lib_extra_dirs = components`: Tells PlatformIO to look for libraries in the `components/` directory
- `-Icomponents/libsmb2/include`: Makes libsmb2 headers available globally
- `-DENABLE_SMB_UPLOAD`: Feature flag to enable SMB upload code

## Build Process

### How It Works

1. **Library Discovery**: PlatformIO scans `components/` for libraries with `library.json`
2. **Build Script Execution**: `library_build.py` configures the build environment
3. **Compatibility Layer**: `esp_compat_wrapper.h` is force-included in all libsmb2 source files
4. **Compilation**: All `.c` files in `components/libsmb2/lib/` are compiled
5. **Linking**: The compiled library is linked with your main application

### Compile Flags Explained

- `HAVE_CONFIG_H`: Tells libsmb2 to use configuration headers
- `NEED_READV/NEED_WRITEV`: Enables compatibility implementations for vector I/O
- `NEED_GETLOGIN_R`: Enables stub for `getlogin_r()` function
- `NEED_RANDOM/NEED_SRANDOM`: Enables ESP32 random number generator mapping
- `_U_=`: Defines the unused variable attribute as empty (GCC handles this automatically)

## Usage in Code

### Include Headers

```cpp
#include <smb2/libsmb2.h>
```

### Example SMB Connection

```cpp
struct smb2_context *smb2 = smb2_init_context();
if (smb2 == NULL) {
    Serial.println("Failed to init SMB2 context");
    return false;
}

smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
smb2_set_user(smb2, username.c_str());
smb2_set_password(smb2, password.c_str());

if (smb2_connect_share(smb2, server.c_str(), share.c_str(), NULL) < 0) {
    Serial.printf("Failed to connect: %s\n", smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return false;
}
```

## Troubleshooting

### Common Issues

#### 1. "smb2.h: No such file or directory"

**Solution:** Ensure `lib_extra_dirs = components` is in `platformio.ini`

#### 2. "undefined reference to smb2_*"

**Solution:** Verify that:
- `library.json` exists in `components/libsmb2/`
- `library_build.py` is being executed (check build output)
- All `.c` files are being compiled

#### 3. Compilation errors about missing types

**Solution:** Check that `esp_compat_wrapper.h` includes all necessary headers

#### 4. "file format not recognized" linking error

**Solution:** This means the library was compiled with the wrong toolchain. Ensure you're not using `env.Clone()` without preserving the toolchain settings.

## Memory Considerations

libsmb2 adds approximately 220-270KB to your firmware size. Monitor your flash usage:

```
Flash: [===       ]  30.8% (used 967365 bytes from 3145728 bytes)
```

If you're running low on flash:
- Use the `huge_app.csv` partition scheme (3MB app space)
- Disable unused features with `#ifdef` guards
- Consider using only the SMB functions you need

## Testing

### Build Test

```bash
pio run
```

Expected output:
```
Compiling .pio/build/pico32/lib82c/libsmb2/lib/libsmb2.c.o
...
Linking .pio/build/pico32/firmware.elf
========================= [SUCCESS] Took X.XX seconds =========================
```

### Runtime Test

1. Configure WiFi credentials
2. Set SMB server details in `config.txt`
3. Upload firmware
4. Monitor serial output for connection status

## References

- [libsmb2 GitHub Repository](https://github.com/sahlberg/libsmb2)
- [PlatformIO Library Documentation](https://docs.platformio.org/en/latest/librarymanager/index.html)
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)

## License

libsmb2 is licensed under LGPL-2.1. See `components/libsmb2/COPYING` for details.
