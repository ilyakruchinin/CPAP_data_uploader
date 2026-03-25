# CPAP AutoSync v2.0i-alpha3

**Incremental patch release on top of v2.0i-alpha2.**
OTA update from v2.0i-alpha1 or v2.0i-alpha2 is supported — no USB flash required.

---

## Bug Fixes

### SMB Upload Failure on Large Files (>32 KB)

**Symptom:** Large files (e.g., 400+ KB EDF recordings) failed at exactly 32768 bytes with `errno=11` (EAGAIN) followed by `errno=9` (EBADF). Small and medium files uploaded successfully, but every file exceeding ~32 KB stalled and triggered a reconnect/retry cycle.

**Root cause:** Under low contiguous heap (`ma ≈ 36 KB`), lwIP has reduced PBUF capacity. SMB write chunks accumulate in the TCP send buffer faster than the stack can drain them. At ~32 KB of in-flight data the send buffer saturates, the next write returns EAGAIN, and during the retry delay lwIP cannot process incoming ACKs — the server resets the connection, turning EAGAIN into EBADF.

**Fix:** A periodic TCP drain pause (10 ms + yield) is injected every 16 KB of data written. This gives lwIP time to process incoming ACKs and drain the send buffer before saturation, preventing the EAGAIN→EBADF cascade on large files.

### Dead-Code PDU Allocation Error Guard

**Symptom:** SMB `stat`/`mkdir` compound commands failed with "Failed to create query command" under heap pressure, but the `isSmbPduAllocationError()` guard never matched — falling through to incorrect "directory does not exist" logic instead of reporting memory pressure.

**Root cause:** The guard only checked for the string `"Failed to allocate pdu"` (set by libsmb2's low-level `pdu.c` on `calloc` failure). However, libsmb2's higher-level callers in `libsmb2.c` **overwrite** that error with operation-specific messages like `"Failed to create query command"`, `"Failed to create write command"`, `"Failed to create create command"`, etc. The guard was effectively dead code — it could never match.

**Fix:** `isSmbPduAllocationError()` now matches all three error patterns from libsmb2:
- `"Failed to allocate pdu"` — low-level calloc failure in `pdu.c`
- `"Failed to create * command"` — higher-level compound command failure in `libsmb2.c`
- `"Failed to allocate *"` — encode-level buffer/name/context allocation failures

Additionally, PDU allocation failures in the `open` and `write` error handlers are now classified as recoverable, triggering a clean SMB disconnect + reconnect rather than a permanent abort.

### Stale TLS Buffers Fragmenting Heap During SMB Phase

**Symptom:** SMB phase started with `ma ≈ 36 KB` even though the TLS pre-warm connection (to SleepHQ) was supposedly released. SMB stat/mkdir compound commands failed due to memory pressure.

**Root cause:** The TLS cleanup before the SMB phase was gated behind `isConnected()`. When the pre-warmed TLS connection died silently during the 30+ seconds of pre-flight scanning (server closed idle connection), `isConnected()` returned `false`, bypassing `resetConnection()`. The mbedTLS internal buffers (~32 KB) remained allocated, fragmenting the heap and reducing the contiguous block available to lwIP and libsmb2.

**Fix:** `resetConnection()` is now called **unconditionally** before the SMB phase, regardless of `isConnected()` state. This ensures mbedTLS buffers and the lwIP socket are always freed. `resetConnection()` is safe to call when already disconnected.

---

## Files Changed

- `src/FileUploader.cpp` — Unconditional TLS resource release before SMB phase
- `src/SMBUploader.cpp` — Fixed `isSmbPduAllocationError()` string matching; TCP drain pause every 16 KB during writes; PDU allocation failures classified as recoverable in open/write error handlers
