# Memory & Heap Stability Proposal — Rebootless Backend Cycling

Date: 2026-03-01
Firmware: v0.11.1-i
Goal: Eliminate the mandatory soft-reboot between SMB↔CLOUD backend cycles by stabilising contiguous heap, and audit all heap anti-patterns across the codebase.

---

## 1. Current Reboot Mechanism

### 1.1 Where It Happens

`main.cpp:handleReleasing()` (lines 797–806):

```cpp
// Otherwise always soft-reboot after a real upload session.
// A clean reboot restores the full contiguous heap and keeps the FSM simple.
// The fast-boot path (ESP_RST_SW) skips cold-boot delays.
LOGF("[FSM] Upload session complete — soft-reboot to restore heap (fh=%u ma=%u)",
     (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
Logger::getInstance().dumpSavedLogsPeriodic(nullptr);
delay(200);
esp_restart();
```

After **every real upload session** (SMB or CLOUD), the firmware reboots. The only exception is when `g_nothingToUpload` is set (pre-flight found no work) — that goes straight to COOLDOWN.

### 1.2 Why It Exists

The reboot was introduced as a pragmatic workaround for heap fragmentation. After a CLOUD session (TLS handshake allocates ~40–50 KB, then releases it), `max_alloc` can drop below the ~36 KB threshold needed for the next TLS handshake. A reboot restores full contiguous heap.

### 1.3 Backend Cycling Flow (SMB+CLOUD)

1. Boot → `FileUploader::begin()` → `selectActiveBackend()` picks SMB (oldest timestamp)
2. Upload SMB session → `handleReleasing()` → **reboot**
3. Boot → `selectActiveBackend()` picks CLOUD (SMB timestamp now newer)
4. Upload CLOUD session → `handleReleasing()` → **reboot**
5. Boot → `selectActiveBackend()` picks SMB again (round-robin)

Each backend runs in a fresh heap. The cycling pointer (`BackendSummary.sessionStartTs`) persists in LittleFS.

### 1.4 Other Reboot Points

| Location | Trigger | Reason |
|:---------|:--------|:-------|
| `main.cpp:914–929` | Upload watchdog (2 min no heartbeat) | Task hung — `vTaskDelete` mid-SD-I/O corrupts bus, only reboot recovers |
| `main.cpp:938–961` | Web UI "Reset State" | NVS flag → reboot → delete state files on clean boot |
| `main.cpp:965–971` | Web UI "Soft Reboot" | User-initiated |
| `main.cpp:975–980` | Web UI "Trigger Upload" | Not a reboot, but triggers FSM cycle |

---

## 2. Proposal: Optional Rebootless Cycling

### 2.1 New Config Key

```ini
MINIMIZE_REBOOTS = false   # default: false (current behaviour)
```

When `true`, the device skips elective soft-reboots after upload sessions and reuses the existing runtime (COOLDOWN → LISTENING loop). Mandatory reboots (watchdog, user-triggered state reset / soft reboot, OTA) still occur. The legacy key name `SKIP_REBOOT_BETWEEN_BACKENDS` is accepted as a backward-compatible alias.

### 2.2 Decision Logic (in `handleReleasing()`)

```
IF g_nothingToUpload → COOLDOWN (no reboot, existing behaviour)
IF config.getMinimizeReboots():
    LOG "MINIMIZE_REBOOTS: skipping elective reboot" + heap stats
    IF max_alloc < 35000 → LOG_WARN "Heap fragmented"
    reset uploadCycleHadTimeout
    → COOLDOWN
ELSE (default):
    flush NAND logs + dump pre-reboot log
    → esp_restart()
```

### 2.3 Reboot Reason Recording

Every `esp_restart()` call should record a reason in NVS before rebooting. On next boot, the reason is read, logged, and cleared.

**NVS key:** `"reboot_reason"` (string, max 32 chars)

| Caller | Reason String |
|:-------|:-------------|
| `handleReleasing()` — normal post-upload | `"upload_session_done"` |
| `handleReleasing()` — heap degraded | `"heap_degraded"` |
| Upload watchdog timeout | `"upload_watchdog"` |
| Web UI "Reset State" | `"state_reset"` |
| Web UI "Soft Reboot" | `"user_soft_reboot"` |

On boot, after Serial init:
```
[INFO] Boot reason: upload_session_done (ESP_RST_SW)
```

This replaces the ad-hoc `watchdog_kill` NVS flag with a unified mechanism.

### 2.4 Backend Teardown (Required for Rebootless Path)

When skipping reboot, the previous backend's resources must be fully released:

1. **SMB → CLOUD transition:**
   - `smbUploader->end()` — disconnect SMB, free libsmb2 context
   - `smbUploader->freeBuffer()` — release the pre-allocated upload buffer (new method needed)
   - Clear `lastVerifiedParentDir` String

2. **CLOUD → SMB transition:**
   - `sleephqUploader->end()` → `resetTLS()` → `delete tlsClient` — free WiFiClientSecure + mbedtls context
   - Clear `accessToken`, `teamId`, `currentImportId` Strings (release heap)

3. **Common:**
   - `FileUploader` must re-run `selectActiveBackend()` to pick the next backend
   - State managers must `save()` then re-`begin()` to reload from LittleFS

---

## 3. Heap & Memory Audit — Findings

### 3.1 Severity Legend

- 🔴 **HIGH** — actively fragments heap or wastes significant contiguous space
- 🟠 **MEDIUM** — contributes to fragmentation over time or wastes moderate memory
- 🟡 **LOW** — minor inefficiency, easy fix

---

### 3.2 Arduino `String` Usage (Heap Fragmentation Source #1)

Arduino `String` uses `realloc()` internally. Every concatenation (`+=`, `+`) may trigger realloc → copy → free of the old buffer, leaving holes in the heap. On ESP32 with ~300 KB total heap, even small holes can prevent a 40 KB TLS allocation.

#### 🔴 F1: `Logger::dumpSavedLogsPeriodic()` — String built char-by-char

**File:** `Logger.cpp:580–587`
```cpp
String logContent;
logContent.reserve(bytesToDump);
for (uint32_t i = 0; i < bytesToDump; i++) {
    logContent += buffer[physicalPos];  // char-by-char append
}
```

**Problem:** Even with `reserve()`, this builds a potentially large String (up to `LOG_BUFFER_SIZE` = 2048 bytes) on heap. If `reserve()` under-estimates or the String gets copied, it fragments.

**Fix:** Write directly to the LittleFS file in chunks from the circular buffer using a small stack buffer (e.g. 256 bytes). No intermediate String needed.

