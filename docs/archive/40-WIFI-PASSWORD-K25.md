# WiFi Password "Blank" Bug Analysis

## Executive Summary

**Issue Identified:** Devices fail to connect to WiFi with "WiFi password is empty" warning, even though users confirm they configured a password. The logs show a 4-way handshake timeout (reason 15), indicating the access point rejected the connection attempt.

**Root Cause:** A silent failure in the credential loading path. When `config.txt` contains the censored placeholder `***STORED_IN_FLASH***`, the code attempts to load the actual password from ESP32 Preferences (NVS storage). If the Preferences key is missing or NVS is corrupted, `loadCredential()` silently returns an empty string default, which then flows to WiFiManager and causes the open-network connection attempt.

**Severity:** High — renders device non-functional after certain reset conditions; users cannot recover without understanding the underlying issue.

---

## Bug Report Context (Issue #42)

### User Symptoms
- Device shows "WiFi password is empty - attempting open network connection" in logs
- Users confirm password IS set in config (and not blank)
- Some users report the device "spontaneously" started working again
- Affects users with power delivery issues (possibly related to brownout recovery)

### Log Analysis

```
[--:--:--] [INFO] Loaded WiFi password from flash
[--:--:--] [WARN] WiFi password is empty - attempting open network connection
[--:--:--] [INFO] Connecting to WiFi: SKY656A8
[--:--:--] [WARN] WiFi Event: Disconnected from AP (reason: 15)
[--:--:--] [WARN] Disconnect reason: 4-way handshake timeout
```

**Key Observation:** The log says "Loaded WiFi password from flash" immediately before "WiFi password is empty". This is contradictory — if it was truly loaded, it wouldn't be empty.

---

## Code Flow Analysis

### Step 1: Config File Parsing (`Config::loadFromSD()`)

```cpp
// Config.cpp:409-412 - Parse config.txt
while (configFile.available()) {
    String line = configFile.readStringUntil('\n');
    parseLine(line);
}
```

The `parseLine()` function (line 179) stores the value from config.txt into `wifiPassword`. If the user previously migrated to secure storage, this value is `***STORED_IN_FLASH***`.

### Step 2: Censorship Check and Preferences Load

```cpp
// Config.cpp:437-444
bool wifiCensored = isCensored(wifiPassword);  // Returns TRUE for ***STORED_IN_FLASH***

if (wifiCensored) {
    wifiPassword = loadCredential(PREFS_KEY_WIFI_PASS, "");  // BUG: Returns "" if key missing
    LOG("Loaded WiFi password from flash");  // Always logs, even if empty!
}
```

