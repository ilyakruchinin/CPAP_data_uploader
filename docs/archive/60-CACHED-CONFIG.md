# AirSense 10 Therapy-Safe Boot (Cached Config)

## The Problem: The "Deadly Embrace" Reboot Loop
When the ESP32 takes control of the SD card via the MUX, it interrupts the AirSense 10's (AS10) active session. Because the physical SD-WIFI-PRO board remains in the slot, the CPAP's mechanical Card Detect (CD) switch is never released. The CPAP firmware believes the card is still present and attempts to resume I/O operations using its established state (specifically, the Relative Card Address or RCA).

When the ESP32 releases the MUX back to the AS10, the SD card has been re-initialized and holds a new RCA. The AS10 sends commands to the old RCA, receives no acknowledgment, and the RTOS Watchdog Timer expires, forcing a hard system reboot to clear the fatal peripheral timeout.

During therapy, this creates an infinite loop:
1. CPAP panic-reboots the SD slot, killing the ESP32.
2. ESP32 boots up and immediately grabs the SD MUX to read `config.txt`.
3. Grabbing the MUX destroys the CPAP's newly recovering SD session.
4. CPAP panics again.

## The Solution: Therapy-Safe Cached Boot
To break the loop, the ESP32 must **not** grab the MUX during these rapid error-recovery power cycles. 

We introduced an `AS10=true` configuration flag. When enabled:
1. On a normal boot, the ESP32 reads `config.txt` from the SD card and **caches it into NVS**.
2. If the AS10 kills the power, the ESP32 experiences an `ESP_RST_POWERON`.
3. We track consecutive power-on resets (`consec_por`). If `consec_por >= 2`, we are caught in an AS10 error-recovery loop.
4. The ESP32 bypasses the SD card entirely, loads the cached `config.txt` from NVS, and proceeds straight to WiFi/WebUI. 
5. The AS10 is left undisturbed to re-initialize the SD card and continue therapy.

## Implementation Details (Code Snippets)

### 1. `Config.h`
Add the `as10Mode` flag and the cached string loader.

```cpp
class Config {
private:
    // ...
    bool as10Mode;                  // AirSense 10 compatibility
    
public:
    // ...
    bool getAS10Mode() const;
    bool loadFromCachedString(const String& rawConfig);
};
```

### 2. `Config.cpp`
Parse the new flag and implement the cached loader.

```cpp
// In Config::Config() constructor:
as10Mode(false),

// In Config::parseLine():
} else if (key == "AS10") {
    as10Mode = (value.equalsIgnoreCase("true") || value == "1");
}

// Getter:
bool Config::getAS10Mode() const { return as10Mode; }

// Cached loader implementation (skips SD credentials masking):
bool Config::loadFromCachedString(const String& rawConfig) {
    LOG("[AS10] Loading config from NVS cache...");
    int startIdx = 0;
    int len = (int)rawConfig.length();
    while (startIdx < len) {
        int endIdx = rawConfig.indexOf('\n', startIdx);
        if (endIdx == -1) endIdx = len;
        String line = rawConfig.substring(startIdx, endIdx);
        parseLine(line);
        startIdx = endIdx + 1;
    }

    if (maskCredentials && initPreferences()) {
        if (isCensored(wifiPassword)) wifiPassword = loadCredential(PREFS_KEY_WIFI_PASS, "");
        if (isCensored(endpointPassword)) endpointPassword = loadCredential(PREFS_KEY_ENDPOINT_PASS, "");
        if (isCensored(cloudClientSecret)) cloudClientSecret = loadCredential(PREFS_KEY_CLOUD_SECRET, "");
        credentialsInFlash = true;
    }

    // Compute endpoint types
    String upper = endpointType; upper.toUpperCase();
    _hasSmbEndpoint = (upper.indexOf("SMB") >= 0);
    _hasCloudEndpoint = (upper.indexOf("CLOUD") >= 0 || upper.indexOf("SLEEPHQ") >= 0);
    _hasWebdavEndpoint = (upper.indexOf("WEBDAV") >= 0);

    isValid = !wifiSSID.isEmpty();
    return isValid;
}
```

### 3. `main.cpp`
Add NVS helpers, track consecutive power-ons, and conditionally skip the SD boot phase.

```cpp
// ── AS10: NVS config cache helpers ──
static void cacheConfigToNVS(fs::FS& sd) {
    File f = sd.open("/config.txt", FILE_READ);
    if (!f) return;
    
    String content = f.readString();
    f.close();

    Preferences p;
    p.begin("cfg_cache", false);
    p.putString("raw", content);
    p.end();
    LOGF("[AS10] Config cached to NVS (%d bytes)", content.length());
}

static String loadCachedConfigFromNVS() {
    Preferences p;
    p.begin("cfg_cache", true);  // read-only
    String content = p.getString("raw", "");
    p.end();
    return content;
}

void setup() {
    // ... rtc_get_reset_reason ...

    // Track consecutive power-on resets
    uint16_t consecutivePOR = 0;
    {
        Preferences bootStats;
        bootStats.begin("boot_stats", false);
        // ... increment totalBoots ...
        consecutivePOR = bootStats.getUShort("consec_por", 0);
        if (resetReason == ESP_RST_POWERON) {
            consecutivePOR++;
        } else {
            consecutivePOR = 0;
        }
        bootStats.putUShort("consec_por", consecutivePOR);
        bootStats.end();
    }

    // ... trafficMonitor.begin ...

    // ── AS10: Therapy-safe cached boot ──
    bool usedCachedConfig = false;
    if (resetReason == ESP_RST_POWERON && consecutivePOR >= 2) {
        String cached = loadCachedConfigFromNVS();
        if (!cached.isEmpty()) {
            if (config.loadFromCachedString(cached) && config.getAS10Mode()) {
                usedCachedConfig = true;
                LOG_WARN("[AS10] Therapy-safe boot — using cached config, skipping SD card access");
            }
        }
    }

    // NVS flags check (state resets, watchdog kills) runs here unconditionally
    // ...

    if (!usedCachedConfig) {
        // ── Normal boot path ──
        // 1. Run Smart Wait
        // 2. Wait for electrical stabilization (delay 8000)
        // 3. sdManager.takeControl()
        // 4. config.loadFromSD()
        
        // Cache config to NVS if AS10 mode is enabled
        if (config.getAS10Mode()) {
            cacheConfigToNVS(sdManager.getFS());
        }
        
        // 6. sdManager.releaseControl()
    }

    // ── Common path resumes ──
    LOG("Configuration loaded successfully");
    g_debugMode = config.getDebugMode();
    // ...
}
```

## Summary
By caching `config.txt` to NVS, the ESP32 can boot and provide WebUI/OTA access even while the AS10 is actively using the SD card for therapy. The FSM upload routines natively respect bus silence, so uploads will naturally defer until therapy is over.
