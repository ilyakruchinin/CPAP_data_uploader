# Reassessment: The 18KB Heap Drop and ma=36852

## Root Cause Chain (Fully Resolved)

The 18KB heap drop during SD mount was caused by a **three-layer configuration failure**:

### Layer 1: PlatformIO INI Parser Strips `#` Comments
PlatformIO's INI parser treats `#` as a comment character. Our inline `custom_sdkconfig` entries like `# CONFIG_WL_SECTOR_SIZE_4096 is not set` were **silently eaten** before the pioarduino builder ever saw them. Only the `CONFIG_WL_SECTOR_SIZE=512` value survived.

### Layer 2: Kconfig Boolean Conflict
The ESP-IDF Kconfig system uses **boolean selectors** (`CONFIG_WL_SECTOR_SIZE_4096=y` vs `CONFIG_WL_SECTOR_SIZE_512=y`) to determine the final value. The base Arduino sdkconfig template has `CONFIG_WL_SECTOR_SIZE_4096=y` hardcoded. Without unsetting it, the generated `sdkconfig.defaults` contained conflicting entries — both `_4096=y` AND `_512=y` — causing the ESP-IDF build to silently fall back to 4096.

### Layer 3: Arduino SD Library Compatibility
Arduino's `SD_MMC.cpp` and `SD.cpp` use the deprecated FatFs R0.13 macro `_MAX_SS` instead of the current R0.15 `FF_MAX_SS`. When `_MAX_SS` is undefined (evaluates to 0), the preprocessor evaluates `#if 0 != 512` → true, taking the `fsinfo->ssize` code path — but `ssize` only exists when `FF_MAX_SS != FF_MIN_SS`. With our fix making both 512, this field is removed, causing a compile error.

## Fix Applied

| File | Change |
|------|--------|
| `sdkconfig.project` (NEW) | External config file with all 5 WL/FATFS overrides including `# CONFIG_X is not set` entries |
| `platformio.ini` | `file://sdkconfig.project` in `custom_sdkconfig` block; `-D_MAX_SS=FF_MAX_SS` build flag |

## Verification

- `sdkconfig.defaults` now correctly shows `CONFIG_WL_SECTOR_SIZE_512=y` and `# CONFIG_WL_SECTOR_SIZE_4096 is not set`
- Build succeeded (2:49, exit 0)
- `FATFS.win[]` and `FIL.buf[]` are now 512 bytes each (down from 4096), saving ~3.5KB per FATFS struct + ~3.5KB per open FIL

## Previous Findings (Still Valid)

1. `pendingFolders` in `UploadStateManager` is a static array — no dynamic leak
2. `ma=36852` is the temporary heap low-watermark during concurrent SD mount + TLS/SMB connections
3. The 3-5.txt.tmp log shows `ma` not recovering after unmount (real fragmentation from TLS/SMB allocation interleaving)

## Expected Impact

The SD mount heap drop should decrease from ~18KB to significantly less, giving more headroom for TLS and SMB connections during upload sessions.