#### 🟠 F2: `Logger::getTimestamp()` returns `String`

**File:** `Logger.cpp:103–119`

Called on **every log line**. Returns a heap-allocated String that is immediately consumed and freed. Over thousands of log calls, this creates micro-fragmentation.

**Fix:** Accept a `char*` buffer parameter instead of returning String. Caller provides stack buffer.

#### 🟠 F3: `CpapWebServer::getUptimeString()` — String concatenation

**File:** `CpapWebServer.cpp:417–432`
```cpp
String uptime = "";
uptime += String(days) + "d ";
uptime += String(hours % 24) + "h ";
// ...
```

**Problem:** Multiple temporary Strings created and destroyed per call.

**Fix:** Use `snprintf()` into a stack buffer, return `const char*` or accept buffer parameter.

#### 🟠 F4: `CpapWebServer::getCurrentTimeString()` — same pattern

**File:** `CpapWebServer.cpp:435–450`

Already uses `snprintf` into a stack buffer but then wraps it in `return String(buffer)`. The String copy is unnecessary.

**Fix:** Accept a `char*` output buffer.

#### 🟠 F5: `CpapWebServer::handleNotFound()` — String JSON construction

**File:** `CpapWebServer.cpp:402`
```cpp
String message = "{\"status\":\"error\",...\"path\":\"" + uri + "\"}";
```

**Fix:** Use `snprintf()` into a stack buffer.

#### 🟠 F6: `CpapWebServer::handleResetState()` — String JSON response

**File:** `CpapWebServer.cpp:359`
```cpp
String response = "{\"status\":\"success\",...}";
```

This is a constant string. Should be a `const char*` literal.

#### 🟠 F7: `SleepHQUploader::httpRequest()` — `responseBody = http.getString()`

**File:** `SleepHQUploader.cpp:838`

`http.getString()` reads the **entire response body** into a heap-allocated Arduino String. For the `/me` endpoint, this can be several KB of JSON. The String is then parsed by ArduinoJson and discarded.

**Fix:** Use `http.getStream()` and feed it directly to `deserializeJson()`. This eliminates the intermediate String entirely. ArduinoJson supports stream input natively.

#### 🟠 F8: `SleepHQUploader` member Strings — never shrunk

**File:** `SleepHQUploader.h:27–33`
```cpp
String accessToken;      // ~64 chars, lives for entire session
String teamId;           // ~6 chars
String currentImportId;  // ~8 chars
```

These persist for the entire upload session. When the session ends, they should be explicitly cleared (`accessToken = String()`) to release heap. Currently they linger until the object is destroyed (which only happens at reboot).

**Fix:** Add a `clearSession()` method that zeroes all session Strings. Call from `end()`.

#### 🟠 F9: `SMBUploader` member Strings — persist after disconnect

**File:** `SMBUploader.h:37–41`
```cpp
String smbServer;
String smbShare;
String smbBasePath;
String smbUser;
String smbPassword;
```

These are set once in the constructor from Config refs and never change. They should be `const` references to Config's own Strings (which already live for the lifetime of the program), eliminating the duplicate heap allocation.

**Fix:** Store `const String&` references or `const char*` pointers to Config's storage.

#### 🟡 F10: `FileUploader` pre-flight lambdas — String temporaries

**File:** `FileUploader.cpp:290–291`
```cpp
String name = String(entry.name());
int sl = name.lastIndexOf('/');
if (sl >= 0) name = name.substring(sl + 1);
```

This pattern (extract filename from path) appears ~10 times across FileUploader. Each creates a temporary String.

**Fix:** Use `strrchr()` on the C string directly. No String allocation needed.

---

### 3.3 `std::vector<String>` Usage (Heap Fragmentation Source #2)

#### 🔴 F11: Folder/file scan vectors in `FileUploader`

**Files:** `FileUploader.cpp:457, 536, 641, 768, 813`

`scanDatalogFolders()`, `scanFolderFiles()`, and `scanSettingsFiles()` all return `std::vector<String>`. Each vector element is a heap-allocated String. The vectors are built, iterated once, then destroyed — creating a burst of heap allocations followed by a burst of frees.

For a CPAP with 30 DATALOG folders of 10 files each, this is 300+ String allocations in a single scan.

**Fix options (in order of preference):**
1. **Callback pattern:** Instead of collecting into a vector, accept a `std::function<bool(const char* name)>` callback that processes each entry inline. Zero heap allocation.
2. **Static char array pool:** Pre-allocate a fixed-size array of `char[16]` entries (DATALOG folder names are always 8 chars; filenames are `YYYYMMDD_HHMMSS_XXX.edf` = ~24 chars). Process in-place.
3. **Reserve + shrink_to_fit:** At minimum, `reserve()` the vector to a reasonable capacity before scanning to avoid realloc during `push_back`.

#### 🟠 F12: Pre-flight `scanFolderFiles()` inside `preflightFolderHasWork` lambda

**File:** `FileUploader.cpp:307, 334`

During pre-flight, `scanFolderFiles()` is called to check if a folder has `.edf` files. It builds a full `std::vector<String>` of all filenames just to check `!files.empty()`. This is wasteful — we only need to know if at least one `.edf` file exists.

**Fix:** Add a `hasFolderFiles()` method that returns `true` on the first `.edf` file found, without collecting all filenames.

---

### 3.4 Dynamic Allocation (`new`) Patterns

#### 🟠 F13: `FileUploader::begin()` — `new` for all subcomponents

**File:** `FileUploader.cpp:67–143`
```cpp
smbUploader = new SMBUploader(...);
smbStateManager = new UploadStateManager();
cloudStateManager = new UploadStateManager();
sleephqUploader = new SleepHQUploader(config);
scheduleManager = new ScheduleManager();
```

Five separate `new` calls, each allocating differently-sized objects on heap. These objects live for the entire session (until reboot).

**Fix:** Make these class members (not pointers) or use placement new with a pre-allocated buffer. Since `FileUploader` itself is already heap-allocated via `new FileUploader(...)` in `main.cpp:478`, the sub-objects could be direct members, eliminating 5 heap allocations and their associated fragmentation.

#### 🟠 F14: `uploader = new FileUploader(...)` and `webServer = new CpapWebServer(...)`

**File:** `main.cpp:478, 524`

These are allocated once at boot and never freed. They should be static objects (stack or BSS) rather than heap-allocated.

**Fix:** Declare as global/static objects. `FileUploader uploader(&config, &wifiManager);` — constructed after Config is loaded.

#### 🟡 F15: `UploadTaskParams* params = new UploadTaskParams{...}`

