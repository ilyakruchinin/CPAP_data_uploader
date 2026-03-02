# Cloud Upload Heap Analysis

## Executive Summary

**There is no memory leak.** The `ma` (max contiguous allocation) drop from 110KB to
39KB is caused by **heap fragmentation** from multiple TLS handshakes and the SD_MMC
driver, not by leaked allocations. Free heap (`fh`) recovers to ~146KB after SD
release, confirming all upload-related memory is properly freed.

The permanent `ma=38900` floor is a fragmentation ceiling — once established during
the first upload session, it persists across all subsequent sessions but **does not
degrade further** because TLS keep-alive prevents additional handshakes.

---

## Heap Timeline (Session 1)

| Event | `fh` | `ma` | Δ `ma` | Cause |
|---|---|---|---|---|
| Boot | 275,792 | 110,580 | — | Baseline after BT release |
| WiFi + OTA + WebServer | 195,300 | 110,580 | 0 | Services use non-contiguous blocks |
| **SD card mounted** | 160,728 | **86,004** | **−24,576** | SD_MMC DMA buffers (recoverable) |
| Upload task spawned | 144,276 | 86,004 | 0 | 16KB task stack from different region |
| **OAuth TLS handshake** | 140,436 | **49,140** | **−36,864** | mbedTLS record buffers + handshake |
| Team ID discovered | 140,776 | 49,140 | 0 | Reuses existing TLS (keep-alive) |
| Import created (raw TLS) | 140,536 | 51,188 | +2,048 | HTTPClient freed, small coalesce |
| **First file TLS reconnect** | 94,592 | **38,900** | **−12,288** | mbedTLS buffers re-allocated at new address |
| Steady-state uploading | ~97,000 | 38,900 | 0 | Keep-alive, no more handshakes |
| SD released + cooldown | 146,100 | 38,900 | 0 | SD freed but fragmentation permanent |

### Sessions 2–4: No Further Degradation

| Event | `ma` | Notes |
|---|---|---|
| Session 2 start | 38,900 | TLS kept alive from session 1 |
| Session 2 import (raw TLS) | 38,900 | No new handshake needed |
| Session 2 uploading | 38,900 | Stable throughout |
| Session 3, 4 | 38,900 | Identical — fragmentation is static |

**Conclusion**: The fragmentation is a one-time cost. Once the mbedTLS buffers find
their permanent home in the heap, `ma` stabilizes.

---

## Root Cause Analysis

### 1. SD_MMC Driver: −24KB of `ma` (recoverable)

When `SD_MMC.begin()` is called, the ESP-IDF SDMMC driver allocates DMA-capable
buffers from DRAM. These are contiguous allocations that reduce `ma` by exactly
24KB (likely 3× 8KB DMA descriptors + data buffers).

**This is recoverable** — `SD_MMC.end()` frees these buffers and `ma` would return
to its pre-mount value if no other fragmentation existed.

### 2. First TLS Handshake (OAuth): −37KB of `ma` (permanent fragmentation)

The `httpRequest()` method creates a stack-local `HTTPClient`, calls
`http.begin(*tlsClient, url)`, which triggers `WiFiClientSecure::connect()`.
This initiates an mbedTLS handshake that allocates:

| Allocation | Size | Lifetime |
|---|---|---|
| `mbedtls_ssl_context` + config | ~500 bytes | Connection lifetime |
| **Input record buffer** | **16,384 bytes** | Connection lifetime |
| **Output record buffer** | **16,384 bytes** | Connection lifetime |
| Peer certificate chain | ~2–4 KB | Connection lifetime (`KEEP_PEER_CERTIFICATE=y`) |
| Handshake temp buffers | ~5–10 KB | **Freed after handshake** |
| HTTPClient internal Strings | ~1–2 KB | **Freed on `http.end()`** |
| Total persistent | **~35 KB** | |

**Critical detail**: `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` is **not set** in the
compiled SDK, so both IN and OUT buffers are 16KB each = **32KB symmetric**. This is
the single largest allocation consumer.

Additionally, `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=y` means the full server
certificate chain (~2–4KB) is kept in memory for the connection's lifetime.

The transient handshake buffers (~5–10KB) and HTTPClient Strings are freed after
the request completes, but they leave "holes" in the heap between the persistent
mbedTLS buffers and other allocations. This is what causes the permanent `ma` drop.

