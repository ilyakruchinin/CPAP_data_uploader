# Architectural Design Update (v0.10.x)

## 1. Rationale: Why Are We Changing It?
The primary goal of this project has always been to transparently upload CPAP data without interfering with the machine's core function: sleep therapy. However, the current architecture holds the SD card "hostage" (maintaining the multiplexer switch toward the ESP32) for the *entire duration* of an upload session, which can take minutes. 

Furthermore, the ESP32 mounts the physical SD card in **Read/Write** mode. It writes state files (`.upload_state.*`), configuration updates, and debugging logs directly to the SD card. 

When the CPAP machine (which caches the FAT32 table in its own RAM) attempts to write data while the ESP32 is holding the card, or discovers that the FAT table has been modified beneath it without its knowledge, it triggers an unrecoverable **"SD Card Error"**, forcing the user to physically remove and format the card. 

## 2. Goals: What Are We Trying to Achieve?
We are transitioning the firmware from a "monolithic hostage" model to a "time-sliced, non-destructive" model. The core objectives are:
1.  **Zero FAT Corruption:** Guarantee the ESP32 will never modify the FAT table behind the CPAP's back.
2.  **Zero Forced Takeovers:** The ESP32 must never force a hostile takeover of the bus if the CPAP is actively writing.
3.  **Minimize Hostage Time:** Reduce the continuous duration the ESP32 holds the SD card from *minutes* to *seconds*.
4.  **Graceful Bus Handoff:** Ensure the physical SD NAND flash chip is in a clean, predictable state before handing the electrical traces back to the CPAP.
5.  **Data Integrity:** Prevent data loss if the CPAP appends data to a file while the ESP32 is simultaneously uploading an older chunk of that same file over the network.

## 3. Architecture: How Will We Achieve It?

### 3.1. Read-Only SD Mounting & Internal State Migration
The ESP32 will mount the CPAP's FAT32 volume strictly as **Read-Only**.
*   **State & Logs:** The `UploadStateManager` JSON tracking files and all `Logger` output will be migrated entirely to the ESP32's internal 960KB `SPIFFS` partition.
*   **Log Rotation (A/B Ping-Pong):** To prevent logs from filling SPIFFS without causing flash-wearing file rewrite overhead, the `Logger` will use two bounded files (`syslog.A.txt` and `syslog.B.txt`). It writes to A until a limit (e.g., 20KB) is reached, then truncates and writes to B. When serving logs to the UI, it concatenates the inactive and active files for a seamless history.
*   **Config Editor (Remount Strategy):** The SD card remains Read-Only 99% of the time. When a user clicks "Save" in the web UI, the ESP32 will briefly remount the SD card as Read/Write, write the new `config.txt`, immediately remount as Read-Only, and display a critical warning requiring the user to physically reinsert the card into the CPAP to flush the CPAP's FAT cache.

### 3.2. SD State Machine Reset (`CMD0` Injection)
When `SD_MMC.end()` completes an upload, it disables the ESP32's SD peripheral but does *not* send the `CMD0` (GO_IDLE_STATE) command to the physical SD card. The multiplexer flips back to the CPAP while the SD card is still in the `Transfer` state listening for the ESP32's Relative Card Address (RCA). 
*   **Fix:** Implement manual bit-banging of the `CMD0` frame (`0x40 0x00 0x00 0x00 0x00 0x95`) on the `SD_CMD_PIN` immediately prior to flipping the multiplexer switch (`SDCardManager::releaseControl()`). This forces a software reset of the NAND chip.

### 3.3. Hybrid Buffering (Time-Slicing) & The Greedy Buffer
To dramatically shrink the hostage window without altering the rigid 960KB SPIFFS partition size limit, we will implement **Hybrid Buffering** using a "Greedy Buffer" strategy.