**File:** `main.cpp:696`

Allocated before each upload task, deleted inside the task. This is a small struct (~16 bytes) but still a heap alloc/free cycle on every upload.

**Fix:** Use a static `UploadTaskParams` global (only one upload task runs at a time).

#### 🟡 F16: `server = new WebServer(80)` in `CpapWebServer::begin()`

**File:** `CpapWebServer.cpp:108`

WebServer is heap-allocated but never freed (until reboot). Could be a member object.

**Fix:** Make `WebServer` a direct member of `CpapWebServer` rather than a pointer.

---

### 3.5 `WiFiClientSecure` (TLS) — The Biggest Single Allocation

#### 🔴 F17: TLS context lifecycle

**File:** `SleepHQUploader.cpp:52–72`

`WiFiClientSecure` is allocated via `new` in `setupTLS()` and freed via `delete` in `resetTLS()`. Each TLS connection allocates ~40–50 KB internally (mbedtls contexts, certificate buffers, I/O buffers). The alloc/free cycle during retries is the **primary source of heap fragmentation**.

**Current pattern:**
```
setupTLS() → new WiFiClientSecure → connect → ... → resetTLS() → delete → setupTLS() → new WiFiClientSecure → ...
```

Each cycle fragments the heap because the ~40 KB block may not be returned to the same location.

**Fix options:**
1. **Allocate once, reuse:** Keep the `WiFiClientSecure` object alive for the entire CLOUD session. On connection failure, call `stop()` (releases mbedtls internals) then `connect()` again — without delete/new of the wrapper object.
2. **Pre-allocate at boot:** Allocate the `WiFiClientSecure` in `SleepHQUploader` constructor (early, when heap is clean) and keep it for the lifetime of the object.
3. **mbedtls static buffers:** ESP-IDF supports `CONFIG_MBEDTLS_DYNAMIC_BUFFER=n` + `CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT=n` which pre-allocates TLS buffers statically. This eliminates the biggest fragmentation source entirely but increases baseline RAM usage.

---

### 3.6 SMB Upload Buffer

#### 🟡 F18: SMB buffer freed too late

**File:** `SMBUploader.cpp:272–291`

The SMB upload buffer is allocated via `malloc()` in `FileUploader::begin()` and only freed in `SMBUploader::~SMBUploader()`. During a CLOUD session, this buffer sits unused on heap, consuming 2–8 KB of contiguous space that could be used for TLS.

**Fix:** Add `SMBUploader::freeBuffer()`. Call it after SMB session completes, before CLOUD session starts (critical for rebootless cycling). Re-allocate before next SMB session.

---

### 3.7 `libsmb2` Context

#### 🟠 F19: libsmb2 internal allocations

The libsmb2 library allocates its own internal buffers (socket buffers, PDU buffers, etc.) via `malloc()`. These are freed on `disconnect()` but the freed regions may not coalesce if other allocations sit between them.

**Mitigation:** Ensure `disconnect()` is called promptly after SMB session ends. Combined with F18 (freeing the upload buffer), this maximises the contiguous block available for subsequent TLS.

---

### 3.8 Logger Circular Buffer

#### 🟡 F20: Logger buffer allocated via `malloc()` at construction

**File:** `Logger.cpp:62`

The 2 KB logger buffer is `malloc()`'d early. Since Logger is a singleton constructed before `setup()`, this allocation happens when heap is clean and stays fixed. **Not a fragmentation concern** — but it could be a static array instead to avoid the malloc entirely.

**Fix:** `static char logBuffer[LOG_BUFFER_SIZE];` and point `buffer` to it.

---

### 3.9 `UploadStateManager` — Good Practice (No Changes Needed)

The UploadStateManager uses **fixed-size arrays** for all state:
- `CompletedFolderEntry completedFolders[368]` — ~1.4 KB
- `PendingFolderEntry pendingFolders[16]` — ~128 bytes
- `FileFingerprintEntry fileEntries[250]` — ~7.5 KB
- `JournalEvent journalEvents[200]` — ~8 KB

This is excellent practice — zero heap fragmentation. The cost is ~17 KB of fixed RAM per instance (×2 for SMB + CLOUD = ~34 KB). This is acceptable.

**Note:** The `String stateSnapshotPath` and `String stateJournalPath` members could be `const char*` literals to avoid two small heap allocations.

---

### 3.10 `Config` — String Members

#### 🟡 F21: Config stores ~15 String members

**File:** `Config.h:34–58`

All configuration values are stored as Arduino Strings. These are loaded once at boot and never change. Since they're allocated early (clean heap) and never freed, they don't cause fragmentation per se — but they do consume ~500 bytes of heap that could be static.

**Fix (low priority):** Use a single `char configBlock[512]` and store offsets/lengths. This is a significant refactor for modest gain.

---

## 4. Implementation Plan (Prioritised)

### Phase 1 — Quick Wins (Low Risk, High Impact on Fragmentation)

| # | Item | Files | Effort | Impact |
|:--|:-----|:------|:-------|:-------|
| 1 | **F1:** Logger dump — write to file directly, eliminate String | `Logger.cpp` | Small | 🔴 Eliminates up to 2 KB transient String |
| 2 | **F17:** TLS — allocate WiFiClientSecure once, reuse across retries | `SleepHQUploader.cpp` | Small | 🔴 Eliminates repeated 40–50 KB alloc/free cycles |
| 3 | **F18:** SMB buffer — add `freeBuffer()`, call after SMB session | `SMBUploader.cpp/h`, `FileUploader.cpp` | Trivial | 🟡 Frees 2–8 KB for CLOUD session |
| 4 | **F8:** SleepHQ — clear session Strings in `end()` | `SleepHQUploader.cpp` | Trivial | 🟠 Frees ~100 bytes of token/ID Strings |
| 5 | **F15:** Static UploadTaskParams | `main.cpp` | Trivial | 🟡 Eliminates one alloc/free per upload |

### Phase 2 — String Elimination (Medium Risk, Cumulative Impact)

| # | Item | Files | Effort | Impact |
|:--|:-----|:------|:-------|:-------|
| 6 | **F7:** Stream JSON responses directly to ArduinoJson | `SleepHQUploader.cpp` | Medium | 🟠 Eliminates multi-KB response Strings |
| 7 | **F2:** Logger timestamp — stack buffer, no String return | `Logger.h/cpp` + all callers | Medium | 🟠 Eliminates micro-fragmentation |
| 8 | **F3, F4, F5, F6:** Web server helpers — snprintf, no String | `CpapWebServer.cpp` | Small | 🟠 Eliminates transient Strings |
| 9 | **F10:** File path extraction — use `strrchr()` on C strings | `FileUploader.cpp` | Small | 🟡 Eliminates ~10 temporary Strings per scan |
| 10 | **F9:** SMB member Strings — store as const refs to Config | `SMBUploader.h/cpp` | Small | 🟠 Eliminates 5 duplicate Strings |

