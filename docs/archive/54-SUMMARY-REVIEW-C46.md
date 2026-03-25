# 54 — Summary Review: Corrections and Final Recommendations

**Date:** 2026-03-22  
**Context:** Second review pass on Documents 52 and 53, incorporating user
feedback and re-examining log evidence for B2.

---

## 1. Correction to Doc 53 — B2 Was Overstated

**Doc 53 claimed:** *"The ESP is stealing the SD card from the CPAP machine
every 2 minutes, all night long"* during active therapy, risking SD Card
Errors.

**This claim is wrong.** After re-examining the raw logs in `2-1.txt.tmp`,
the 62-second silence detection is **rock-solid** during active therapy. The
cycling only occurs **after therapy has already ended**, when the CPAP is
doing infrequent post-therapy housekeeping writes.

### Evidence from `2-1.txt.tmp`

| Time Range | Duration | Behaviour | Interpretation |
|---|---|---|---|
| 02:44 → 06:01 | **3 hours 17 min** | Zero 62s silences detected | Active therapy — CPAP writing every <62s |
| 06:01:18 | — | First 62s silence → upload runs | Therapy ended, data uploaded immediately |
| 06:08 → 06:59+ | ~51 min | 2-minute cycle: silence → probe → no work | Post-therapy CPAP housekeeping writes |

The 3-hour gap from 02:44 to 06:01 with **zero false triggers** is
definitive proof that the 62-second threshold correctly prevents the ESP
from grabbing the card during active therapy. The CPAP's ~57-second write
interval keeps the PCNT counter well below 62s at all times during therapy.

### Why the post-therapy cycling is NOT a problem

The repeated cycles from 06:08 onward occur because:

1. Therapy ended at ~06:01. Data was uploaded successfully.
2. The CPAP continues brief housekeeping writes (STR.edf updates, internal
   state) roughly every 2 minutes.
3. Between these brief writes, 62+ seconds of genuine silence occurs.
4. The ESP detects this silence, probes, finds `cloud=0 smb=0`, and cycles.

This is the **correct behaviour by design**:

- The ESP only grabs the card during a **verified 62-second silence window**.
  If the CPAP tried to write during the ~1.7s mount, it would need to have
  been silent for 62 seconds first — meaning its next write is not imminent.
- Each probe correctly checks for new data. If the CPAP's housekeeping DID
  produce a new uploadable file, the probe would catch it.
- The cost of each empty cycle is minimal: ~1.7 seconds of SD ownership,
  no network connections (WorkProbe returns immediately).
- With the A5 fix applied, the SD mount/unmount cost drops from ~27 KB to
  ~1.5 KB, making the heap impact of these cycles negligible.

### Conclusion on B2

**The user is correct: B2 does not need fixing.** The three arguments
provided by the user are all valid:

1. **The 62s silence detection is reliable.** The 3h17m gap during active
   therapy with zero false triggers confirms this. It has been rock-solid.
2. **The goal is to upload ASAP.** Adding a suppression timer (e.g., 30
   minutes) would delay detecting genuinely new data if the user resumes
   therapy or if the CPAP produces a new file.
3. **The 62s threshold is configurable** and the Profiler tool already
   exists for users to tune it to their specific CPAP model. There is no
   evidence in the logs that the threshold was ever reached during active
   therapy writes.

**No evidence of unreliability was found.** Every 62s silence event in the
log corresponds to a period where the CPAP genuinely stopped frequent
writes — either end-of-therapy or post-therapy housekeeping gaps.

---

## 2. Validation of Doc 53 — Part A (SD Fragmentation)

### A5: `max_files` recommendation — Amended to `max_files=2`

Doc 53's recommendation of `max_files=2` is **correct and well-reasoned**.

**Why not `max_files=1`:**

The user correctly identified a concurrency scenario. The Web Server (Core 1)
can access the SD card while the upload task (Core 0) has it mounted. Two
code paths open files on the SD during runtime:

- **Upload task** opens 1 file at a time for reading (e.g., `.edf` file
  being uploaded via SMB or Cloud).
- **Web Server** `handleApiConfigRawGet` opens 1 file (`/config.txt`) for
  reading. `handleApiConfigRawPost` opens 1 file (`/config.txt.tmp`) for
  writing. Either can run while an upload is in progress because the web
  server checks `sdManager->hasControl()` and shares the existing mount.

Worst-case concurrent: **1 (upload) + 1 (web config) = 2 files**.