*   **Pre-calculation:** When the ESP32 takes the card, it scans pending files and reads their sizes (a microsecond metadata read).
*   **Greedy Batching:** It sequentially adds files to a batch until the total batch size nears a safe threshold (e.g., 800KB, leaving 160KB for SPIFFS overhead). 
*   **Copy & Release:** It copies the batch of small files into a SPIFFS `/buffer/` directory, immediately releases the SD card back to the CPAP, and uploads the files to the network at its leisure.
*   **Direct Streaming Fallback:** If a single file (like `BRP.edf`) is larger than 800KB, it bypasses the buffer. The ESP32 locks the SD card, streams the file directly to the network, and releases the card.
*   **Pre-Session Purge:** To prevent orphaned files from filling SPIFFS due to power loss or crashes, the system will execute a strict purge of the `/buffer/` directory on boot and at the exact start of every FSM `UPLOADING` state.

### 3.4. Point-in-Time State Snapshot
Because the SD card is released to the CPAP *during* the network upload of buffered files, a race condition exists: the CPAP could append data to a file on the SD card while the ESP32 is uploading its buffered copy.
*   **Fix:** During the SD lock (when copying to SPIFFS), capture the exact `size` and `modified_time` of each file into a RAM `PendingUploadState` struct.
*   Upon successful network upload, the permanent `UploadStateManager` JSON is updated using these *frozen* RAM values, not by re-querying the physical SD card. 
*   On the next cycle, the FSM will see the physical SD card file is larger than the recorded state snapshot, and correctly trigger a delta upload.

### 3.5. SMART_WAIT Cleanup
*   **Remove `SMART_WAIT_MAX_MS`:** The highly dangerous 45-second timeout, which previously forced a hostile takeover of the bus even if the CPAP was actively writing, will be permanently deleted.
*   **Configurable `SMART_WAIT_SECONDS`:** The 5-second silence requirement (used at boot, and now between every single batch grab in Hybrid Buffering) will be exposed to `config.txt`.
*   **`INACTIVITY_SECONDS`:** Remains the initial trigger (default 62s) to transition the FSM from `LISTENING` to `ACQUIRING`.

### 3.6. UI Cleanup & CPAP Profiler Wizard
*   Remove the legacy 24-hour graph and `CPAPMonitor` code, which generates confusing pseudo-errors (`[INFO] CPAP monitor disabled (CS_SENSE hardware issue)`).
*   Introduce a **CPAP Profiler Wizard** in the Web UI to empirically measure a specific CPAP machine's active writing frequency and post-therapy cooldown hold times, allowing users to accurately tune their `UPLOAD_WINDOW` and `SMART_WAIT_SECONDS`.

---

## 4. Implementation Phases

### Phase 1: Storage Migration & R/O Mount
1.  Update `SDCardManager` to mount `SD_MMC` strictly as Read-Only.
2.  Refactor `Logger` to write `syslog.txt` exclusively to SPIFFS.
3.  Refactor `UploadStateManager` to save `.upload_state.v2.*` JSON files exclusively to SPIFFS.
4.  Implement the "Remount Strategy" in `CpapWebServer::handleApiConfigRawPost()`.

### Phase 2: CMD0 Injection & SMART_WAIT Safety
1.  Implement `SDCardManager::sendCMD0()` using standard Arduino `digitalWrite` bit-banging on `SD_CMD_PIN`, clocking `SD_CLK_PIN`.
2.  Call `sendCMD0()` immediately before pulling the multiplexer pin low in `releaseControl()`.
3.  Rip `SMART_WAIT_MAX_MS` logic out of `main.cpp`. If silence isn't achieved, wait indefinitely.
4.  Expose `SMART_WAIT_SECONDS` to `Config.h` and the Web UI.

### Phase 3: Hybrid Buffering Engine
1.  Create `BufferManager` class to handle SPIFFS `/buffer/` lifecycle (including the pre-session purge).
2.  Modify `FileUploader::uploadDatalogFolderSmb()` and `uploadDatalogFolderCloud()` to utilize the Greedy Buffer.
3.  Implement the `PendingUploadState` struct (`BufferedFile`: `sourcePath`, `bufferPath`, `size`).
4.  Update the logic loop:
    *   Lock -> Scan sizes -> Copy to SPIFFS -> Snapshot Size in RAM (`BufferedFile.size`) -> Release
    *   Upload from SPIFFS -> Delete from SPIFFS -> `markFileUploaded()` using frozen size -> **`stateManager->save()` (per-batch journal flush)**
    *   `Smart Wait` -> Re-acquire -> Repeat until folder is complete.
