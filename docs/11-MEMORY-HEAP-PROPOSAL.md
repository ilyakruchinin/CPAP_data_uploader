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
SKIP_REBOOT_BETWEEN_BACKENDS = false   # default: false (current behaviour)
```

When `true`, after an upload session completes, the FSM checks heap health before deciding whether to reboot or continue to COOLDOWN → next backend.

### 2.2 Decision Logic (in `handleReleasing()`)

```
IF g_nothingToUpload → COOLDOWN (no reboot, existing behaviour)
IF NOT skipRebootBetweenBackends → reboot (current behaviour, default)
IF skipRebootBetweenBackends:
    free_heap = ESP.getFreeHeap()
    max_alloc = ESP.getMaxAllocHeap()
    IF max_alloc >= HEAP_HEALTHY_THRESHOLD (e.g. 50000):
        LOG reason: "Heap healthy — skipping reboot"
        → COOLDOWN (next cycle picks next backend)
    ELSE:
        LOG reason: "Heap degraded — rebooting to restore contiguous memory"
        → reboot (with recorded reason)
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
| 16 | Add `SKIP_REBOOT_BETWEEN_BACKENDS` config key | `Config.h/cpp` | Trivial | — |
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
2. **Phase 5:** Enable `SKIP_REBOOT_BETWEEN_BACKENDS=true` and run 3+ consecutive backend cycles without reboot. Monitor `max_alloc` trend — it should remain stable (not monotonically decreasing).
3. **Regression:** Ensure `SKIP_REBOOT_BETWEEN_BACKENDS=false` (default) behaviour is unchanged.
4. **Edge case:** Verify the heap health gate triggers a reboot when `max_alloc` drops below threshold (can be tested by temporarily lowering the threshold).