### Phase 3 — Vector Elimination (Medium Risk, Major Impact)

| # | Item | Files | Effort | Impact |
|:--|:-----|:------|:-------|:-------|
| 11 | **F12:** Pre-flight `hasFolderFiles()` — early return | `FileUploader.cpp` | Small | 🟠 Eliminates vector alloc during pre-flight |
| 12 | **F11:** Callback-based folder/file scanning | `FileUploader.cpp/h` | Medium–Large | 🔴 Eliminates 100s of String allocs per scan |

### Phase 4 — Static Object Promotion (Low Risk, Structural)

| # | Item | Files | Effort | Impact |
|:--|:-----|:------|:-------|:-------|
| 13 | **F13:** FileUploader sub-objects as direct members | `FileUploader.h/cpp` | Medium | 🟠 Eliminates 5 heap allocations |
| 14 | **F14:** Static FileUploader and CpapWebServer | `main.cpp`, headers | Medium | 🟠 Moves ~1 KB from heap to BSS |
| 15 | **F20:** Static logger buffer | `Logger.cpp` | Trivial | 🟡 Eliminates 1 malloc |

### Phase 5 — Rebootless Backend Cycling

| # | Item | Files | Effort | Impact |
|:--|:-----|:------|:-------|:-------|
| 16 | Add `MINIMIZE_REBOOTS` config key (alias: `SKIP_REBOOT_BETWEEN_BACKENDS`) | `Config.h/cpp` | Trivial | ✅ Done |
| 17 | Reboot reason recording (NVS unified mechanism) | `main.cpp` | Small | Improves diagnostics |
| 18 | Backend teardown logic in `handleReleasing()` | `main.cpp`, `FileUploader.cpp` | Medium | Enables rebootless cycling |
| 19 | Re-select backend without reboot (`FileUploader`) | `FileUploader.cpp` | Medium | Core feature |
| 20 | Heap health gate (skip reboot only if `max_alloc >= 50 KB`) | `main.cpp` | Small | Safety net |

**Phase 5 depends on Phases 1–3.** The rebootless path is only viable if heap fragmentation is sufficiently reduced by the earlier phases. The heap health gate (item 20) ensures the firmware falls back to rebooting if fragmentation is still too high.

---

## 5. Expected Outcome

### Before (Current)
```
Boot → SMB upload → REBOOT → CLOUD upload → REBOOT → SMB upload → ...
Total cycle time: ~90s boot + upload + ~90s boot + upload = high overhead
```

### After (With Phases 1–5)
```
Boot → SMB upload → teardown → COOLDOWN → CLOUD upload → teardown → COOLDOWN → ...
Total cycle time: upload + 10s cooldown + upload = minimal overhead
```

### Heap Profile Target

| Metric | Current (post-SMB) | Target (post-cleanup) |
|:-------|:-------------------|:----------------------|
| `getFreeHeap()` | ~120–140 KB | ~140–160 KB |
| `getMaxAllocHeap()` | ~35–50 KB (variable) | **~60–80 KB (stable)** |

The key metric is `getMaxAllocHeap()` stability. If it stays above 50 KB after a full SMB session teardown, the CLOUD TLS handshake will succeed without rebooting.

---

## 6. Testing Strategy

1. **Phase 1–3:** After each phase, run a full SMB+CLOUD upload cycle with `DEBUG=true` and compare `fh/ma` snapshots at key points (pre-upload, post-SMB, pre-CLOUD-TLS, post-CLOUD).
2. **Phase 5:** Enable `MINIMIZE_REBOOTS=true` and run 3+ consecutive upload cycles without reboot. Monitor `max_alloc` trend — it should remain stable (not monotonically decreasing).
3. **Regression:** Ensure `MINIMIZE_REBOOTS=false` (default) behaviour is unchanged (reboot after every real upload session).
4. **Edge case:** Verify the heap health gate triggers a reboot when `max_alloc` drops below threshold (can be tested by temporarily lowering the threshold).

---

## 7. Boot-Time Pre-Allocated Buffers (Zero-Fragmentation Strategy)

### 7.1 Core Principle

The **ideal** approach to eliminating heap fragmentation is to **allocate all major buffers once at boot and never free them**. If a buffer is allocated early (when heap is a single contiguous block) and kept alive, it cannot cause fragmentation — regardless of how many times it is reused.

This is fundamentally different from the current pattern of `new`/`delete` or `malloc`/`free` during runtime, which leaves holes in the heap map.

### 7.2 Candidate Buffers for Boot-Time Allocation

| Buffer | Current Allocation | Size | Lifetime | Boot-Time Feasible? |
|:-------|:-------------------|:-----|:---------|:--------------------|
| **WiFiClientSecure** (TLS wrapper) | `new` in `setupTLS()`, `delete` in `resetTLS()` | ~120 bytes (wrapper only) | Per-CLOUD-session | ✅ Yes — allocate once, call `stop()`/`connect()` to reuse |
| **mbedtls internal buffers** (TLS I/O, cert chain, session) | Allocated inside `WiFiClientSecure::connect()` by mbedtls | ~40–50 KB | Per-TLS-connection | ⚠️ Partially — see §7.3 |
| **SMB upload buffer** | `malloc()` in `allocateBuffer()` | 2–8 KB | Per-SMB-session | ✅ Yes — allocate once, reuse across sessions |
| **libsmb2 context** | `smb2_init_context()` → internal malloc | ~8–12 KB | Per-SMB-connection | ❌ No — internal to libsmb2, cannot pre-allocate |
| **Logger circular buffer** | `malloc()` in Logger constructor | 2 KB | Lifetime (already boot-time) | ✅ Already done (but could be static array) |
| **UploadStateManager** (×2) | `new` in FileUploader::begin() | ~17 KB each | Lifetime | ✅ Yes — make direct members |

### 7.3 mbedtls Static Buffer Mode

ESP-IDF supports `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`, which is the **opposite** of what we want — it *reduces* memory by allocating/freeing TLS buffers dynamically per connection.

The standard mbedtls behaviour (without `CONFIG_MBEDTLS_DYNAMIC_BUFFER`) allocates TLS I/O buffers when `mbedtls_ssl_setup()` is called (inside `WiFiClientSecure::connect()`) and frees them when the SSL context is destroyed. The buffers are:

- **Input buffer:** `MBEDTLS_SSL_IN_CONTENT_LEN` (default 16 KB on ESP32)
- **Output buffer:** `MBEDTLS_SSL_OUT_CONTENT_LEN` (default 4 KB on ESP32)
- **Handshake context:** ~8–12 KB (temporary, freed after handshake)
- **Certificate chain:** ~4–8 KB (depends on CA cert size)

**Problem:** Even if we keep the `WiFiClientSecure` wrapper alive, calling `stop()` destroys the internal mbedtls SSL context and frees those ~30–40 KB of buffers. The next `connect()` re-allocates them — potentially at a different address, causing fragmentation.

**Solution: Keep the SSL context alive across connections.** Instead of `stop()` + `connect()`, use:

1. `mbedtls_ssl_session_reset()` — resets the SSL state but **keeps the buffers allocated**
2. Re-establish TCP connection
3. `mbedtls_ssl_handshake()` — re-handshakes using the same buffers

This requires modifying `WiFiClientSecure` or wrapping it. The `WiFiClientSecure` class in ESP32 Arduino does not expose `mbedtls_ssl_session_reset()` directly, so we would need to either:

- **(a)** Subclass `WiFiClientSecure` and add a `reconnect()` method that resets without freeing
- **(b)** Use the lower-level `mbedtls_*` API directly (skip `WiFiClientSecure` entirely)
- **(c)** Accept the alloc/free cycle but mitigate by ensuring the TLS alloc always happens at the **same heap address** (possible if all other allocations are stable — which is the goal of this entire proposal)

**Recommendation:** Option **(c)** is the safest first step. If Phases 1–3 eliminate the other sources of fragmentation, the TLS alloc/free cycle will naturally return to the same heap region each time (because nothing else is moving around). Option **(a)** is the ultimate zero-fragmentation fix but requires deeper ESP-IDF integration work.

### 7.4 Proposed Boot-Time Allocation Order

Allocate in order of **decreasing size** during `setup()`, so the largest buffers occupy the low end of heap and smaller allocations fill in above them:

```
1. WiFiClientSecure wrapper         (~120 bytes)  — keep alive forever
2. UploadStateManager × 2           (~34 KB total) — direct members, not pointers
3. SMB upload buffer                (4–8 KB)       — allocate once via allocateBuffer()
4. Logger circular buffer           (2 KB → 8 KB)  — static array or boot-time malloc
5. FileUploader + sub-objects       (~2 KB)        — direct members
6. CpapWebServer + WebServer        (~1 KB)        — direct members
```

After boot, the heap layout is fixed. All runtime operations (TLS connect, SMB transfer, log writes) operate within these pre-allocated regions. The only remaining dynamic allocations are:

- mbedtls internal buffers (~40 KB) — allocated/freed per TLS connection but always at the top of heap
- libsmb2 internal buffers (~10 KB) — allocated/freed per SMB connection, also at heap top
- Transient `String` / `std::vector` — to be eliminated in Phases 2–3

---

## 8. Logging Architecture Overhaul

### 8.1 Current Logging State — Audit

| Aspect | Current Value | Source |
|:-------|:-------------|:-------|
| **Circular buffer size** | 2 KB (`LOG_BUFFER_SIZE = 2048`) | `Logger.h:22` |
| **NAND log file size** | 20 KB per file × 2 (A/B rotation) = **40 KB max** | `Logger.cpp:614` |
| **NAND flush interval** | **5 seconds** (`LOG_FLUSH_INTERVAL_MS = 5000`) | `main.cpp:102` |
| **Flush gate** | Only if `SAVE_LOGS = true` in config | `main.cpp:879` |
| **Web GUI poll interval** | **4 seconds** (`setInterval(fetchLogs, 4000)`) | `web_ui.h:572` |
| **Web GUI source** | Full circular buffer via `printLogs()` / `printLogsTail()` | `CpapWebServer.cpp:508–531` |
| **Client-side buffer** | 2000 lines max (`LOG_BUF_MAX = 2000`) | `web_ui.h:484` |
| **Emergency flush before reboot** | `dumpSavedLogsPeriodic()` called before `esp_restart()` at all 4 reboot points | `main.cpp:803,927,959,968` |
| **Emergency flush if SAVE_LOGS=false** | **Logs are lost** — `dumpSavedLogsPeriodic()` is a no-op if logging not enabled | `Logger.cpp:529` |
| **LittleFS partition** | 960 KB (`0xF0000`) — labeled `spiffs` in partition table | `partitions_ota.csv:6` |
| **Config key** | `SAVE_LOGS` (with `LOG_TO_SD_CARD` as legacy alias) | `Config.cpp:194` |
| **Log message bug** | Line 381 says "every 10 seconds" but actual interval is 5 seconds | `main.cpp:381` vs `main.cpp:102` |

### 8.2 Problems Identified

#### P1: Low NAND Log Capacity

Only **40 KB** of logs are retained (20 KB × 2 files with A/B rotation). At typical log rates (~50–100 bytes/line, ~2–5 lines/second during uploads), this captures only **~5–15 minutes** of upload activity. If a user presses "Download Logs" after a multi-hour upload session, most of the session's logs are already gone.

The LittleFS partition is **960 KB**. We are using less than 5% of it for logs. Even accounting for upload state files (a few KB), there is significant room to increase log retention.

#### P2: NAND Write Cycle Longevity

ESP32 uses **NOR flash** (not NAND — the partition table label is misleading). NOR flash is rated for **100,000 write cycles** per 256-byte page. LittleFS performs **wear leveling** — it spreads writes across pages so no single page is written disproportionately.

Current worst-case write rate:
- Flush every 5 seconds
- Each flush appends ~500 bytes (typical)
- LittleFS writes at page granularity (256 bytes) → each flush touches ~2 pages
- Writes per day: `86400 / 5 = 17,280` flushes
- But the device is not always flushing — only when `SAVE_LOGS=true` AND there are new logs

