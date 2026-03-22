# 52 — Summary of Fixes Needed (CO46 Log Analysis)

**Date:** 2026-03-22  
**Logs analysed:** `2-1.txt.tmp`, `2-2.txt.tmp`, `2-3.txt.tmp`  
**Firmware:** `v2.0i-beta2-dev+2 (2f51804)`

---

## Part A — SD Card Mount Fragmentation (Deep Research)

### A.1  The Problem

Every `SD_MMC.begin()` / `SD_MMC.end()` cycle allocates ~27 KB from the
general heap and then frees it on unmount. When **any other allocation**
(WiFi, web server, mDNS, log buffer, etc.) lands between those blocks during
the mount window, the freed regions can no longer merge. The result is a
permanent drop in maximum contiguous allocation (`ma`):

| Stage | `fh` (total free) | `ma` (max contiguous) |
|---|---|---|
| Before first SD mount | 107–112 KB | 63–65 KB |
| During SD mount | 80–82 KB | 36,852 |
| After SD unmount | 107–112 KB | **36,852 — never recovers** |

`ma = 36852` is the **permanent fragmentation floor** for the rest of the
boot. It is sufficient for TLS (handled by the arena) but marginal for
libsmb2 PDU allocation.

### A.2  Allocation Breakdown — Where the 27 KB Goes

The Arduino `SD_MMC.begin()` calls `esp_vfs_fat_sdmmc_mount()` which
ultimately allocates one large `vfs_fat_ctx_t` via `ff_memalloc()`:

```c
// ESP-IDF vfs_fat.c — esp_vfs_fat_register_cfg()
size_t ctx_size = sizeof(vfs_fat_ctx_t) + max_files * sizeof(FIL);
vfs_fat_ctx_t* fat_ctx = (vfs_fat_ctx_t*) ff_memalloc(ctx_size);
```

The `vfs_fat_ctx_t` struct contains:

| Field | Size | Notes |
|---|---|---|
| `FATFS fs` (incl `win[FF_MAX_SS]`) | ~4.5 KB | `FF_MAX_SS=4096` from sdkconfig |
| `tmp_path_buf[FILENAME_MAX+3]` × 2 | ~516 B | |
| `bool *o_append` array | 5 B | for max_files=5 |
| `FIL files[max_files]` | **~21 KB** | 5 × sizeof(FIL) ≈ 5 × 4200 B |
| base fields, lock, etc. | ~200 B | |
| **Subtotal (vfs_fat_ctx_t)** | **~26 KB** | |

Each `FIL` struct contains a `buf[FF_MAX_SS]` = **4096-byte** sector cache
(because `FF_FS_TINY=0` and `FF_MAX_SS=4096`).

Remaining ~1 KB:

| Allocation | Size | Source |
|---|---|---|
| `sdmmc_card_t` | ~200 B | `malloc()` in `esp_vfs_fat_sdmmc_mount` |
| SDMMC event queue | ~400 B | `xQueueCreate(32, sizeof(sdmmc_event_t))` in `sdmmc_host_init` |
| IO interrupt semaphore | ~100 B | `xSemaphoreCreateBinary()` |
| Interrupt handler | ~200 B | `esp_intr_alloc()` |
| VFS registration | ~100 B | `esp_vfs_register()` |
| `vfs_fat_sd_ctx_t` + `strdup` | ~60 B | context bookkeeping |

**Key finding:** The dominant allocation is the **5 pre-allocated FIL
structures**, each with a 4 KB sector buffer. These account for ~21 KB
of the ~27 KB total — and the code only ever opens **1 file at a time**.

### A.3  Why `FF_MAX_SS = 4096` (Not 512)?

The sdkconfig sets:
```
CONFIG_FATFS_SECTOR_4096=y
CONFIG_WL_SECTOR_SIZE_4096=y
CONFIG_WL_SECTOR_SIZE=4096
```

FatFS derives `FF_MAX_SS = MAX(FF_SS_SDCARD=512, FF_SS_WL=4096) = 4096`.
The 4096-byte WL sector size exists for the wear-levelling component used
when FAT is mounted on internal SPI flash. **This project uses LittleFS for
internal flash, not FAT+WL**, so the 4096-byte WL sector size imposes an
unnecessary cost on SD card buffers.

### A.4  Can We Build an "SD Arena" Like the TLS Arena?

**Short answer: Not directly, but we can achieve the same effect.**

