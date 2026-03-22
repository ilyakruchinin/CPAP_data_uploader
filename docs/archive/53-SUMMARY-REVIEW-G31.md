# 53 — Summary Review and Clarifications

**Date:** 2026-03-22  
**Context:** Review of proposed fixes in Document 52 based on user feedback.

---

## 1. Review of Part A: SD Card Fragmentation

### Your Insight on `max_files=1` (Option A5)
Your intuition here is **brilliant and absolutely correct**. 

Because the ESP32 is a dual-core system, the Web Server runs in the `loop()` on **Core 1**, while the upload task runs on **Core 0**. If you are mid-upload and a user clicks "Save Config" in the Web UI, the Web Server will attempt to `sd.open("/config.txt.tmp", FILE_WRITE)`. 

If we hardcode `max_files=1`, that concurrent `sd.open()` will immediately fail, throwing an error in the UI or potentially causing a crash if an edge case isn't caught. 

**The adjustment:** We should set `max_files=2`. 
Because we are also implementing the `CONFIG_WL_SECTOR_SIZE=512` change, the memory math looks like this:
*   **Original (5 files, 4096 buffer):** ~21 KB
*   **Proposed originally (1 file, 512 buffer):** ~512 bytes
*   **Revised for safety (2 files, 512 buffer):** ~1024 bytes (1 KB)

Dropping the SD mount allocation from ~27 KB down to just ~1.5 KB total (including the FATFS window) completely solves the fragmentation problem while keeping a 100% safe margin for concurrent Web UI access. 

### Option A6 (Persistent Mount) vs A5
Since we agree on discarding A7 and A8, the choice is between A5 and A6.

*   **A5 (Config tweaks):** Requires changing literally 2 lines of code (one in `SDCardManager.cpp`, one in `sdkconfig.defaults`). It reduces the fragmentation to a negligible ~1.5 KB.
*   **A6 (Persistent mount):** Eliminates the 1.5 KB entirely, but requires rewriting `SDCardManager` to bypass Arduino's `SD_MMC` wrapper and directly manage the ESP-IDF FatFs/SDMMC APIs. It requires carefully remounting the logical drive without freeing the structs to ensure the cache isn't corrupted when the CPAP writes to it.

**Recommendation:** Let's do **A5 first** (with `max_files=2`). It is extremely low-risk, trivial to implement, and should permanently fix the `ma` drop. We will keep A6 in our back pocket only if A5 somehow isn't enough (which is highly unlikely).

---

## 2. Clarification on Part B: No-Work Suppression (B.2)

You asked a very good question: *"The CPAP writes to SD every 57 seconds... I tend to believe we do need to mount the card to check... Why is it a problem worth fixing?"*

Here is the exact sequence of events happening in the logs during the user's sleep, which explains why B2 is actually a critical stability issue for the CPAP machine:

1.  **Therapy is active.** The CPAP writes data to the card.
2.  The ESP detects bus activity.
3.  The CPAP finishes its write and goes idle for ~60-65 seconds (due to slight timing variations or CPAP processing, it occasionally hits our 62s silence threshold).
4.  The ESP sees 62s of silence, **takes control of the SD card via the MUX**, and mounts it.
5.  The ESP probes for work. Because it's the middle of the night (outside the upload window) or the deltas are tiny, it decides there is "No work to do".
6.  The ESP sets a "No-work suppression" flag and releases the SD card back to the CPAP.
7.  A few seconds later, the CPAP performs its next 57-second write.
8.  The ESP detects this bus activity, and its logic says: *"Ah! The CPAP wrote something! Let's clear the no-work suppression flag so I can check it!"*
9.  **The loop repeats.**

### Why is this a problem?
Because of this loop, the ESP is stealing the SD card from the CPAP machine **every 2 minutes, all night long**. 

Every time we toggle the MUX, we cut the physical connection between the CPAP and the SD card. If the CPAP's internal 57-second timer drifts slightly and attempts to write to the SD card *exactly* during the 2-3 seconds that the ESP holds the card to do its "no-work probe", the CPAP will fail to write. If this happens, the CPAP will likely throw an **"SD Card Error"** on its screen, potentially aborting data logging for the rest of the night or waking the user.

### The Goal of B2
The goal of B2 is to ensure the ESP **stays out of the CPAP's way** while therapy is active. 

If we probe the card and find nothing useful to upload, we should say: *"The CPAP is running, but we don't need to upload right now. Let's ignore bus activity and **not** steal the card again for at least 30 minutes."* 

By adding a minimum suppression time (e.g., 30 minutes), we allow the CPAP to own the card continuously for long, safe blocks of time, drastically reducing the chance of an active-therapy bus collision.

---

## 3. Final Agreed Implementation Plan

Based on your feedback, here is exactly what we will implement in the code:

1.  **Fix A5 (The Heap Saver):** 
    *   Modify `SD_MMC.begin(...)` to include `maxOpenFiles=2`.
    *   Change `CONFIG_WL_SECTOR_SIZE=4096` to `512` in `sdkconfig.defaults`.
2.  **Fix B1 (The Phantom Uploader):** 
    *   Apply the `MAX_DAYS` filter inside the work probe so old folders don't trigger empty upload sessions.
3.  **Fix B2 (The CPAP Protector):** 
    *   Add a `MIN_SUPPRESSION_MINUTES` timer. Once suppression is set, do not clear it on bus activity until that timer expires, keeping the MUX stable during therapy.
4.  **Fix B3 (The Premature Completer):** 
    *   Track per-folder upload success accurately in `runFullSession()` so failures aren't accidentally marked as completed.

*(Note: B4 is left alone as requested, and A7/A8 are discarded).*
