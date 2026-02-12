# Implementation Requirements

## 1. Configuration Parameters

### 1.1 New Parameters

| Parameter | Type | Default | Description |
|---|---|---|---|
| `UPLOAD_MODE` | string | `"scheduled"` | `"scheduled"` or `"smart"` |
| `UPLOAD_START_HOUR` | int (0-23) | `8` | Start of allowed upload window |
| `UPLOAD_END_HOUR` | int (0-23) | `22` | End of allowed upload window |
| `INACTIVITY_SECONDS` | int | `300` | Bus silence required before upload (Z) |
| `EXCLUSIVE_ACCESS_MINUTES` | int | `5` | Max time ESP holds SD card exclusively (X) |
| `COOLDOWN_MINUTES` | int | `10` | Time card is released between sessions (Y) |

### 1.2 Retained Parameters (unchanged)

| Parameter | Type | Default | Role in new architecture |
|---|---|---|---|
| `RECENT_FOLDER_DAYS` | int | `2` | Defines fresh vs old data boundary (B) |
| `MAX_DAYS` | int | `0` (all) | Hard cutoff — older folders ignored entirely |
| `MAX_RETRY_ATTEMPTS` | int | `3` | Retries per folder on upload failure |
| `BOOT_DELAY_SECONDS` | int | `30` | Initial delay before first SD access |
| `GMT_OFFSET_HOURS` | int | `0` | Timezone for schedule calculations |
| `LOG_TO_SD_CARD` | bool | `false` | Debug logging to SD |
| All WiFi/endpoint/cloud params | — | — | No changes |

### 1.3 Deprecated Parameters

These are no longer needed and should be **ignored with a warning log** if present:

| Parameter | Reason | Migration |
|---|---|---|
| `UPLOAD_HOUR` | Replaced by `UPLOAD_START_HOUR` + `UPLOAD_END_HOUR` | If present and new params absent: `START = UPLOAD_HOUR`, `END = UPLOAD_HOUR + 2` |
| `SESSION_DURATION_SECONDS` | Replaced by `EXCLUSIVE_ACCESS_MINUTES` | If present and new param absent: `EXCLUSIVE_ACCESS_MINUTES = SESSION_DURATION_SECONDS / 60` (min 1) |
| `SD_RELEASE_INTERVAL_SECONDS` | No periodic releases in new architecture | Ignored |
| `SD_RELEASE_WAIT_MS` | No periodic releases in new architecture | Ignored |
| `UPLOAD_INTERVAL_MINUTES` | Replaced by `UPLOAD_MODE` | If > 0 and UPLOAD_MODE absent: set `UPLOAD_MODE = "smart"` |

### 1.4 Example config.json

```json
{
  "WIFI_SSID": "MyNetwork",
  "WIFI_PASSWORD": "********",
  "ENDPOINT": "smb://nas/cpap_data",
  "ENDPOINT_TYPE": "SMB",
  "ENDPOINT_USER": "cpap",
  "ENDPOINT_PASSWORD": "********",

  "UPLOAD_MODE": "smart",
  "UPLOAD_START_HOUR": 8,
  "UPLOAD_END_HOUR": 22,
  "INACTIVITY_SECONDS": 300,
  "EXCLUSIVE_ACCESS_MINUTES": 5,
  "COOLDOWN_MINUTES": 10,
  "RECENT_FOLDER_DAYS": 2,
  "MAX_DAYS": 30,

  "GMT_OFFSET_HOURS": 11,
  "BOOT_DELAY_SECONDS": 30,
  "MAX_RETRY_ATTEMPTS": 3
}
```

---

## 2. Validation Rules

| Parameter | Validation | On failure |
|---|---|---|
| `UPLOAD_MODE` | Must be `"scheduled"` or `"smart"` | Default to `"scheduled"`, warn |
| `UPLOAD_START_HOUR` | 0–23 | Default to 8, warn |
| `UPLOAD_END_HOUR` | 0–23 | Default to 22, warn |
| `INACTIVITY_SECONDS` | 10–3600 | Clamp to range, warn |
| `EXCLUSIVE_ACCESS_MINUTES` | 1–30 | Clamp to range, warn |
| `COOLDOWN_MINUTES` | 1–60 | Clamp to range, warn |
| `RECENT_FOLDER_DAYS` | 0–30 | Existing validation (keep) |

