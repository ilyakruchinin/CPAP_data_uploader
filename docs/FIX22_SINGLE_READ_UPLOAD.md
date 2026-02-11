# Fix 22: Single-Read Upload with Size-Based Change Detection

## Problem

The ESP32 firmware reads each CPAP file from the SD card **3 times** during a SleepHQ upload:

| Read | Function | Purpose |
|------|----------|---------|
| 1 | `computeContentHash()` | MD5(content + filename) for SleepHQ dedup |
| 2 | `httpMultipartUpload()` | Send file bytes over TLS |
| 3 | `calculateChecksum()` | MD5(content) stored for next-session change detection |

Additionally, for recent-folder re-scans, `hasFileChanged()` performs a **4th full read** just to check if the file was modified — even though DATALOG files are append-only and a simple size comparison suffices.

Each SD read holds the SPI bus, blocking the CPAP machine from writing therapy data. For a 1MB BRP.edf file, this means 3–4MB of redundant SD I/O per file.

## Solution: Two Optimizations

### Optimization 1: Size-Based Change Detection for DATALOG Files

**Assumption (safe):** ResMed AirSense 11 DATALOG `.edf` files are append-only. New therapy data = file grows. No new data = file size unchanged. The EDF header is fixed-size and pre-allocated at file creation, so header metadata updates don't change file size.

**Change:** For files under `/DATALOG/`, compare `file.size()` (a metadata-only operation — no SPI data transfer) against the stored size from the last successful upload. If size matches → skip entirely. If size differs → file needs uploading.

**Impact:** Unchanged files go from 1 full SD read (MD5 hash) to 0 reads (stat call only).

**Implementation:**
- Store `fileSize` alongside `checksum` in `UploadStateManager::fileChecksums`
- New method `hasFileSizeChanged(path, currentSize)` — pure in-memory lookup
- Use size-based check in `uploadDatalogFolder()` re-scan path instead of `hasFileChanged()`
- Non-DATALOG files (root, SETTINGS) keep hash-based detection (they can change in-place)

### Optimization 2: Single-Read Streaming Upload with Progressive Hash

**Goal:** Read each file from SD exactly **once**. Compute the SleepHQ content hash progressively during that single read, interleaving SD release between chunks.

**Key insight:** The SleepHQ `content_hash` multipart field can be placed **after** the file data in the multipart body. Multipart field order is irrelevant to the server (Rails parses the entire body into a params hash before processing). The `Content-Length` is still calculable because the hash is always a fixed 32-char hex string.

**New multipart body structure:**
```
--boundary
Content-Disposition: form-data; name="name"
<filename>
--boundary
Content-Disposition: form-data; name="path"
<dirpath>
--boundary
Content-Disposition: form-data; name="file"; filename="<filename>"
Content-Type: application/octet-stream
<file content — streamed with progressive hash>
--boundary
Content-Disposition: form-data; name="content_hash"
<hash — computed during streaming, sent after file>
--boundary--
```

**Interleaved SD release flow (per chunk):**
```
1. Open file, note size
2. Connect TLS, send HTTP headers + multipart preamble + file part header
3. Loop until all bytes sent:
   a. Take SD control (if not held)
   b. Read chunk into buffer (e.g. 4KB)
   c. Release SD → CPAP gets access
   d. Update MD5 context with chunk (CPU only)
   e. Send chunk over TLS (network only, CPAP still has SD)
4. Close file
5. Finalize both MD5 contexts:
   - MD5(content + filename) → SleepHQ content_hash
   - MD5(content) → state tracking checksum (non-DATALOG files only)
6. Send file part trailer + content_hash part + closing boundary
7. Wait for server response (CPAP has SD)
```

**Impact:** File read count drops from 3 to 1. SD hold time per chunk is ~1ms (read 4KB at ~4MB/s SPI), with release during the ~50-500ms network send. CPAP gets ~98% of SD time during uploads.

## Files Modified

| File | Changes |
|------|---------|
| `include/UploadStateManager.h` | Add `fileSizes` map, `hasFileSizeChanged()`, `markFileUploadedWithSize()` |
| `src/UploadStateManager.cpp` | Implement size tracking, persist in state JSON |
| `include/SleepHQUploader.h` | New `uploadStreaming()` replacing `computeContentHash()` + `httpMultipartUpload()` |
| `src/SleepHQUploader.cpp` | Single-read streaming upload with progressive hash, hash-after-file multipart |
| `src/FileUploader.cpp` | Size-based check in DATALOG re-scan, store size after upload, eliminate post-upload `calculateChecksum()` for DATALOG |

## Backward Compatibility

- State file gains optional `file_sizes` object. Old state files without it work fine (size lookup returns 0, triggering re-upload — safe).
- Non-DATALOG files continue using hash-based change detection.
- SMB/WebDAV backends are unaffected (they don't use content_hash).
