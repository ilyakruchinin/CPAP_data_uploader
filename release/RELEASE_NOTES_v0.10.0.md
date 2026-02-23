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

## Upload Engine

### ⚡ Always-Async Upload Task
**Symptom:** Rare stack canary crash during TLS handshakes in mixed SMB+CLOUD sessions under low heap.
**Cause:** When `max_alloc < 50 KB`, the old code fell back to `runUploadBlocking()`, running the full TLS stack directly on the main Arduino loop task (8 KB stack). OpenSSL record processing and certificate parsing exceeded the available stack, triggering a hardware stack canary fault.
**Fix:** `runUploadBlocking()` and the `UPLOAD_ASYNC_MIN_MAX_ALLOC_BYTES` threshold are completely removed. All uploads now run in a dedicated **16 KB FreeRTOS task pinned to Core 0**, unconditionally. The main loop task stack was also increased from 8 KB to 12 KB as an additional safety net. If task creation fails (extreme heap pressure), the FSM transitions gracefully to RELEASING instead of crashing.

---

## Log Persistence

### 💾 Continuous Log Flushing
**Previous behaviour:** Logs were only flushed to LittleFS when the upload task was **not** running, and only every 10 seconds. Upload sessions — often the most diagnostically interesting period — were never persisted.
**Fix:** The `!uploadTaskRunning` guard has been removed. Logs now flush every **5 seconds continuously**, including during active upload sessions.

### 📥 Pre-Reboot Log Flush
**Feature:** Logs are now explicitly flushed immediately before every `esp_restart()` call: upload-complete reboot, software watchdog kill, state reset, and soft reboot. This guarantees zero log loss on any planned or watchdog-triggered reboot when `SAVE_LOGS=true`.

### ⬇ Download Saved Logs Button
**Feature:** A new **⬇ Download Saved Logs** button appears in the Logs tab toolbar. Clicking it:
1. Triggers a final flush of any unflushed in-memory logs
2. Downloads `syslog.B.txt` (older rotation) + `syslog.A.txt` (current) as a single `cpap_logs.txt` file directly to your browser

This is the primary way to retrieve logs from a previous session after a reboot or crash.

### 🐛 Crash Log Path Fix
**Bug:** `dumpSavedLogs()` was writing to `/littlefs/crash_log.txt` — a literal path that LittleFS cannot create (it has no subdirectories named `littlefs`). The file was silently never written.
**Fix:** Path corrected to `/crash_log.txt`.

---

## UI / UX Enhancements (Additional)

### 📈 Memory Tab — Rolling Minimum Heap Tracking
**Feature:** The Memory tab now tracks **2-minute rolling minimums** for both `Free Heap` and `Max Contiguous Alloc`. The minimum values are displayed in amber-coloured stat boxes at equal visual prominence to the live values. This surfaces worst-case heap conditions at a glance without requiring `DEBUG=true`.

### ✏️ Config Editor — Edit During Active Uploads
**Previous behaviour:** Clicking Edit while an upload was running returned a 409 error or aborted the upload session.
**Fix:** The upload-in-progress restriction has been removed from both the config lock endpoint and the config save endpoint. Uploads now run completely uninterrupted while the user edits and saves the config. This is safe because:
- The upload task reads CPAP data files; it does not read or write `config.txt`
- Config changes take effect after reboot, not mid-session
- The SD_MMC driver serialises hardware transactions internally

After **Save & Reboot**, the browser displays a countdown and auto-redirects to the Dashboard after 10 seconds.

### 🧘 Profiler Wizard — Breathing Instruction
**Feature:** The Profiler Wizard instructions now include a clearly highlighted step requiring the user to **breathe in and out continuously as in normal therapy** during the measurement period. This is critical for accurate results — the profiler measures SD write gaps during live therapy, not idle gaps.

---

## Upgrade Notes

- **LittleFS path correction (developer note):** If you wrote custom code targeting `/littlefs/crash_log.txt`, update to `/crash_log.txt`. LittleFS on ESP32 Arduino exposes the filesystem root directly — the `/littlefs/` prefix in file paths is not a valid subdirectory and writes to it silently fail.
- **Blocking upload path removed:** The `runUploadBlocking()` function and the `UPLOAD_ASYNC_MIN_MAX_ALLOC_BYTES = 50000` constant no longer exist. If you have any downstream code or references to these, remove them.
- **`LOG_TO_SD_CARD` config key:** Still accepted as a deprecated alias for `SAVE_LOGS`. A future release may remove support. Migrate to `SAVE_LOGS = true`.
- **Logs now flush during uploads:** If you rely on `SAVE_LOGS` and were previously using `UPLOAD_MODE = scheduled` specifically to ensure log flushing happened, this restriction no longer applies. Logs flush every 5 seconds regardless of upload state.
- **First Boot:** Upon installing v0.10.0 for the first time, the ESP32 will format its internal `LittleFS` partition. The first upload session will scan your entire SD card from scratch — the old `.upload_state.v2` tracking files on the SD card are no longer used.