---

## 3. Functional Requirements

### 3.1 TrafficMonitor (New Component)

**Purpose**: Detect SD bus activity on GPIO 33 using ESP32 PCNT peripheral.

| Requirement | Description |
|---|---|
| **R-TM-01** | Initialize PCNT unit 0, channel 0 on GPIO 33 |
| **R-TM-02** | Count both rising and falling edges |
| **R-TM-03** | Hardware glitch filter: ignore pulses < ~100ns (filter value = 10) |
| **R-TM-04** | `isBusy(windowMs)`: clear counter, wait windowMs, return count > 0 |
| **R-TM-05** | `getConsecutiveIdleMs()`: track consecutive idle windows, return cumulative duration |
| **R-TM-06** | `resetIdleTracking()`: reset consecutive idle counter (on any activity detection) |
| **R-TM-07** | Non-blocking `update()` method for use in main loop (sample every ~100ms) |
| **R-TM-08** | GPIO 33 configured as INPUT_PULLUP before PCNT init |
| **R-TM-09** | Rolling sample buffer: store last N samples (e.g., 300 = 5 min at 1/sec) with pulse count + timestamp |
| **R-TM-10** | `getSampleBuffer()`: return recent samples for web UI (SD Activity Monitor) |
| **R-TM-11** | Track longest idle streak and total active/idle sample counts for statistics |

### 3.2 Upload FSM (Main Loop Rewrite)

| Requirement | Description |
|---|---|
| **R-FSM-01** | Implement 8 states: IDLE, LISTENING, ACQUIRING, UPLOADING, RELEASING, COOLDOWN, COMPLETE, MONITORING |
| **R-FSM-02** | IDLE → LISTENING transition based on mode + time + data availability |
| **R-FSM-03** | LISTENING: non-blocking, uses TrafficMonitor.update() every loop iteration |
| **R-FSM-04** | LISTENING → ACQUIRING: only after Z consecutive seconds of silence |
| **R-FSM-05** | ACQUIRING: take SD control, mount, handle failure → RELEASING |
| **R-FSM-06** | UPLOADING: exclusive access, no periodic SD releases |
| **R-FSM-07** | UPLOADING timer: track elapsed time, check after each file completes |
| **R-FSM-08** | UPLOADING → RELEASING: when X minutes expired AND current file done |
| **R-FSM-09** | UPLOADING → COMPLETE: when all eligible files uploaded |
| **R-FSM-10** | COOLDOWN: non-blocking Y-minute wait, web server remains responsive |
| **R-FSM-11** | COOLDOWN → LISTENING: if still eligible (time window, data remaining) |
| **R-FSM-12** | COOLDOWN → IDLE: if no longer eligible (window closed, no data) |
| **R-FSM-13** | COMPLETE (smart mode): after cooldown + Z inactivity, re-scan fresh data |
| **R-FSM-14** | COMPLETE (scheduled mode): mark done, go to IDLE |
| **R-FSM-15** | Web "Upload Now": force transition to ACQUIRING (skip inactivity check) |
| **R-FSM-16** | All states: handle web server requests (non-blocking) |
| **R-FSM-17** | MONITORING: entered via web button, all uploads stopped, TrafficMonitor continues sampling |
| **R-FSM-18** | MONITORING → IDLE: when user clicks "Stop Monitor" in web UI |
| **R-FSM-19** | Entering MONITORING from UPLOADING: finish current file, upload root/SETTINGS, release SD, then enter MONITORING |

### 3.3 Scheduling Logic (ScheduleManager Rewrite)

| Requirement | Description |
|---|---|
| **R-SCH-01** | `isInUploadWindow()`: check if current hour is between START and END |
| **R-SCH-02** | Support cross-midnight windows (START > END wraps around) |
| **R-SCH-03** | `canUploadFreshData()`: smart mode → always true; scheduled mode → `isInUploadWindow()` |
| **R-SCH-04** | `canUploadOldData()`: both modes → `isInUploadWindow()` |
| **R-SCH-05** | `isUploadEligible(hasFreshData, hasOldData)`: combines mode + window + data checks |
| **R-SCH-06** | Daily completion tracking: in scheduled mode, mark day as done after full upload |