Realistic estimate (device active ~4 hours/day, logging ~2 hours):
- `2 × 3600 / 5 = 1,440` flushes/day × 2 pages = 2,880 page writes/day
- With wear leveling across 960 KB (3,840 pages): each page sees `2,880 / 3,840 ≈ 0.75` writes/day
- **Time to 100K cycles: `100,000 / 0.75 ≈ 133,000 days ≈ 365 years**

**Conclusion:** NAND longevity is **not a concern** at current or even 10× increased write rates. The bottleneck would be the ESP32 module's general lifespan (~10–20 years), not flash endurance.

However, there are still best practices to follow:
1. **Batch writes** — write larger chunks less often (reduces metadata overhead per write)
2. **Append-only** — never rewrite existing data (LittleFS handles this natively with COW)
3. **Avoid file truncation** — truncating a file rewrites metadata and free-space bitmaps
4. **Keep files open** — each `open()`/`close()` cycle touches metadata pages

#### P3: Normal Reboot Log Loss (LittleFS)

When `SAVE_LOGS=false` (the default), the call to `dumpSavedLogsPeriodic()` before `esp_restart()` is a no-op — the function returns immediately at line 529. **All circular buffer contents are lost.**

This means:
- Watchdog-triggered reboots lose all diagnostic context
- State-reset reboots lose the log trail
- User-initiated soft reboots lose recent context

Even when `SAVE_LOGS=true`, the flush only captures what's in the 2 KB circular buffer — if a high-rate burst of logs overflowed the buffer before the reboot, those logs are already gone.

These are reboots during **normal operation** — WiFi is up, and the Web UI will be accessible on the next boot. Therefore, persisting logs to **LittleFS** (internal flash) is the correct destination — the user can view them via the Web UI after the device reboots.

#### P6: Boot-Failure Emergency Log — SD Card as Last Resort

There is a fundamentally different failure class: **the device cannot establish WiFi at all** (missing/wrong SSID, bad password, hardware failure). In this case:

- The Web UI is **unreachable** — the user cannot browse to the device
- LittleFS logs are **inaccessible** — there is no network path to read them
- The **only storage the user can physically access** is the SD card (pull it out, read on PC)

The firmware already handles this with `Logger::dumpToSD()` → `/uploader_error.txt` on the SD card at two boot-failure points:

1. **Config load failure** (`main.cpp:354`) — config.txt missing, malformed, or unreadable
2. **WiFi connection failure** (`main.cpp:414`) — SSID not found, wrong password, DHCP timeout, hardware error

Both paths log the fatal error, dump the circular buffer to `/uploader_error.txt` on the SD card, release SD control back to the CPAP machine, and halt (`setup()` returns without entering the main loop).

**Current gaps in the SD emergency dump:**

1. **Overwrites previous dump** — `FILE_WRITE` mode truncates the file. If the device boot-loops (e.g., WiFi is persistently down), each reboot overwrites the previous dump. Only the last attempt survives.
2. **No timestamp or reboot reason** — the dump is raw circular buffer content with no header indicating when or why the dump occurred.
3. **Byte-by-byte writes** — `dumpToSD()` writes one byte at a time in a loop (`f.write(this->buffer[physicalPos])`) instead of using a chunk buffer, which is slow on SD card FAT filesystem.
4. **Not documented in specs** — the `dumpToSD()` mechanism and `/uploader_error.txt` convention are not mentioned in `docs/specs/logging-system.md` or any other spec file.
5. **No indication on next successful boot** — if the device eventually boots successfully (e.g., user fixes config), there is no log message telling the user that `/uploader_error.txt` exists on the SD card from a previous failed boot.

#### P4: Web GUI Shows Only Circular Buffer (2 KB)

The `/api/logs` endpoint streams the circular buffer contents. When the user opens the Logs tab, they see at most ~30–50 recent log lines (2 KB). Logs from before the tab was opened, or from before a reboot, are not available through this endpoint.

The `/api/logs/saved` endpoint downloads the NAND-saved files, but this is a separate download action, not integrated into the live view. There is a gap between "live circular buffer" and "saved NAND logs" that makes it hard to get a complete picture.

#### P5: Config Key Naming

`SAVE_LOGS` implies logs are being "saved" (i.e. backed up). The actual behaviour is **persistent logging to flash** — a continuous process, not a one-time save. `PERSISTENT_LOGS` better communicates the intent and the NAND write impact.

### 8.3 Proposed Architecture

#### 8.3.1 Increase Circular Buffer to 8 KB

Change `LOG_BUFFER_SIZE` from 2048 to **8192**. This provides ~100–200 lines of log history in RAM, enough to survive short bursts and provide useful context on the Web GUI without hitting NAND.

Cost: 6 KB additional DRAM. Acceptable — the ESP32 has ~300 KB total heap and this replaces transient String allocations that were using similar amounts.

**Important:** The buffer should be a **static array** (not malloc'd) to avoid heap fragmentation:
```cpp
static char logBuffer[LOG_BUFFER_SIZE];  // in Logger.cpp, BSS segment
```

#### 8.3.2 Two-Tier Emergency Log Strategy

There are two fundamentally different failure scenarios that require different log destinations:

##### Tier 1: Pre-Reboot Flush to LittleFS (Normal Operation)

During normal operation (WiFi is up, Web UI accessible), every `esp_restart()` should flush the circular buffer to a **dedicated file** on LittleFS, **regardless of `PERSISTENT_LOGS` setting**:

```
/last_reboot_log.txt  — overwritten on each reboot (not appended)
```

This ensures watchdog kills, state resets, soft reboots, and post-upload reboots always leave a diagnostic trail. The file is visible via the Web UI on the next boot.

- Size: up to `LOG_BUFFER_SIZE` (8 KB after §8.3.1) — negligible flash wear
- Written only at reboot time — not part of the periodic flush cycle
- On next boot, if `/last_reboot_log.txt` exists, log: `"Previous reboot log available on internal flash"`

Implementation: replace `dumpSavedLogsPeriodic(nullptr)` calls before `esp_restart()` with a new `dumpPreRebootLog()` method that writes to LittleFS **unconditionally** (bypasses `logSavingEnabled` check). Uses a 256-byte stack chunk buffer (same pattern as §8.3.5) — zero heap allocation.

##### Tier 2: Boot-Failure Dump to SD Card (No WiFi — Existing Mechanism)

When the device **cannot establish WiFi** (config failure, wrong credentials, hardware error), LittleFS is useless — the user has no network path to read it. The **SD card is the only accessible storage**.

This mechanism already exists via `Logger::dumpToSD()` → `/uploader_error.txt` on the SD card. It is called at the two boot-failure halt points in `setup()`. **This is the true "emergency log".**

**Proposed enhancements to the existing SD emergency dump:**

1. **Add a header with timestamp and reason:**
```cpp
// Before dumping circular buffer, write a header line:
f.printf("=== CPAP UPLOADER BOOT ERROR ===%s",
         "\nReason: %s\nUptime: %lu ms\nFirmware: %s\n\n",
         reason, millis(), FIRMWARE_VERSION);