### 3. Mid-Session TLS Reconnect: −12KB additional (the critical fragmenter)

Between `createImport()` (which succeeds via raw TLS) and the first file upload
via `httpMultipartUpload()`, the TLS connection dies. The log confirms:

```
[16:53:52] Import created: 12696532
[16:53:54] Streaming: establishing TLS connection (attempt 1, free: 137716, max_alloc: 51188)
[16:54:06] Uploaded: 20260302_003708_CSL.edf → ma=38900
```

The connection dropped in the ~2 seconds between import creation and first file
upload. The most likely causes:

1. **Server sent `Connection: close`** in the createImport response — the raw TLS
   code in `createImport()` does NOT parse the `Connection` header
2. **Server-side keep-alive timeout** — unlikely at only 2 seconds
3. **Unread trailing bytes** from the createImport response corrupting the next read

When `httpMultipartUpload()` detects the dead connection, it calls
`tlsClient->connect(host, port)`. This:
1. Calls `mbedtls_ssl_free()` — frees the old 32KB record buffers
2. Allocates NEW 32KB record buffers — but at a **different heap address**
3. The old address region is now free but fragmented by small allocations that
   were placed nearby between handshakes

This is why `ma` drops another 12KB: the new buffers had to go in a smaller
contiguous region.

### 4. HTTPClient as a Fragmentation Amplifier

The `httpRequest()` method creates/destroys an `HTTPClient` for each API call
(OAuth, team discovery). Each `HTTPClient` instance allocates internal `String`
objects and transport structures on the heap. Even though these are freed when
`http.end()` + destructor runs, they create micro-holes between the persistent
mbedTLS buffers.

The flow:
```
authenticate() → HTTPClient allocs [A] + mbedTLS allocs [B] → HTTPClient freed [A]
discoverTeamId() → HTTPClient allocs [C] in gap [A] → HTTPClient freed [C]
createImport() → raw TLS, no new allocs
                 ↑ but [A] and [C] gaps now fragment the space around [B]
```

---

## What's Working Well

1. **TLS keep-alive across files**: Once connected, the TLS session persists for
   all files in a folder and across import cycles. No per-file reconnection.

2. **Raw TLS for createImport/processImport**: Bypasses HTTPClient for these calls,
   avoiding additional heap churn when `ma` is already low.

3. **Stack-based buffers throughout**: `httpMultipartUpload()` uses stack `char[]`
   for headers, boundary, response parsing. No heap Strings during file streaming.

4. **4KB upload buffer**: `CLOUD_UPLOAD_BUFFER_SIZE=4096` is appropriately sized —
   large enough for throughput, small enough to not fragment.

5. **UploadStateManager**: Uses fixed-size arrays (not JSON/heap), journal-append
   pattern, stack buffers for I/O. **Zero heap churn** during uploads.

6. **Stable steady-state**: `ma=38900` with `fh≈97–100KB` during active uploads
   is healthy. 38.9KB contiguous is sufficient for continued operation since no
   new TLS handshakes are needed.

---

## Potential Improvements (Ranked by Impact)

### HIGH IMPACT

#### 1. Enable Asymmetric mbedTLS Buffers — Save 12KB

Add to `sdkconfig.defaults`:
```
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
```

**Why**: The SleepHQ API responses are small (< 2KB JSON). We never receive TLS
records > 4KB from the server. The 16KB OUT buffer is needed for sending large
multipart file chunks, but the IN buffer can safely be 4KB.

**Savings**: 12KB freed from persistent mbedTLS allocations. This directly raises
the `ma` floor.

**Risk**: LOW. If any server response exceeds 4KB in a single TLS record, mbedTLS
will return an error and we'd retry. The SleepHQ API uses chunked transfer for
large responses, so individual records stay small.

**CAVEAT**: This requires the pre-compiled Arduino-ESP32 framework to support this
option. Since `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN` is a Kconfig option, it may
require a custom ESP-IDF build or an arduino-esp32 version that exposes it. If the
pre-compiled `.a` libraries don't respect `sdkconfig.defaults` overrides for mbedTLS
buffer sizes (because mbedTLS is compiled into a static lib), this change would have
no effect. **Verify by checking `ma` delta before and after.**

#### 2. Eliminate the Mid-Session TLS Reconnect

The connection drops between `createImport()` and the first file upload. Fix by:

**Option A**: In `createImport()` raw TLS path, parse the `Connection` response
header. If `Connection: close`, call `tlsClient->stop()` but DON'T delete/recreate
the WiFiClientSecure — just let `httpMultipartUpload()` reconnect on the same object.
This keeps the mbedTLS memory in the same heap region.

**Option B** (better): Move the first TLS connect into `begin()` BEFORE OAuth, so the
mbedTLS buffers are allocated early when `ma` is highest. Then keep the connection
alive through OAuth → team discovery → createImport → first upload via raw TLS for
ALL calls (eliminate HTTPClient entirely from the cloud path).

**Savings**: Could prevent the 12KB `ma` drop entirely (38900 → ~51000).

#### 3. Eliminate HTTPClient from the Auth Path

Replace `httpRequest()` (which creates/destroys HTTPClient per call) with raw TLS
for OAuth and team discovery, using the same pattern as `createImport()`.

**Why**: HTTPClient allocates internal `String` objects and transport structures that
create fragmentation gaps between mbedTLS buffers. Using raw TLS for ALL API calls
eliminates this source of interleaved micro-allocations.

**Savings**: Reduces fragmentation from the OAuth → teamID → createImport sequence.
Combined with #2, could keep `ma` at ~51KB+ throughout the session.

### MEDIUM IMPACT

#### 4. Disable `KEEP_PEER_CERTIFICATE` — Save ~2–4KB

Add to `sdkconfig.defaults`:
```
CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE=n
```

**Why**: The server certificate chain is only needed during handshake verification.
Keeping it in memory afterward wastes 2–4KB per connection.

**Savings**: 2–4KB freed. Modest but free.

**CAVEAT**: Same caveat as #1 — may require custom ESP-IDF build. The pre-compiled
Arduino framework may ignore this override.

#### 5. Pre-Allocate TLS Before SD Mount

Call `setupTLS()` + `tlsClient->connect(host, 443)` during the NTP sync window
(before SD_MMC.begin()). This "claims" a contiguous heap region for mbedTLS before
the SD driver fragments the space.

**Flow change**:
```
Boot → WiFi connect → NTP sync → TLS pre-warm → SD mount → Upload
```

**Savings**: The mbedTLS 32KB block would be allocated at `ma=110580` instead of
`ma=86004`, giving the allocator the best possible contiguous space.

**Risk**: MEDIUM. If the pre-warm connection dies before the upload starts (server
keep-alive timeout, typically 30–60s), we'd need to reconnect anyway. But even if
it dies, the mbedTLS buffers are freed in-place and the re-allocation can reuse the
same region (no intervening allocations).

#### 6. Enable Variable Buffer Length

Add to `sdkconfig.defaults`:
```
CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH=y
```

**Why**: With this enabled, mbedTLS shrinks the I/O buffers after the handshake to
match the actual max record size negotiated with the server. For typical HTTPS APIs,
this can shrink the IN buffer from 16KB to ~4KB after handshake.

**Savings**: Up to 12KB freed after handshake completes.

**Same caveat**: May require custom ESP-IDF build.

### LOW IMPACT / INFORMATIONAL

#### 7. StaticJsonDocument Stack Usage

The `StaticJsonDocument<2048>` in `discoverTeamId()` and `createImport()` fallback
path allocates 2KB on the **stack**, not the heap. This is fine for the 16KB upload
task stack. No heap impact.

#### 8. String Member Variables

`accessToken`, `teamId`, `currentImportId` are `String` members of
`SleepHQUploader`. These are small (~100–200 bytes total) and have stable sizes
after initialization. Not a fragmentation concern.

#### 9. computeContentHash() Buffer

Uses a 4KB stack buffer for MD5 hashing. No heap impact. Called per-file but the
stack is reused each time.

---

## Why `ma` Never Recovers Even After SD Release