### 3.4 FileUploader Modifications

| Requirement | Description |
|---|---|
| **R-FU-01** | Remove `checkAndReleaseSD()` method entirely |
| **R-FU-02** | Remove `lastSdReleaseTime` tracking |
| **R-FU-03** | Add `uploadWithExclusiveAccess(sdManager, maxMinutes, dataFilter)` method |
| **R-FU-04** | `dataFilter` enum: `FRESH_ONLY`, `ALL_DATA`, `OLD_ONLY` |
| **R-FU-05** | Timer check after each DATALOG file: if X minutes exceeded, exit DATALOG phase (not error) |
| **R-FU-06** | Return status: `COMPLETE`, `TIMEOUT`, `ERROR` (not just bool) |
| **R-FU-07** | Retain all existing: folder scanning, file checksums, retry logic, multi-backend upload |
| **R-FU-08** | Phase 2 (old data): only execute when `canUploadOldData()` returns true |
| **R-FU-09** | **Root/SETTINGS files are MANDATORY**: always uploaded after DATALOG phases, X timer does NOT skip them |
| **R-FU-10** | An upload without root/SETTINGS files is not valid — these files finalize the import |

### 3.5 SDCardManager Modifications

| Requirement | Description |
|---|---|
| **R-SD-01** | Remove `digitalRead(CS_SENSE)` check from `takeControl()` |
| **R-SD-02** | Activity detection is now handled externally by TrafficMonitor + FSM |
| **R-SD-03** | `takeControl()` simply switches mux and initializes SD |
| **R-SD-04** | `releaseControl()` unchanged |

### 3.6 Pin Configuration

| Requirement | Description |
|---|---|
| **R-PIN-01** | Change `CS_SENSE` from GPIO 32 to GPIO 33 in `pins_config.h` |
| **R-PIN-02** | Update the TODO comment to reflect verified status |

---

## 4. Non-Functional Requirements

| Requirement | Description |
|---|---|
| **R-NF-01** | Main loop must remain non-blocking (no long delays, use millis() tracking) |
| **R-NF-02** | Web server must remain responsive in all states (including COOLDOWN) |
| **R-NF-03** | PCNT sampling must not interfere with WiFi or upload operations |
| **R-NF-04** | Memory usage: TrafficMonitor adds negligible RAM (PCNT is hardware) |
| **R-NF-05** | Backward compatible: old config.json files work with deprecation warnings |
| **R-NF-06** | State file (`.upload_state.json`) format unchanged — no migration needed |
| **R-NF-07** | All build flags (ENABLE_SMB_UPLOAD, ENABLE_SLEEPHQ_UPLOAD, etc.) unchanged |
| **R-NF-08** | OTA update functionality unchanged |

---

## 5. Testing Requirements

| Test | Description |
|---|---|
| **T-01** | TrafficMonitor: verify PCNT counts edges on GPIO 33 (manual test with signal generator or CPAP) |
| **T-02** | FSM transitions: verify all state transitions with logging |
| **T-03** | Scheduled mode: verify uploads only occur within window |
| **T-04** | Smart mode: verify fresh data uploads outside window, old data blocked |
| **T-05** | Inactivity detection: verify Z-second threshold works correctly |
| **T-06** | Exclusive access timer: verify X-minute limit respected, current file finishes |
| **T-07** | Cooldown: verify Y-minute wait before re-listening |
| **T-08** | Smart re-scan: verify new fresh files detected after COMPLETE |
| **T-09** | Backward compat: verify old config.json loads with warnings |
| **T-10** | Web trigger: verify forced upload bypasses inactivity check |
| **T-11** | Cross-midnight window: verify START > END wraps correctly |
| **T-12** | Long-running stability: verify no memory leaks or watchdog timeouts over 24h+ |
| **T-13** | SD Activity Monitor: verify live PCNT data displayed in web UI, uploads stopped |
| **T-14** | SD Activity Monitor: verify entering from UPLOADING state finishes current file + root/SETTINGS first |
| **T-15** | Root/SETTINGS mandatory: verify root/SETTINGS always uploaded even when X timer expired during DATALOG |