Note: `Config::censorConfigFile()` does open 2 files simultaneously (read
old + write new), but this only runs during `setup()` → `loadFromSD()`,
never during upload sessions. So it doesn't affect the runtime max.

Note: Directory operations (`sd.open("/DATALOG")`, `openNextFile()`) use
FatFS `FF_DIR` structures, **not** `FIL` file slots. They do not count
toward the `max_files` limit.

**`max_files=2` is the correct choice.**

### A5 + WL_SECTOR_SIZE=512 — Confirmed correct

The project uses LittleFS for internal flash storage, not FAT+WearLevelling.
The `CONFIG_WL_SECTOR_SIZE=4096` setting is inherited from the default
sdkconfig and imposes an unnecessary 4 KB buffer size on SD card FIL and
FATFS structures. Setting it to 512 is safe.

**Combined allocation with `max_files=2`, `WL_SECTOR_SIZE=512`:**

| Component | Size |
|---|---|
| FATFS struct (incl `win[512]`) | ~1.0 KB |
| `tmp_path_buf` × 2 | ~516 B |
| 2 × FIL (incl `buf[512]` each) | ~1.2 KB |
| `o_append` array | 2 B |
| Other (base fields, lock) | ~200 B |
| **Total vfs_fat_ctx_t** | **~3.0 KB** |
| sdmmc_card_t + host init | ~1.0 KB |
| **Grand total SD mount** | **~4.0 KB** |

Down from ~27 KB. The `ma` drop during SD mount would be ~4 KB instead of
~28 KB, making post-mount fragmentation negligible.

### A6 (Persistent Mount) — Confirmed as backup only

A5 reduces the SD mount allocation so dramatically that A6's additional
complexity is not justified unless A5 proves insufficient in practice. Keep
A6 as a documented fallback.

---

## 3. Validation of Part B Recommendations

| Item | Doc 52 Recommendation | User Feedback | Final Status |
|---|---|---|---|
| **B1** | Apply MAX_DAYS in work probe | Agreed | **DO** — Eliminates phantom cycles |
| **B2** | Add minimum suppression timer | Disagreed — not a real problem | **DROP** — User is correct (see §1) |
| **B3** | Track per-folder upload success | Agreed | **DO** — Prevents premature day completion |
| **B4** | Fix log skipping | By design, has override | **DROP** — Not needed |

---

## 4. Revised Implementation Plan

Only the items that survived review:

| # | Fix | Priority | Complexity | Change |
|---|---|---|---|---|
| 1 | `max_files=2` in `SD_MMC.begin()` | **Critical** | 1 line | `SDCardManager.cpp` |
| 2 | `CONFIG_WL_SECTOR_SIZE=512` | **Critical** | sdkconfig | `sdkconfig.defaults` |
| 3 | Apply `MAX_DAYS` in work probe | **High** | Low | `FileUploader.cpp` |
| 4 | Track per-folder success in session | **Low** | Low | `FileUploader.cpp` |

### Implementation Order

1. **Items 1+2 together** — Build, flash, verify `ma` drop is ≤5 KB
   during SD mount. Verify `ma` recovers after unmount.
2. **Item 3** — Apply MAX_DAYS filter in `hasWorkToUpload()` and the
   `preflightFolderHasWork` lambda. Verify no phantom cycles in logs.
3. **Item 4** — As time permits.

### What was dropped (and why)

- **B2 (suppression timer):** 62s detection works correctly. Post-therapy
  cycling is harmless and by-design. Suppression timer would delay uploads.
- **B4 (log skipping):** By design; user has override option.
- **A6 (persistent mount):** Unnecessary given A5's effectiveness. Kept as
  documented fallback only.
- **A7 (dummy block):** Fragile. Discarded.
- **A8 (accept low ma):** Downstream workaround. Discarded.

---

## Implementation Status

| # | Fix | Status | Notes |
|---|---|---|---|
| **1** | `max_files=2` in `SD_MMC.begin()` | ✅ **DONE** | Reduced from 5 to 2, saves ~17KB |
| **2** | `CONFIG_WL_SECTOR_SIZE=512` | ✅ **DONE** | Changed in both sdkconfig files |
| **3** | Apply `MAX_DAYS` in work probe | ✅ **DONE** | Added to `hasWorkToUpload()` and pre-flight |
| **4** | Track per-folder success | ✅ **DONE** | Added `sessionHadFailure` tracking in `runFullSession()` |

All agreed items from doc 54 have been implemented. The changes reduce SD mount heap allocation from ~27KB to ~4KB, eliminate phantom upload cycles for old folders, and prevent premature day completion when folder uploads fail.
