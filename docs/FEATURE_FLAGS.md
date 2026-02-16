# Feature Flags for Upload Backends

This document describes the compile-time feature flag system used to select upload backends.

## Overview

The SD WIFI PRO auto uploader supports multiple upload backends (SMB, WebDAV, SleepHQ). To minimize binary size and memory usage, these backends are conditionally compiled using preprocessor feature flags.

## Benefits

1. **Reduced Binary Size**: Only include code for backends you actually use
2. **Lower Memory Usage**: Fewer classes instantiated at runtime
3. **Faster Compilation**: Less code to compile
4. **Cleaner Code**: Clear separation between backend implementations

## Available Feature Flags

### ENABLE_SMB_UPLOAD

Enables SMB/CIFS upload support for Windows shares, NAS devices, and Samba servers.

**Binary Size Impact**: +220-270KB (includes libsmb2 library)

**Requirements**:
- libsmb2 library must be cloned into `components/libsmb2`
- See [LIBSMB2_SETUP.md](LIBSMB2_SETUP.md) for setup instructions

**Usage in config.txt**:
```ini
ENDPOINT_TYPE = SMB
ENDPOINT = //192.168.1.100/cpap_backups
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password
```

### ENABLE_WEBDAV_UPLOAD

Enables WebDAV upload support for Nextcloud, ownCloud, and standard WebDAV servers.

**Status**: TODO - Placeholder implementation only

**Binary Size Impact**: +50-80KB (estimated, uses HTTPClient)

**Usage in config.txt**:
```ini
ENDPOINT_TYPE = WEBDAV
ENDPOINT = https://cloud.example.com/remote.php/dav/files/user/cpap
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password
```

### ENABLE_SLEEPHQ_UPLOAD

Enables direct upload to SleepHQ cloud service via REST API with OAuth authentication.

**Status**: Implemented

**Binary Size Impact**: +110KB (includes ISRG Root X1 CA cert for TLS)

**Usage in config.txt**:
```ini
ENDPOINT_TYPE = CLOUD
CLOUD_CLIENT_ID = your-sleephq-client-id
CLOUD_CLIENT_SECRET = your-sleephq-client-secret
```

Can also be combined with SMB: `ENDPOINT_TYPE = SMB,CLOUD`

## How to Enable/Disable Backends

### Method 1: Edit platformio.ini (Recommended)

Open `platformio.ini` and uncomment the desired feature flag(s):

```ini
build_flags = 
    -DCORE_DEBUG_LEVEL=3
    -Icomponents/libsmb2/include
    -DENABLE_SMB_UPLOAD          ; Enable SMB/CIFS upload support
    ; -DENABLE_WEBDAV_UPLOAD     ; Enable WebDAV upload support (TODO)
    ; -DENABLE_SLEEPHQ_UPLOAD    ; Enable SleepHQ cloud upload (HTTPS + OAuth)
```

### Method 2: Command Line Override

You can override flags at build time:

```bash
# Build with only WebDAV support
pio run -e pico32 --build-flag="-DENABLE_WEBDAV_UPLOAD"

# Build with multiple backends
pio run -e pico32 --build-flag="-DENABLE_SMB_UPLOAD" --build-flag="-DENABLE_WEBDAV_UPLOAD"
```

## Runtime Backend Selection

When multiple backends are enabled at compile time, the active backend is selected at runtime based on the `ENDPOINT_TYPE` setting in `config.txt`.

**Example**: If both SMB and WebDAV are enabled:
- Set `ENDPOINT_TYPE = SMB` to use SMB upload
- Set `ENDPOINT_TYPE = WEBDAV` to use WebDAV upload
- Change `config.txt` and reboot to switch backends

## Implementation Details

### Conditional Compilation

The feature flags use C preprocessor directives to conditionally include code:

```cpp
#ifdef ENABLE_SMB_UPLOAD
#include "SMBUploader.h"
#endif

#ifdef ENABLE_WEBDAV_UPLOAD
#include "WebDAVUploader.h"
#endif

#ifdef ENABLE_SLEEPHQ_UPLOAD
#include "SleepHQUploader.h"
#endif
```

### FileUploader Integration

The `FileUploader` class automatically selects the appropriate uploader based on:
1. Which backends are enabled at compile time (feature flags)
2. The `ENDPOINT_TYPE` setting in `config.txt`

If the requested backend is not enabled at compile time, an error message is displayed with instructions on which flag to enable.

### Error Handling

If you configure an endpoint type that wasn't compiled in:

```
[FileUploader] ERROR: Unsupported or disabled endpoint type: WEBDAV
[FileUploader] Supported types (based on build flags):
[FileUploader]   - SMB (enabled)
[FileUploader]   - WEBDAV (disabled - compile with -DENABLE_WEBDAV_UPLOAD)
[FileUploader]   - SLEEPHQ (disabled - compile with -DENABLE_SLEEPHQ_UPLOAD)
```

## Binary Size Comparison

| Configuration | Approximate Binary Size |
|--------------|------------------------|
| No backends | Base size |
| SMB only | Base + 220-270KB |
| WebDAV only | Base + 50-80KB (est.) |
| SleepHQ only | Base + 110KB |
| All backends | Base + 330-380KB |

**Recommendation**: Enable only the backend(s) you need to maximize available flash space for future features.

## Adding New Backends

To add a new upload backend:

1. Create header file: `include/NewBackendUploader.h`
2. Create implementation: `src/NewBackendUploader.cpp`
3. Wrap with feature flag: `#ifdef ENABLE_NEWBACKEND_UPLOAD`
4. Add flag to `platformio.ini`
5. Update `FileUploader.h` to include the new header
6. Update `FileUploader.cpp` to instantiate and use the new uploader
7. Document in this file

## Requirements Mapping

This feature flag implementation satisfies the following requirements from the spec:

- **Requirement 10.1**: Read ENDPOINT_TYPE configuration value
- **Requirement 10.6**: Support WebDAV protocol (placeholder)
- **Requirement 10.7**: Support SleepHQ direct upload (placeholder)

## See Also

- [LIBSMB2_SETUP.md](LIBSMB2_SETUP.md) - SMB backend setup instructions
- [README.md](../README.md) - Main project documentation
- [requirements.md](../.kiro/specs/file-tracking-and-upload-scheduling/requirements.md) - Full requirements specification