5.  Implement the >800KB direct stream fallback.
6.  **Per-batch state durability:** `markFileUploaded()` queues a `JournalEvent` in RAM only (`queueEvent()`). It does **not** write to SPIFFS until `save()` → `flushJournal()` is explicitly called. `flushJournal()` is a lightweight append-only SPIFFS write (~25 bytes per file entry). Calling `save()` once per completed batch (not per file, not per folder) gives per-batch crash recovery with minimal SPIFFS wear: if power fails mid-session, at most one batch of already-uploaded files is re-uploaded on the next session.

### Phase 4: Web UI Diagnostics
1.  Remove `CPAPMonitor.cpp/h` and associated frontend code.
2.  Build the CPAP Profiler Wizard REST API endpoints (`/api/profiler/start`, `/api/profiler/status`).
3.  Build the frontend Wizard UI.

---

## 5. Architectural Flow Diagrams

### Legacy Architecture: "The Monolithic Hostage"

```text
Time (Minutes)
|-- 0:00 -- LISTENING: 62s of silence detected
|
|-- 1:02 -- ESP32 LOCKS SD CARD MUX (R/W Mount)
|             |-- Upload File 1 (Network Latency)
|             |-- Write State to SD (FAT Cache Risk!)
|             |-- Upload File 2 (Network Latency)
|             |-- Write State to SD
|             |-- Upload File 3 (Network Latency)
|             |-- Write State to SD
|           (CPAP wakes up and tries to write -> SD CARD ERROR)
|             |-- Upload File N (Network Latency)
|             |-- Write Final State to SD
|-- 5:30 -- ESP32 RELEASES SD CARD MUX
```

### New Architecture: "Hybrid Buffering (Time-Sliced)"

```text
Time (Seconds)
|-- 0:00 -- LISTENING: 62s of silence detected
|
|-- 1:02 -- ESP32 LOCKS SD CARD MUX (R/O Mount)
|             |-- Scan File Sizes
|             |-- Copy File 1, 2, 3 to SPIFFS (Fast local I/O)
|             |-- Snapshot Size/Time in RAM
|-- 1:04 -- ESP32 RELEASES SD CARD MUX
|             |
|             |-- (CPAP is free to write to SD Card safely)
|             |
|             |-- Upload File 1 from SPIFFS to Cloud (Network Latency)
|             |-- Upload File 2 from SPIFFS to Cloud
|             |-- Upload File 3 from SPIFFS to Cloud
|             |-- Delete SPIFFS Buffer
|             |-- Commit RAM Snapshot to SPIFFS State JSON
|
|-- 1:45 -- SMART WAIT: Checking for 5s of silence...
|-- 1:50 -- ESP32 LOCKS SD CARD MUX (R/O Mount)
|             |-- Scan File Sizes
|             |-- Copy File 4, 5 to SPIFFS
|-- 1:51 -- ESP32 RELEASES SD CARD MUX
|             |-- Upload File 4, 5...
```

### Point-in-Time State Snapshot (Race Condition Prevention)

```text
[ SD Card ]                     [ ESP32 RAM ]                   [ Cloud ]
File A (Size: 10KB)  ------->   Buffer File A
                                Snapshot: A = 10KB
                                
[ SD CARD RELEASED ]

(CPAP wakes up)
Appends 5KB to A     
File A (Size: 15KB)             Upload File A (10KB)  ------->  Cloud gets 10KB

                                Update State JSON:
                                A is marked done at 10KB
                                
[ NEXT CYCLE ]
Scan File A (15KB)
Compare to State (10KB)
Delta = 5KB!
Copy 5KB to SPIFFS...
```
