# Comprehensive Reassessment of SD Card Error and Contiguous Heap Issues (G31A)

After carefully analyzing the logs from `3-4-1.txt.tmp`, `3-4-2.txt.tmp`, and `3-4-after-fix.txt.tmp`, the true root causes of the two highly disruptive issues—recurring CPAP "SD Card Error"s and max contiguous heap (`ma`) depletion—have been uncovered. 

This document serves as the final, no-code-changed summary of the findings so that these fixes can be safely applied.

## 1. The "SD Card Error" Root Cause
**Timeline from the logs:**
1. The CPAP machine performs natural "silent" periods while actively delivering therapy because it buffers high-resolution (BRP) data into its internal RAM to reduce wear and tear on the SD card.
2. The `UploadStateManager` and `FSM` in the ESP32 use a PCNT counter to track bus activity. The `INACTIVITY_SECONDS` limit was set to **62 seconds**. 
3. At `21:09:17`, the ESP32 finished an operation and began listening to the bus.
4. Exactly 62 seconds later, at `21:10:19`, the ESP32 triggered an upload because `idle=62070ms >= threshold=62000ms`. The ESP32 falsely assumed the CPAP machine was off or idle.
5. Between `21:10:20` and `21:11:17`, the ESP32 mounted the SD card and took exclusive physical control over the SD bus for **56 seconds** to process a large file upload.
6. **The Collision**: During that 56-second window, the CPAP machine's internal RAM buffer filled up. It woke up and attempted to flush the data to the SD card. Because the ESP32's hardware multiplexer (mux) had physically disconnected the CPAP from the SD card, the CPAP machine timed out and permanently logged/displayed a fatal "SD Card Error".
7. Once the CPAP machine throws this error, it gives up on writing to the SD card for the entirety of the sleep session. As a result, the bus remains permanently silent, which causes the ESP32 to endlessly trigger `62s of bus silence confirmed` loops indefinitely (seen continuously from `21:13:20` to `23:05:44`).

**Required Fix:**
- The inactivity timeout (`INACTIVITY_SECONDS`) must be dramatically increased (e.g., to 300 seconds / 5 minutes or 600 seconds / 10 minutes) to ensure the ESP32 does not steal the SD card while the CPAP is buffering data during active therapy. This can be configured in `config.txt` and the factory default in `Config.cpp`.

## 2. The 18KB Heap Drop and `ma=36852` Fragmentation Trap
**Timeline from the logs:**
1. The `SD_MMC.begin()` command allocates approximately 18KB of contiguous memory when mounting the SD card, dropping `ma` immediately.
2. In a previous patch, it was advised to change `CONFIG_WL_SECTOR_SIZE=512` in the `sdkconfig.defaults` file to reduce this allocation to ~4KB. 
3. **The Miss:** Modifying `sdkconfig.defaults` in this project *did absolutely nothing*. The project uses PlatformIO with `pioarduino` to do a "hybrid compile". As indicated in the `platformio.ini` comments, the builder script takes configuration solely from the `custom_sdkconfig` property in `platformio.ini` and overwrites `sdkconfig.defaults` during the build process. Precompiled libraries `libfatfs.a` and `libsdmmc.a` were loaded using the default 4096 sector size, causing the 18KB SD mount bloat to remain.
4. Additionally, because the 18KB block takes up a huge chunk of the 59KB max contiguous block (`ma=59380`), any tiny asynchronous allocation (such as LwIP TCP sockets retaining data, or dynamic SMB buffer creation) made *during* the multi-minute background upload splits the remaining heap.
5. When the SD card is finally released, the 18KB block is freed, but it cannot merge with the rest of the free space due to the mid-upload fragmentation. This permanently traps `ma` at exactly `36852` bytes.

**Required Fix:**
- Add `CONFIG_WL_SECTOR_SIZE=512` and `CONFIG_FATFS_SECTOR_512=y` strictly to the `custom_sdkconfig` section inside `platformio.ini`. This will correctly trigger the framework builder to recompile the IDF libraries and properly truncate the SD buffer overhead down to ~4KB. 

## Next Steps
This concludes the review and reassessment. No code changes have been applied in this session. When you are ready, follow these specific items to update the repository and resolve both critical stability bugs.