```

2. **Append instead of overwrite** — use `FILE_APPEND` to preserve previous boot-failure dumps. Add a size cap (e.g., 64 KB) and truncate from the beginning if exceeded, so multiple boot-loop iterations are captured.

3. **Chunk-buffer writes** — replace the byte-by-byte loop with a 256-byte chunk buffer (matching §8.3.5 pattern) for better SD card write performance:
```cpp
char chunk[256];
size_t chunkPos = 0;
for (uint32_t i = 0; i < availableBytes; i++) {
    chunk[chunkPos++] = buffer[(tailIndex + i) % bufferSize];
    if (chunkPos == sizeof(chunk)) {
        f.write((const uint8_t*)chunk, chunkPos);
        chunkPos = 0;
    }
}
if (chunkPos > 0) f.write((const uint8_t*)chunk, chunkPos);
```

4. **Next-boot detection** — on a successful boot (config loads, WiFi connects), check if `/uploader_error.txt` exists on the SD card and log a warning:
```
[WARN] Previous boot failure log found on SD card: /uploader_error.txt
[WARN] Check the file for details about the prior failure.
```

5. **Update spec** — add the `dumpToSD()` → `/uploader_error.txt` convention to `docs/specs/logging-system.md`.

##### Summary of Two-Tier Strategy

| Scenario | Destination | File | Trigger | User Access |
|:---------|:-----------|:-----|:--------|:------------|
| Normal reboot (watchdog, soft-reboot, post-upload) | **LittleFS** | `/last_reboot_log.txt` | Before every `esp_restart()` | Web UI on next boot |
| Boot failure (config error, WiFi failure) | **SD Card** | `/uploader_error.txt` | `setup()` halt points | Pull SD card, read on PC |

#### 8.3.3 Increase NAND Log Retention

When `PERSISTENT_LOGS=true`:

- Increase file size limit from 20 KB to **64 KB** per file
- Keep A/B rotation (older file is B, newer is A)
- Total: **128 KB** of saved logs — enough for ~30–60 minutes of continuous upload logging
- This uses ~13% of the 960 KB LittleFS partition — well within budget

#### 8.3.4 Reduce Flush Frequency (NAND-Friendly)

Change flush interval from 5 seconds to **30 seconds** for normal operation. This reduces write cycles by 6× while having minimal impact on log freshness (the circular buffer still captures everything in real-time).

For the emergency dump (§8.3.2), the full circular buffer is dumped on every reboot, so no logs are lost even with the longer flush interval.

Add an **on-demand flush before download**: the `/api/logs/saved` handler already calls `dumpSavedLogsPeriodic()` before streaming — this ensures the download is current.

#### 8.3.5 Direct-to-File Flush (Eliminate F1 String)

Replace the current `dumpSavedLogsPeriodic()` implementation (which builds an intermediate `String`) with direct file writes from the circular buffer using a stack buffer:

```cpp
// Instead of:
String logContent;
logContent.reserve(bytesToDump);
for (...) logContent += buffer[physicalPos];
logFile.print(logContent);

// Use:
char chunk[256];
size_t chunkPos = 0;
for (uint32_t i = 0; i < bytesToDump; i++) {
    chunk[chunkPos++] = buffer[(lastDumpedBytes + i) % bufferSize];
    if (chunkPos == sizeof(chunk)) {
        logFile.write((const uint8_t*)chunk, chunkPos);
        chunkPos = 0;
    }
}
if (chunkPos > 0) logFile.write((const uint8_t*)chunk, chunkPos);
```

This eliminates the up-to-8 KB String allocation entirely. Zero heap impact.

#### 8.3.6 Rename `SAVE_LOGS` → `PERSISTENT_LOGS`

- Accept `PERSISTENT_LOGS` as the primary key
- Keep `SAVE_LOGS` and `LOG_TO_SD_CARD` as backward-compatible aliases
- Update all user-facing strings (web UI, docs, config reference)
- Fix the "every 10 seconds" log message to match actual interval

### 8.4 Web GUI Log Continuity — Options Analysis

The fundamental problem: the Web GUI only shows the circular buffer (last ~8 KB after §8.3.1). Logs from before the tab was opened, or from before a reboot, are invisible in the live view.

#### Option A: Polling NAND + Circular Buffer (Hybrid)

**How it works:**
1. When the Logs tab is first opened, the client fetches `/api/logs/full` which streams:
   - NAND saved logs (syslog.B.txt + syslog.A.txt) — historical
   - Circular buffer contents — recent
2. Subsequent polls fetch only `/api/logs` (circular buffer) for live updates
3. The client-side JS merges them, deduplicating overlapping lines

**Pros:**
- No new server-side infrastructure
- Uses existing HTTP polling pattern
- Zero additional heap allocation on ESP32 (NAND files streamed via existing `ChunkedPrint`)

**Cons:**
- Initial load could be slow (up to 128 KB of NAND logs)
- Deduplication is imperfect (timestamps may collide)
- Still misses logs between the last NAND flush and a reboot (gap = up to 30 seconds at proposed flush rate)

**Heap impact:** Zero — all streaming uses stack buffers.

#### Option B: Server-Sent Events (SSE)

**How it works:**
1. Client opens a persistent HTTP connection to `/api/logs/stream`
2. Server holds the connection open and pushes log lines as `text/event-stream` events
3. Each new log line is written to the SSE stream as well as the circular buffer

**Pros:**
- True real-time log streaming — no polling latency
- Standard browser API (`EventSource`) — simple client-side code
- Unidirectional (server→client) — simpler than WebSocket
- **Can be implemented with the existing sync `WebServer`** — just keep the client WiFiClient open and write to it periodically from the main loop

**Cons:**
- **Holds a TCP socket open permanently** — the sync `WebServer` library processes one request at a time on `handleClient()`. A long-lived SSE connection would **block all other web requests** unless we manage the SSE client separately (outside the WebServer request handler).
- Each connected SSE client consumes a TCP socket + lwIP buffers (~1.6 KB per pbuf × several pbufs). With the current pool of 32 pbufs, this is manageable for 1 client but not scalable.
- If the WiFi connection drops, the SSE stream breaks and must be re-established — losing any logs emitted during the gap.
- **Does NOT solve the "logs from before tab open" problem** — SSE only streams new events.

**Heap impact:** ~0 bytes additional heap (the WiFiClient is from the existing pool). But the persistent socket holds lwIP pbufs.

**Implementation sketch (sync WebServer SSE):**
```cpp
// In main loop, not in handleClient():
WiFiClient sseClient;  // stored globally, not on heap
bool sseActive = false;