After the upload session ends:
1. SD_MMC.end() frees the 24KB DMA buffers
2. Upload task stack (16KB) is freed
3. The SleepHQUploader object persists (it's a member of FileUploader)
4. The WiFiClientSecure + mbedTLS buffers persist (~35KB)

So `fh` recovers to ~146KB (vs 195KB pre-mount), but `ma` stays at 38900 because
the mbedTLS buffers are sitting in the middle of the heap, splitting the free space
into two regions — neither of which is larger than 38900 bytes.

**To recover `ma`**, the TLS connection would need to be torn down
(`sleephqUploader->resetConnection()`) during cooldown. But this would force a new
TLS handshake on the next upload session, potentially at an even lower `ma` if other
allocations have moved. The current approach of keeping TLS alive is actually the
**safer strategy**.

---

## Recommendations Summary

| # | Change | `ma` Impact | Effort | Risk |
|---|---|---|---|---|
| 1 | Asymmetric mbedTLS buffers (4KB IN) | +12KB | Low | Low* |
| 2 | Fix mid-session reconnect | +12KB | Medium | Low |
| 3 | Eliminate HTTPClient from auth | +5–8KB | Medium | Low |
| 4 | Disable KEEP_PEER_CERTIFICATE | +2–4KB | Low | Low* |
| 5 | Pre-allocate TLS before SD mount | +10–15KB | Medium | Medium |
| 6 | Enable variable buffer length | +8–12KB | Low | Low* |

*Requires verification that pre-compiled Arduino-ESP32 framework respects sdkconfig
overrides for mbedTLS. If not, a custom ESP-IDF partition or framework build is needed.

**Best-case combined**: `ma` could improve from 38,900 to ~65,000–75,000, providing
a much healthier margin for TLS reconnection recovery.

**Pragmatic first step**: Items #2 and #3 (fix reconnect + eliminate HTTPClient) are
pure code changes with no framework dependency. They address the two biggest
fragmentation sources and could raise `ma` to ~51–55KB.

---

## Appendix: Is 38.9KB `ma` Actually a Problem?

For CLOUD-only uploads: **No, it's fine.** The session works perfectly:
- TLS keep-alive means no new handshakes (which need ~36KB contiguous)
- All per-file buffers fit comfortably in 38.9KB
- `fh` stays at ~97–100KB, providing plenty of total memory
- The system runs for hours across multiple sessions without degradation

For **SMB+CLOUD mixed** mode: **Yes, it could be.** If SMB needs to reconnect
(e.g., after a transport failure), `smb2_connect_share()` needs contiguous heap for
its internal buffers. At `ma=38900`, this is marginal.

For **TLS reconnection recovery**: **Marginal.** If the server drops the keep-alive
connection mid-session, a new TLS handshake needs ~36KB contiguous. At `ma=38900`,
this would succeed but with zero margin. The improvements above would provide a
healthy buffer.

---

## Implementation Status

All recommendations have been implemented:

### Completed Changes

| # | Change | Files Modified |
|---|---|---|
| 1 | Asymmetric mbedTLS buffers (4KB OUT) | `sdkconfig.defaults` |
| 2 | Eliminate mid-session reconnect | `src/SleepHQUploader.cpp` — `httpRequest()` now parses `Connection: close` |
| 3 | Eliminate HTTPClient from auth path | `src/SleepHQUploader.cpp` — `httpRequest()` rewritten to raw TLS |
| 4 | Disable KEEP_PEER_CERTIFICATE | `sdkconfig.defaults` |
| 5 | Pre-allocate TLS before SD mount | `src/main.cpp` — `handleAcquiring()` calls `preWarmTLS()` before `takeControl()` |
| 6 | Enable variable buffer length | `sdkconfig.defaults` |

### Code Impact

- **`src/SleepHQUploader.cpp`**: Removed `#include <HTTPClient.h>`. Rewrote
  `httpRequest()` to use raw TLS with zero heap allocation (stack buffers only).
  Simplified `createImport()` and `processImport()` to use `httpRequest()` instead
  of maintaining separate raw TLS + HTTPClient fallback paths.  Added `parseHostPort()`
  shared helper and `preWarmTLS()` public method.  Net reduction: ~300 lines removed.
- **`include/SleepHQUploader.h`**: Added `parseHostPort()` (private) and
  `preWarmTLS()` (public) declarations.
- **`src/main.cpp`**: `handleAcquiring()` calls `preWarmTLS()` before SD mount
  when cloud is configured — allocates mbedTLS buffers while `ma` is at ~110KB.
- **`sdkconfig.defaults`**: Added `CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN`,
  `CONFIG_MBEDTLS_SSL_VARIABLE_BUFFER_LENGTH`, disabled
  `CONFIG_MBEDTLS_SSL_KEEP_PEER_CERTIFICATE`.  These may require a custom ESP-IDF
  build to take effect with pre-compiled Arduino framework libraries.

### Build Verification

Both `pico32` and `pico32-ota` targets build successfully.
