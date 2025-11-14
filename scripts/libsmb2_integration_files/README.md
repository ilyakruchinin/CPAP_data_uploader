# libsmb2 Integration Files

This directory contains the integration files needed to build libsmb2 with PlatformIO for ESP32.

## Files

- **library.json** - PlatformIO library manifest
- **library_build.py** - Build configuration script
- **lib/esp_compat_wrapper.h** - ESP32 compatibility wrapper

## Usage

These files are automatically copied to `components/libsmb2/` by the setup script:

```bash
./scripts/setup_libsmb2.sh
```

## Documentation

See `docs/LIBSMB2_INTEGRATION.md` for detailed information about these files and how they work.