| Approach | Feasibility | Why |
|---|---|---|
| **Hook allocator** (like mbedTLS) | ❌ Not possible | ESP-IDF SDMMC/FAT uses standard `malloc()` / `ff_memalloc()`. No configurable allocator hook like `mbedtls_platform_set_calloc_free()`. |
| **Replace global malloc** | ❌ Dangerous | Would affect ALL tasks/ISRs simultaneously. Race conditions, ISR safety issues. |
| **Patch ESP-IDF source** | ⚠️ Possible but fragile | Could add arena hooks to `vfs_fat.c` and `sdmmc_host.c`, but breaks on framework updates. |
| **Persistent mount** (keep structures alive) | ✅ Best approach | Allocate once at boot, never free. Achieves the arena goal without hooks. |
| **Reduce allocation size** | ✅ Easy quick-win | `max_files=1` + `WL_SECTOR_SIZE=512` shrinks 27 KB → ~4 KB. |

### A.5  Option 1: Reduce Allocation Size (Quick Win)

Two config changes dramatically shrink the SD mount footprint:

**Change A — `maxOpenFiles = 1`** (in `SDCardManager::takeControl`):
```cpp
// Before (uses Arduino default of 5):
SD_MMC.begin("/sdcard", mode, false, SDMMC_FREQ_DEFAULT);

// After (explicit max_files=1):
SD_MMC.begin("/sdcard", mode, false, SDMMC_FREQ_DEFAULT, 1);
```
- Saves: **4 × sizeof(FIL) ≈ 16.8 KB**
- Risk: **Zero** — the code only opens 1 SD file at a time during upload.
  Directory operations (`openDir`/`readDir`) use `FF_DIR`, not `FIL` slots.

**Change B — `CONFIG_WL_SECTOR_SIZE=512`** (in sdkconfig.defaults):
```
CONFIG_FATFS_SECTOR_4096=n     →  CONFIG_FATFS_SECTOR_512=y
CONFIG_WL_SECTOR_SIZE_4096=n   →  CONFIG_WL_SECTOR_SIZE_512=y
CONFIG_WL_SECTOR_SIZE=512
```
- Shrinks `FATFS.win[]` from 4096 → 512 (saves 3.5 KB)
- Shrinks each `FIL.buf[]` from 4096 → 512 (saves 3.5 KB per slot)
- Risk: **Low** — LittleFS doesn't use FAT wear-levelling. Verify that no
  other component depends on `CONFIG_WL_SECTOR_SIZE=4096`.

**Combined effect:**

| Config | vfs_fat_ctx allocation | ma drop |
|---|---|---|
| Current (`max_files=5`, `SS=4096`) | ~26 KB | ~28 KB |
| `max_files=1`, `SS=4096` | ~9 KB | ~10 KB |
| `max_files=1`, `SS=512` | ~2.4 KB | ~3.5 KB |

With both changes, the SD mount barely dents `ma`. Even without the
persistent mount approach, fragmentation risk becomes negligible.

### A.6  Option 2: Persistent SD Mount (Eliminates Fragmentation Entirely)

**Goal:** Allocate all SD/FAT structures once at boot, keep them alive
across mount/unmount cycles, and only re-negotiate with the card on acquire.

**How it works:**

```
BOOT (once):
  sdmmc_host_init()           // allocates queue, semaphore — persistent
  sdmmc_host_init_slot()      // configures GPIO — no heap alloc
  sdmmc_card_init()           // negotiates with card
  esp_vfs_fat_register()      // allocates FATFS, VFS, FIL — persistent
  f_mount(fs, "0:", 1)        // reads FAT into FATFS.win[]

RELEASE (every cycle):
  f_mount(NULL, "0:", 0)      // lazy unmount — invalidates FS state
                               // does NOT free FATFS struct
  // do NOT call sdmmc_host_deinit()
  switch_mux_to_cpap()

ACQUIRE (every cycle):
  switch_mux_to_esp()
  delay(500)
  sdmmc_card_init(&host, card) // re-negotiate (card may have changed state)
  f_mount(fs, "0:", 1)         // remount — re-reads FAT into existing buffer
```

**Advantages:**
- Zero `malloc`/`free` churn during upload cycles
- `ma` never drops due to SD operations
- All SD structures are at predictable heap positions (allocated early at boot)
- Functionally equivalent to an "SD Arena" without needing allocator hooks

**Implementation complexity:** Medium. Requires bypassing the Arduino
`SD_MMC` wrapper and using ESP-IDF low-level APIs directly. The current
`SDCardManager` already wraps `SD_MMC.begin()`/`end()`, so the change is
contained to that class.

**Key risk:** After the CPAP uses the SD card, the card's bus state (width,
speed mode) may have changed. `sdmmc_card_init()` resets this via CMD0 →
CMD8 → ACMD41 → etc. This is the same sequence that runs during normal
`SD_MMC.begin()`, but without the heap allocations.