**THE BUG:** The log statement at line 444 is unconditional. Even when `loadCredential()` returns the empty default (because the key doesn't exist in NVS), the code still logs "Loaded WiFi password from flash".

### Step 3: `loadCredential()` Silent Failure

```cpp
// Config.cpp:93-110
String Config::loadCredential(const char* key, const String& defaultValue) {
    String value = preferences.getString(key, defaultValue);
    
    if (value == defaultValue) {
        LOG_DEBUGF("WARNING: Credential '%s' not found in Preferences, using default", key);
        // Returns defaultValue (empty string) without error!
    }
    
    if (value.isEmpty()) {
        LOG_DEBUGF("WARNING: Credential '%s' retrieved from Preferences is empty, using default", key);
        return defaultValue;  // Returns empty string
    }
    
    return value;
}
```

**Problems:**
1. The warning is at `LOG_DEBUGF` level — invisible unless DEBUG mode is enabled
2. The calling code (line 443) doesn't check if the return value is empty
3. No fallback or recovery mechanism — just proceeds with empty password

### Step 4: WiFi Connection Attempt

```cpp
// WiFiManager.cpp:171-186
if (password.isEmpty()) {
    LOG_WARN("WiFi password is empty - attempting open network connection");
}

WiFi.begin(ssid.c_str(), password.c_str());  // Empty password = open network attempt
```

The ESP32 Arduino `WiFi.begin()` with empty password attempts an open (unsecured) network connection. Since the user's AP requires a password, the 4-way handshake times out (reason 15).

---

## Root Cause: When Does the Preferences Key Go Missing?

The password is stored in NVS during `migrateToSecureStorage()` (Config.cpp:326-391). The key can be missing or corrupted due to:

### Scenario 1: First Boot After Flash/OTA (No Migration Occurred)
- Device flashed with new firmware, NVS partition wiped
- User has `***STORED_IN_FLASH***` in config.txt (from previous device or manual edit)
- No plaintext password present to trigger migration
- `wifiPassword` is set to placeholder, censored check passes, load from NVS returns empty

### Scenario 2: NVS Corruption from Power Issues
- User has brownout/power delivery issues
- NVS write during initial migration gets corrupted
- ESP32 NVS has transactional integrity, but power loss during write can leave partial data
- Result: Key appears to exist but value is corrupted/empty, or key missing entirely

### Scenario 3: Race Condition During Migration
- Migration code at line 463-468:
```cpp
if (needsMigration) {
    if (migrateToSecureStorage(sd)) {
        credentialsInFlash = true;
    }
}
```
- If `migrateToSecureStorage()` fails partway (SD card write error, etc.), credentials may not be stored
- But the code doesn't revert the config.txt to plaintext — it stays censored
- Next boot: censored value present, NVS key missing

### Scenario 4: User Manually Edited Config After Migration
- User sees `***STORED_IN_FLASH***` and replaces it with actual password
- But they add trailing spaces, comments, or formatting issues
- `parseLine()` may not handle edge cases correctly
- Password appears "set" but is actually empty or malformed

---

## Why Did It "Spontaneously" Work for Some Users?

The issue #42 mentions one user's device "ended up connecting and he didn't edit any config." Possible explanations:

1. **NVS Layout Changed Between Boots:** The ESP32 NVS library can have timing-dependent initialization behavior. A subsequent boot may have had different power conditions that allowed NVS to initialize correctly.

2. **Config.txt Rewrite Triggered:** If something caused the config to be rewritten (web UI edit, migration retry), the password may have been re-migrated successfully.

3. **Temporary SD Card Read Error:** If the first read of config.txt failed partially, a retry on next boot may have succeeded.

4. **Power Stabilization:** Users with power delivery issues may have had marginal behavior that cleared up on subsequent boots.

---

## Evidence from Provided Logs

### Log Entry Sequence Analysis

```
[--:--:--] [INFO] Loaded WiFi password from flash
[--:--:--] [INFO] Loaded endpoint password from flash
[--:--:--] [INFO] Loaded cloud client secret from flash
```

All three credentials report "loaded from flash" — but the WiFi password is subsequently empty. This suggests:
- The NVS namespace `cpap_creds` was accessible
- The endpoint and cloud secrets were successfully retrieved
- Only the WiFi password key was missing or empty

This is consistent with a partial migration failure or selective NVS corruption.

### Brownout Detection Disabled

```
[--:--:--] [WARN] BROWNOUT_DETECT=OFF — disabling brownout detection per config
```

The user has disabled brownout detection. This is significant because:
- Brownouts during NVS writes can cause corruption
- Without brownout detection, the device continues operating at marginal voltage
- NVS operations may fail silently under low-voltage conditions

---

## Affected Code Locations

| File | Lines | Issue |
|---|---|---|
| `src/Config.cpp` | 443-444 | Unconditional success logging despite empty return |
| `src/Config.cpp` | 93-110 | `loadCredential()` returns default silently; debug-only warning |
| `src/Config.cpp` | 459-468 | No handling for migration failure (config stays censored) |
| `src/WiFiManager.cpp` | 171-173 | Correctly detects empty password but too late |

---

## Fix Recommendations

### Option 1: Validate Loaded Credential (Recommended)

Add validation after loading from Preferences:

```cpp
if (wifiCensored) {
    String loadedPass = loadCredential(PREFS_KEY_WIFI_PASS, "");
    if (loadedPass.isEmpty()) {
        LOG_ERROR("WiFi password is censored but not found in flash!");
        LOG_ERROR("Possible causes: NVS corruption, interrupted migration, or manual config edit");
        LOG_ERROR("To recover: set WIFI_PASSWORD = your_actual_password in config.txt");
        // Optionally: force plaintext mode or trigger re-migration
    } else {
        wifiPassword = loadedPass;
        LOG("Loaded WiFi password from flash");
    }
}
```

### Option 2: Change Log Level

Change the `loadCredential()` warning from `LOG_DEBUGF` to `LOG_WARN` so it's always visible:

```cpp
if (value == defaultValue) {
    LOG_WARN("Credential '%s' not found in flash (NVS), using default", key);
}
```

### Option 3: Migration Failure Recovery

On migration failure, revert config.txt to plaintext so the user can recover:

```cpp
if (migrateToSecureStorage(sd)) {
    credentialsInFlash = true;
} else {
    LOG_ERROR("Migration failed — keeping plaintext credentials");
    // Don't censor config.txt on failure
    // Or: revert config.txt if it was already censored
}
```

### Option 4: Pre-Flight NVS Check

During config loading, verify NVS health:

```cpp
if (!preferences.begin(PREFS_NAMESPACE, true)) {  // Read-only check
    LOG_ERROR("NVS partition not accessible — credentials may be unavailable");
    storePlainText = true;  // Force plaintext fallback
}
```

---

## Workaround for Affected Users

Users experiencing this issue can recover by:

1. **Remove the SD card** from the CPAP device
2. **Edit config.txt** on a computer
3. **Replace `***STORED_IN_FLASH***`** with the actual WiFi password:
   ```
   WIFI_PASSWORD = your_actual_password
   ```
4. **Save and reinsert** the SD card
5. **Power cycle** the device

The device will detect a plaintext password, migrate it to NVS, and censor the config file on the next successful boot.

---

## Related Code Patterns

Similar issues may exist for endpoint password and cloud client secret:

```cpp
// Config.cpp:446-452
if (endpointCensored) {
    endpointPassword = loadCredential(PREFS_KEY_ENDPOINT_PASS, "");
    LOG("Loaded endpoint password from flash");
}
if (cloudSecretCensored) {
    cloudClientSecret = loadCredential(PREFS_KEY_CLOUD_SECRET, "");
    LOG("Loaded cloud client secret from flash");
}
```

Both have the same pattern and could fail silently in the same way. However:
- Endpoint/cloud failures occur later during upload, not at boot
- The upload task has better error handling and logging
- Users see upload failures, not boot failures

---

## Summary

The "WiFi password is blank" bug is a silent failure in the secure credential loading path. When:
1. `config.txt` contains `***STORED_IN_FLASH***` (censored)
2. The actual password is NOT in NVS (corruption, no migration, or first boot)
3. `loadCredential()` returns empty string without visible error
4. WiFi connection fails with 4-way handshake timeout

The fix requires adding validation after loading credentials from Preferences, with clear error messages directing users to recovery steps.

**Files involved:**
- `src/Config.cpp` — credential loading and migration
- `src/WiFiManager.cpp` — WiFi connection (detects but doesn't diagnose empty password)
- `include/Config.h` — constants for NVS keys and censored placeholder
