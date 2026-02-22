# CPAP Data Uploader v0.10.0 Release Notes

## Overview

A major architectural update focused on completely eliminating the "SD Card Error" on the CPAP machine and resolving heap fragmentation/OOM crashes during extended uploads. 

This release fundamentally changes how the ESP32 interacts with the SD card, migrating all write operations to internal flash and ensuring absolute protocol safety when handing the card back to the CPAP.

---

## Critical Fixes

### 🛡️ 100% Read-Only SD Card Mounting
**Symptom:** The CPAP machine occasionally displayed an unrecoverable "SD Card Error," forcing users to reformat the card.
**Cause:** The ESP32 was writing state tracking files (`.upload_state.*`) and logs to the FAT32 volume. If the CPAP machine had the FAT table cached in its RAM, it detected this modification as silent corruption upon waking up.
**Fix:** The ESP32 now mounts the physical SD card strictly as **Read-Only**. It is physically impossible for the ESP32 to corrupt the CPAP's FAT32 filesystem.

### 💾 LittleFS Internal State Migration
**Feature:** All upload tracking state and session summaries have been migrated off the physical SD card and onto the ESP32's internal 960KB `LittleFS` partition. The physical SD card is only used for reading therapy data.

### 🔄 SD Protocol Reset (CMD0 Bit-Banging)
**Symptom:** Intermittent CPAP timeouts when the ESP32 released the card.
**Cause:** `SD_MMC.end()` left the physical NAND flash chip in a `Transfer` state listening for the ESP32's Relative Card Address (RCA). When the CPAP took over, it sent commands using its own RCA, which the card ignored.
**Fix:** The firmware now manually bit-bangs the `CMD0` (GO_IDLE_STATE) frame before releasing the multiplexer. This forcefully crashes the SD card's state machine back to `Idle`, prompting the CPAP's error-recovery routines to cleanly re-initialize and remount the card. This behavior can be disabled via `ENABLE_SD_CMD0_RESET=false` in `config.txt`.

### ⏱️ Hostile Takeover Removed
**Symptom:** Corrupted therapy data if an upload started while the CPAP was writing.
**Cause:** A 45-second timeout (`SMART_WAIT_MAX_MS`) would eventually force the ESP32 to take control of the SD card even if the CPAP was actively writing to it.
**Fix:** The timeout has been completely removed. The ESP32 will now wait indefinitely for the required `INACTIVITY_SECONDS` of absolute bus silence before taking control.

---

## Memory & Stability Optimizations

### 📉 Heap Fragmentation Mitigations
**Feature:** Addressed the root causes of the `LoadProhibited` crashes and `libsmb2` hangs caused by memory exhaustion during large `SMB+CLOUD` syncs:
- **Reduced Buffers:** `LOG_BUFFER_SIZE` reduced from 12KB to 4KB.
- **Shrunk Task Stacks:** `ARDUINO_LOOP_STACK_SIZE` reduced from 16KB to 8KB.
- **JSON Allocator Discipline:** Replaced highly fragmenting `DynamicJsonDocument` allocations in the SleepHQ Cloud upload path with stack-allocated `StaticJsonDocument`.
- **String Elimination:** Refactored URL building and SleepHQ authentication to use static `char` buffers instead of `String` concatenation.

### ✂️ Link-Time Optimizations
**Feature:** The compiler now uses `-Os`, `-ffunction-sections`, `-fdata-sections`, and `-Wl,--gc-sections` to strip all unused code and variables from the final binary, significantly reducing flash footprint and freeing up statically allocated RAM.

---

## UI / UX Enhancements

### ⚙️ Config Editor Remount Strategy
**Feature:** You can still edit `config.txt` via the Web UI! When you click Save, the ESP32 briefly remounts the SD card as Read/Write, saves the file, and immediately remounts it as Read-Only. 
**⚠️ CRITICAL WARNING:** The UI will now lock and display a prominent warning requiring you to **physically eject and reinsert** the SD card into the CPAP machine after saving to prevent FAT table corruption.

### 🩺 CPAP Profiler Wizard
**Feature:** A new interactive wizard in the Web UI (Monitor page) allows you to empirically measure your specific CPAP machine's active writing behavior. It records the longest continuous silence between SD writes and recommends a safe, personalized `INACTIVITY_SECONDS` setting.

### 📊 Runtime Heap Diagnostics UI
**Feature:** The Dashboard now displays live `Free Heap` and `Max Alloc` statistics directly from the ESP32, updating every 3 seconds without relying on heavy internal circular buffers.

### 🐛 Monitor Page Bug Fixed
**Feature:** Opening the `/monitor` page no longer automatically engages Monitoring Mode (which pauses uploads). You must explicitly click the "Start Monitoring" button. The legacy, memory-heavy `CPAPMonitor` backend class has been completely removed.

---

## Upgrade Notes

- **Logs Location:** If `LOG_TO_SD_CARD=true` is set in your `config.txt`, logs are now written to the internal `LittleFS` partition (as `syslog.A.txt` / `syslog.B.txt` ping-pong files) to prevent SD card corruption. They can be viewed normally via the Web UI Logs tab.
- **First Boot:** Upon installing v0.10.0, your ESP32 will format its internal `LittleFS` partition. The very first upload session will scan your entire SD card from scratch, as the old `.upload_state.v2` tracking files on the SD card are no longer used.