**When to use:** If Option 1 (reduce allocation size) doesn't provide
sufficient `ma` margin. Option 1 is simpler and should be tried first.

### A.7  Option 3: Heap Reservation Block ("Dummy Block")

**Concept:** Allocate a large block early in `setup()`. Free it immediately
before `SD_MMC.begin()`. SD allocations fill the freed space. After
`SD_MMC.end()`, re-allocate the reservation.

**Assessment:**

| Pro | Con |
|---|---|
| Works with current Arduino wrapper | Fragile — depends on allocator placement |
| No ESP-IDF API changes | Other tasks can steal the freed space |
| Simple concept | Race condition if web server/WiFi allocates during the free window |

The ESP32 heap uses a first-fit allocator. If the reservation block is at
the bottom of the heap, freeing it creates a hole at the bottom, and
first-fit should find it immediately for the next allocation. However,
because the upload task runs on Core 0 while the main loop (Core 1) handles
web requests, a concurrent allocation could split the hole.

**Verdict:** Possible but fragile. Not recommended as a primary solution.
Could work as a belt-and-suspenders backup if combined with Option 1.

### A.8  Option 4: Accept `ma=36852` + Fix libsmb2

**Concept:** Don't fix the fragmentation; instead make libsmb2 work within
the available contiguous heap.

The libsmb2 "Failed to allocate pdu" error occurs during `smb2_stat()` calls
for directory validation. The actual PDU allocation is relatively small
(~200-500 bytes), but at `ma=36852` with the SMB connection active (socket
buffers, SMB context, etc.), the remaining contiguous block is insufficient.

**Potential fixes:**
- Reduce SMB upload buffer (already at 2048 — limited room)
- Skip `smb2_stat()` for directory validation when memory is low (already
  implemented as "Proceeding with direct open" fallback)
- The current fallback **works** when the directory already exists on the
  server — it only fails on the first upload to a new directory

**Verdict:** Good as a **complementary** fix alongside Option 1 or 2.
Not sufficient alone because the first upload to a new directory still fails.

### A.9  Recommended Approach

**Phase A (Quick Win):** Apply Option 1 — reduce `max_files` to 1 and
`WL_SECTOR_SIZE` to 512. This alone should reduce the SD mount heap impact
from ~27 KB to ~4 KB, keeping `ma` above 60 KB after mount.

**Phase B (If needed):** If Phase A doesn't provide sufficient margin (e.g.,
if concurrent web server activity still causes fragmentation during the ~4 KB
allocation window), implement Option 2 (persistent mount) to eliminate heap
churn entirely.

**Phase C (Complementary):** Regardless of Phase A/B, the libsmb2 directory
stat fallback should be hardened to handle the case where the directory
doesn't exist yet at low `ma`. This is already partially implemented but
needs the reconnect+retry path to succeed.

---

## Part B — Other Issues Found in Logs

### B.1  Stale Folder 20260124 Causes Phantom Upload Cycles

**Symptom:** Every upload cycle in `2-3.txt.tmp` reports:
```
Pre-flight: WORK — folder 20260124 has 5 file(s)
```
But `MAX_DAYS=10` filters it out (`only processing folders >= 20260312`),
so no files are actually uploaded. The work probe reports `cloud=1 smb=1`,
triggering full upload sessions (TLS connect + SMB connect) for zero useful
work. Each cycle wastes ~11 seconds of SD+WiFi+TLS activity.

**Root cause:** The work probe (`hasWorkToUpload`) does NOT apply the
`MAX_DAYS` filter. It sees folder 20260124 as having work, but the upload
phase ignores it due to `MAX_DAYS`.

**Fix:** Apply `MAX_DAYS` cutoff inside `hasWorkToUpload()` and inside the
pre-flight `preflightFolderHasWork` lambda. Alternatively, mark folders older
than `MAX_DAYS` as completed so they stop triggering the probe.

**Priority:** Medium — wastes power and creates unnecessary TLS/SMB
connections, but doesn't cause data loss.

### B.2  No-Work Suppression Ineffective During Active CPAP

**Symptom:** In `2-1.txt.tmp` from 06:07–07:00, the CPAP is actively
running. Suppression is set after each no-work probe but immediately cleared
by new bus activity:
```
Nothing to upload — suppressing retries until new bus activity
...
No-work suppression cleared — new bus activity detected
62s of bus silence confirmed
```

This creates a ~2-minute polling loop: suppress → clear → wait 62s →
mount SD → probe → no work → suppress → loop. Each cycle mounts/unmounts
the SD card.

**Fix options:**
- **Minimum suppression duration:** Require at least N minutes (e.g., 10)
  before suppression can be cleared, even if bus activity is detected