// When /api/logs/stream is requested:
void handleSseStart() {
    sseClient = server->client();
    sseClient.print("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n");
    sseActive = true;
    // Do NOT call server->send() — we've taken over the socket
}

// In main loop (called every ~100ms):
void pushSseLogs() {
    if (!sseActive || !sseClient.connected()) {
        sseActive = false;
        return;
    }
    // Read new lines from circular buffer since last push
    // Write: "data: [log line]\n\n"
    char chunk[256];
    // ... read from circular buffer, format as SSE ...
    sseClient.write(chunk, len);
}
```

#### Option C: WebSocket

**How it works:**
1. Client opens a WebSocket connection to `ws://<ip>/ws/logs`
2. Server pushes log lines over the WebSocket frame protocol
3. Bidirectional — client could send commands back (e.g. "clear", "filter")

**Pros:**
- True real-time, low-latency
- Bidirectional — future-proof for interactive features
- Well-supported in browsers

**Cons:**
- **Requires a WebSocket library** — the sync `WebServer` does not support WebSocket. Would need `WebSocketsServer` (links2004) or switch to `ESPAsyncWebServer` + `AsyncWebSocket`. Both add code size and complexity.
- `ESPAsyncWebServer` is async (interrupt-driven) — **fundamentally different concurrency model** from our current sync main-loop design. Migrating is a major refactor.
- `WebSocketsServer` (links2004) can work alongside sync `WebServer` but **allocates per-client buffers on heap** (~1–2 KB per client). Each message frame involves heap allocation for the frame header.
- **Same "logs before tab open" gap** as SSE — WebSocket only streams new events.

**Heap impact:** 1–2 KB per connected client (WebSocketsServer internal buffers), plus frame allocation per message. **NOT zero-fragmentation.**

#### Option D: Hybrid — NAND Backfill + SSE Live Stream (Recommended)

**How it works:** Combine Option A (initial NAND backfill) with Option B (SSE live stream):

1. **On tab open:** Client fetches `/api/logs/full` — streams NAND logs + circular buffer (historical context)
2. **After initial load:** Client opens SSE connection to `/api/logs/stream` for live push updates
3. **Client-side JS** merges the initial backfill with the live stream, deduplicating by timestamp + content
4. **On SSE disconnect** (WiFi drop, reboot): Client falls back to polling `/api/logs` until SSE reconnects. `EventSource` has built-in auto-reconnect.

**Server-side implementation:**
- `/api/logs/full` — new endpoint, streams syslog.B + syslog.A + circular buffer (same as handleApiLogsSaved but inline in web view, not download)
- `/api/logs/stream` — SSE endpoint, managed outside `handleClient()` (see sketch in Option B)
- SSE push happens in the main loop, reading from the circular buffer using a dedicated `lastPushedIndex` tracker (similar to `lastDumpedBytes` for NAND flush)

**Why this is best:**
- **Full log history** on first open (NAND backfill solves "logs before tab open")
- **Real-time updates** without 4-second polling gaps (SSE push)
- **Zero heap fragmentation** — SSE uses the existing WiFiClient socket, NAND streaming uses stack buffers
- **Graceful degradation** — if SSE fails, client falls back to polling
- **No library changes** — works with existing sync `WebServer`
- **Single SSE client limit** is acceptable (only one browser tab typically views logs)

**Caveat:** The sync `WebServer` processes requests sequentially in `handleClient()`. While an SSE stream is active, we must not block on it — the SSE write happens in the main loop outside `handleClient()`, so other HTTP requests are still processed normally. The SSE client socket is managed independently.

### 8.5 Heap Impact Summary for Logging Changes

| Change | Heap Impact | Fragmentation Impact |
|:-------|:-----------|:--------------------|
| Circular buffer 2 KB → 8 KB (static array) | +6 KB **BSS** (not heap) | **Negative** (removes 2 KB malloc) |
| Direct-to-file flush (eliminate String) | -2–8 KB transient | **Eliminates** F1 fragmentation |
| Pre-reboot flush to LittleFS (Tier 1) | 0 (stack buffer writes) | Zero |
| SD card emergency dump enhancements (Tier 2) | 0 (stack buffer writes) | Zero |
| NAND file size 20→64 KB | 0 (file system, not heap) | Zero |
| SSE client socket | 0 (from existing socket pool) | Zero |
| NAND backfill streaming | 0 (256-byte stack buffer) | Zero |

**Net heap change:** Reduces heap usage by ~2 KB (malloc → static) and eliminates up to 8 KB of transient String fragmentation.

### 8.6 Implementation Plan for Logging

| # | Item | Files | Effort | Priority | Status |
|:--|:-----|:------|:-------|:---------|:-------|
| L1 | Increase `LOG_BUFFER_SIZE` to 8192, make buffer a static array | `Logger.h`, `Logger.cpp` | Trivial | High | ✅ Done |
| L2 | Direct-to-file flush — eliminate intermediate String (F1) | `Logger.cpp` | Small | High | ✅ Done |
| L3a | Add `dumpPreRebootLog()` — always flush to LittleFS before `esp_restart()` | `Logger.h/cpp`, `main.cpp` | Small | High | ✅ Done |
| L3b | Enhance `dumpToSD()` — header with reason/timestamp, append mode, chunk writes, next-boot detection | `Logger.h/cpp`, `main.cpp` | Small | High | ✅ Done |
| L4 | Increase NAND log file size to 64 KB | `Logger.cpp` | Trivial | Medium | ✅ Done |
| L5 | Reduce flush interval to 30 seconds | `main.cpp` | Trivial | Medium | ✅ Done |
| L6 | Rename `SAVE_LOGS` → `PERSISTENT_LOGS` (keep aliases) | `Config.cpp`, `CpapWebServer.cpp`, docs | Small | Medium | ✅ Done |
| L7 | Fix "every 10 seconds" log message | `main.cpp:381` | Trivial | Low | ✅ Done |
| L8 | Add `/api/logs/full` endpoint (NAND + circular buffer backfill) | `CpapWebServer.cpp` | Medium | Medium | ✅ Done |
| L9 | Add SSE `/api/logs/stream` endpoint + main-loop push | `CpapWebServer.cpp/h`, `main.cpp` | Medium | Medium | ✅ Done |
| L10 | Update Web GUI JS for hybrid backfill + SSE live stream | `web_ui.h` | Medium | Medium | ✅ Done |

**All items implemented.** Documentation updated in: `docs/specs/logging-system.md`, `docs/CONFIG_REFERENCE.md`, `docs/DEVELOPMENT.md`, `docs/03-REQUIREMENTS.md`, `docs/specs/configuration-management.md`, `README.md`.
