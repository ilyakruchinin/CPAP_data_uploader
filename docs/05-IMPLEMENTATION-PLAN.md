# Implementation Plan: Stability & Memory Optimizations

This document outlines the prioritized implementation plan for resolving the unrecoverable "SD Card Error" on the CPAP machine and mitigating heap fragmentation/memory exhaustion on the ESP32.

## Phase 1: SD Protocol & Filesystem Safety (Priority 1)
*Goal: Ensure the CPAP machine never encounters an invalid protocol state or fractured FAT32 filesystem.*

### 1. Read-Only SD Mounting & LittleFS State Migration
- **Mount SD Read-Only**: Change the SD mount command to `SD_MMC.begin("/sdcard", true)` to enforce hardware read-only mode for the ESP32.
- **Initialize LittleFS**: Initialize the ESP32's internal flash partition using `LittleFS.begin(true)`.
- **Migrate State Files**: Update `UploadStateManager` to write all `.upload_state.v2.*` files, `.backend_summary.*`, and `.upload_state.*.log` files to the `/littlefs` partition. Absolutely no state or counters will be written to the physical SD card.
- **Migrate Logs**: If persistent logging is enabled, implement a lightweight A/B ping-pong file rotation on `LittleFS` (e.g., `syslog.A.txt`, `syslog.B.txt`) to prevent filling the internal flash.

### 2. Config Editor Remount Strategy
- **Temporary R/W Remount**: When the user saves configuration changes via the Web UI, the ESP32 will briefly unmount the SD card, remount it as Read/Write, save `config.txt`, and immediately remount it as Read-Only.
- **Critical UI Warning**: The Web UI will block further actions and display a prominent warning stating that the SD card **MUST be physically ejected and reinserted** into the CPAP machine. This is strictly required because the CPAP caches the FAT table in RAM; modifying `config.txt` beneath it without a physical removal will cause a FAT mismatch and a fatal error.

### 3. SD State Machine Reset (CMD0 Bit-banging)
- **Configurable Protocol Reset**: Modify `SDCardManager::releaseControl()` to manually bit-bang the `CMD0` (GO_IDLE_STATE) frame (`0x40 0x00 0x00 0x00 0x00 0x95`) over the `SD_CMD_PIN` immediately before releasing the `GPIO_26` multiplexer. This will be controlled by a new `config.txt` parameter (e.g., `ENABLE_SD_CMD0_RESET=true` by default).
- **Why**: `SD_MMC.end()` disables the ESP32 peripheral but leaves the physical NAND chip in a `Transfer` state listening for the ESP32. Forcing a software reset makes the CPAP machine treat the card as freshly inserted, avoiding a timeout exception. Allowing it to be toggled ensures fallback logic is retained if needed.

### 4. Remove Hostile Takeover (`SMART_WAIT_MAX_MS`)
- **Strict Silence Requirement**: Delete the 45-second `SMART_WAIT_MAX_MS` timeout from `main.cpp`. The ESP32 will wait indefinitely for the required `SMART_WAIT_SECONDS` of bus silence.
- **Why**: Taking control of the bus while the CPAP is actively writing guarantees filesystem corruption. We must never interrupt a write operation.

### 5. Retain PCNT-Based Bus Snooping
- **Keep Existing Implementation**: The current `TrafficMonitor` implementation utilizing the ESP32's Pulse Counter (PCNT) on the `CS_SENSE` pin will be retained.
- **Why**: The PCNT peripheral counts electrical edges in pure hardware with zero CPU overhead. It safely monitors the CPAP's high-frequency SPI/SDIO activity without risking the interrupt storms and Watchdog crashes that a software ISR approach would cause.

## Phase 2: Memory Optimization & Heap Fragmentation (Priority 2)
*Goal: Reclaim contiguous DRAM to prevent SSL allocation failures and libsmb2 hangs.*

### 6. Shrink Task Stacks and Buffers
- **Log Buffer**: Reduce `LOG_BUFFER_SIZE` in `platformio.ini` from 12288 bytes to 4096 bytes (or 2048 bytes).
- **Loop Stack**: Reduce `ARDUINO_LOOP_STACK_SIZE` from 16KB back to a more reasonable 8KB.
- **Why**: Directly reclaims 12-16KB of precious static RAM.

### 7. Allocator Discipline (Static Buffers & JSON)
- **Eliminate String Concatenation**: Refactor URL building, logging, and path manipulation in `SleepHQUploader` and `SMBUploader` to use fixed-size stack `char` arrays and `snprintf`.
- **JSON Handling**: Transition `DynamicJsonDocument` allocations to `StaticJsonDocument` for state loading/saving to avoid fragmenting the heap.

### 8. Link-Time Optimizations
- **Build Flags**: Ensure `platformio.ini` utilizes `-O2` (or `-Os`), `-ffunction-sections`, `-fdata-sections`, and `-Wl,--gc-sections` to strip unused functions and variables from the final compiled binary.

## Phase 3: UI/UX & Diagnostics (Priority 3)
*Goal: Provide empirical telemetry and prevent accidental upload interruptions.*

### 9. Fix Monitor Page Bug & Remove Legacy CPAPMonitor
- **Safe Monitor Page**: Prevent the `/monitor` endpoint from automatically engaging Monitoring mode upon page load. Require the user to click an explicit "Start Monitoring" button with a clear explanation that uploads will be paused.
- **Remove Legacy Code**: Strip out the legacy `CPAPMonitor` backend code and the 24-hour graph to eliminate false-positive `[INFO] CPAP monitor disabled (CS_SENSE hardware issue)` errors from the logs.

### 10. Runtime Heap Diagnostics UI
- **Lightweight API**: Create an endpoint (`/api/diagnostics`) returning `ESP.getFreeHeap()` and `ESP.getMaxAllocHeap()`.
- **Client-Side Rendering**: The Web UI will poll this endpoint and render a memory health indicator. No historical memory tracking will be kept in the ESP32's RAM.

### 11. CPAP Profiler Wizard
- **Empirical Tuning**: Create a wizard in the Web UI that leverages the `TrafficMonitor` to graph the CPAP's actual writing bursts and cooldown periods. This will guide users in securely setting their `UPLOAD_WINDOW` and `SMART_WAIT_SECONDS` parameters.