- **Exponential backoff:** Double the suppression time after each no-work
  result (cap at e.g., 30 minutes)
- **Day-completed gate:** If the day is already marked completed, don't
  clear suppression for routine bus activity — only for a manual web trigger

**Priority:** Medium — wastes power during active CPAP therapy. Becomes
more important once SD fragmentation is fixed (since each cycle will still
briefly mount the SD card).

### B.3  Day Marked Complete Despite Partial SMB Failure

**Symptom:** In `2-1.txt.tmp` at 06:01:57, the SMB upload of folder
20260321 fails (0/5 files uploaded), but the day is marked as completed
because folder 20260320 (already uploaded — 19 unchanged) reports
`success=yes`.

**Self-healing:** The next cycle detected `smb=1` work remaining, retried,
and succeeded. So the data was eventually uploaded. However, if the device
had lost power or the CPAP turned off between cycles, the SMB upload would
have been lost.

**Root cause:** `hasIncompleteFolders()` doesn't account for folders where
upload was attempted but failed within the same session.

**Fix:** Track per-folder upload success within `runFullSession()`. Only
return `UploadResult::COMPLETE` if ALL folders that were attempted (not just
scanned) report success for all active backends.

**Priority:** Low — the system self-heals on retry, and with the SD
fragmentation fix the SMB failures should largely disappear.

### B.4  LOG NOTICE — Skipped Log Lines

**Symptom:** `2-2.txt.tmp` and `2-3.txt.tmp` show:
```
=== LOG NOTICE ===
Some detailed log lines were skipped before they could be saved...
Approximate bytes skipped: 2724
```

**Cause:** During busy upload periods, log saving is temporarily delayed.
The existing config option `FLUSH_LOGS_DURING_UPLOAD=true` addresses this.

**Priority:** Low — cosmetic. The uploader continues working normally.

---

## Part C — Testing Instructions

### C.1  Validate SD Mount Heap Impact (After Fix)

1. Apply `max_files=1` change to `SDCardManager::takeControl()`
2. Apply `CONFIG_WL_SECTOR_SIZE=512` to sdkconfig.defaults
3. Build and flash
4. Set `DEBUG=true` in config.txt
5. Trigger an upload via web UI
6. Check logs for:
   - `ma` before SD mount (should be ~63–69 KB)
   - `ma` after SD mount (should drop by only ~3–4 KB, not ~28 KB)
   - `ma` after SD unmount (should recover to within ~1 KB of pre-mount)
   - SMB directory operations should succeed without "Failed to allocate pdu"

### C.2  Validate Stale Folder Fix

1. Apply `MAX_DAYS` filter in work probe
2. Check logs for: `WorkProbe Result: cloud=0 smb=0` when only old folders
   have work (instead of `cloud=1 smb=1`)
3. Verify no unnecessary TLS/SMB connections are made

### C.3  Validate No-Work Suppression

1. Apply minimum suppression duration
2. Run with active CPAP for 30+ minutes
3. Check logs: after first "no work" result, suppression should hold for the
   configured minimum duration despite bus activity
4. Manual web trigger should still override suppression

### C.4  Stress Test with `MINIMIZE_REBOOTS=true`

1. Run multiple upload cycles without rebooting
2. Monitor `ma` across cycles — it should stay stable (not degrade)
3. Confirm both Cloud and SMB uploads succeed on every cycle

---

## Part D — Summary of Fixes (Ranked by Priority)

| # | Fix | Priority | Complexity | Impact |
|---|---|---|---|---|
| **D.1** | Reduce `max_files` from 5 → 1 | **Critical** | Trivial (1 line) | Saves ~17 KB heap per mount |
| **D.2** | Reduce `WL_SECTOR_SIZE` 4096 → 512 | **High** | Low (sdkconfig) | Saves additional ~7 KB |
| **D.3** | Apply `MAX_DAYS` in work probe | **High** | Low | Eliminates phantom upload cycles |
| **D.4** | Minimum no-work suppression duration | **Medium** | Low | Reduces power waste during therapy |
| **D.5** | Persistent SD mount (if needed) | **Medium** | Medium | Eliminates ALL SD heap churn |
| **D.6** | Track per-folder success in session | **Low** | Low | Prevents premature day completion |
| **D.7** | Harden libsmb2 directory creation | **Low** | Low | Belt-and-suspenders for low-ma SMB |

### Implementation Order

1. **D.1 + D.2** — Apply together, rebuild, flash, test (Part C.1)
2. **D.3** — Apply, test (Part C.2)
3. **D.4** — Apply, test (Part C.3)
4. **D.5** — Only if D.1+D.2 don't provide sufficient `ma` margin
5. **D.6 + D.7** — Apply as time permits
