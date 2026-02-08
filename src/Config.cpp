#include "Config.h"
#include "Logger.h"
#include <ArduinoJson.h>

// Define static constants for Preferences
const char* Config::PREFS_NAMESPACE = "cpap_creds";
const char* Config::PREFS_KEY_WIFI_PASS = "wifi_pass";
const char* Config::PREFS_KEY_ENDPOINT_PASS = "endpoint_pass";
const char* Config::PREFS_KEY_CLOUD_SECRET = "cloud_secret";
const char* Config::CENSORED_VALUE = "***STORED_IN_FLASH***";
const size_t Config::JSON_FILE_MAX_SIZE;

Config::Config() : 
    uploadHour(12),  // Default: noon
    sessionDurationSeconds(30),  // Default: 30 seconds
    maxRetryAttempts(3),  // Default: 3 attempts
    gmtOffsetHours(0),  // Default: UTC
    bootDelaySeconds(30),  // Default: 30 seconds
    sdReleaseIntervalSeconds(2),  // Default: 2 seconds
    sdReleaseWaitMs(500),  // Default: 500ms
    logToSdCard(false),  // Default: do not log to SD card (debugging only)
    isValid(false),
    
    // Cloud upload defaults
    cloudBaseUrl("https://sleephq.com"),
    cloudDeviceId(0),
    maxDays(0),  // Default: all days
    uploadIntervalMinutes(0),  // Default: use daily schedule
    cloudInsecureTls(false),  // Default: use root CA validation
    
    storePlainText(false),  // Default: secure mode
    credentialsInFlash(false),  // Will be set during loadFromSD
    
    // Power management defaults
    cpuSpeedMhz(240),  // Default: 240MHz (full speed)
    wifiTxPower(WifiTxPower::POWER_HIGH),  // Default: high power
    wifiPowerSaving(WifiPowerSaving::SAVE_NONE)  // Default: no power saving
{}

Config::~Config() {
    closePreferences();
}

bool Config::initPreferences() {
    // Attempt to open Preferences namespace in read-write mode
    if (!preferences.begin(PREFS_NAMESPACE, false)) {
        LOG_ERROR("Failed to initialize Preferences namespace");
        LOG("Falling back to plain text credential storage");
        // Fall back to plain text mode on failure
        storePlainText = true;
        credentialsInFlash = false;
        return false;
    }
    
    LOG_DEBUG("Preferences initialized successfully");
    LOG_DEBUGF("Using Preferences namespace: %s", PREFS_NAMESPACE);
    return true;
}

void Config::closePreferences() {
    // Close Preferences to free resources
    preferences.end();
    LOG_DEBUG("Preferences closed");
}

bool Config::storeCredential(const char* key, const String& value) {
    // Validate that the credential is not empty
    if (value.isEmpty()) {
        LOGF("WARNING: Attempted to store empty credential for key '%s'", key);
        return false;
    }
    
    // Attempt to write the credential to Preferences
    size_t written = preferences.putString(key, value);
    
    if (written == 0) {
        LOGF("ERROR: Failed to store credential '%s' in Preferences", key);
        return false;
    }
    
    LOG_DEBUGF("Credential '%s' stored successfully in Preferences (%d bytes)", key, written);
    return true;
}

String Config::loadCredential(const char* key, const String& defaultValue) {
    // Attempt to read the credential from Preferences
    String value = preferences.getString(key, defaultValue);
    
    // Check if we got the default value (key not found)
    if (value == defaultValue) {
        LOG_DEBUGF("WARNING: Credential '%s' not found in Preferences, using default", key);
    } else {
        // Validate that the retrieved credential is not empty
        if (value.isEmpty()) {
            LOG_DEBUGF("WARNING: Credential '%s' retrieved from Preferences is empty, using default", key);
            return defaultValue;
        }
        LOG_DEBUGF("Credential '%s' loaded successfully from Preferences", key);
    }
    
    return value;
}

bool Config::isCensored(const String& value) {
    // Check if the value matches the censored placeholder
    return value.equals(CENSORED_VALUE);
}

bool Config::censorConfigFile(fs::FS &sd) {
    LOG_DEBUG("Starting config file censoring operation");
    
    // Read existing config.json
    File configFile = sd.open("/config.json", FILE_READ);
    if (!configFile) {
        LOG_ERROR("Cannot open config.json for reading during censoring");
        return false;
    }
    
    // Parse JSON document
    StaticJsonDocument<Config::JSON_FILE_MAX_SIZE> doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();
    
    if (error) {
        LOGF("ERROR: Failed to parse config.json for censoring: %s", error.c_str());
        return false;
    }
    
    return censorConfigFileWithDoc(sd, doc);
}

template<typename T>
bool Config::censorConfigFileWithDoc(fs::FS &sd, T& doc) {
    // Update credential fields with censored values
    doc["WIFI_PASS"] = CENSORED_VALUE;
    doc["ENDPOINT_PASS"] = CENSORED_VALUE;
    if (doc.containsKey("CLOUD_CLIENT_SECRET")) {
        doc["CLOUD_CLIENT_SECRET"] = CENSORED_VALUE;
    }
    
    LOG_DEBUG("Credential fields updated with censored values");
    
    // Write updated JSON back to SD card
    File configFile = sd.open("/config.json", FILE_WRITE);
    if (!configFile) {
        LOG_ERROR("Cannot open config.json for writing during censoring");
        return false;
    }
    
    // Serialize JSON to file (pretty printed)
    // Use serializeJsonPretty if available (ArduinoJson 6.19+), otherwise use serializeJson
    #if ARDUINOJSON_VERSION_MAJOR >= 6 && ARDUINOJSON_VERSION_MINOR >= 19
        size_t bytesWritten = serializeJsonPretty(doc, configFile);
    #else
        size_t bytesWritten = serializeJson(doc, configFile);
    #endif
    configFile.close();
    
    if (bytesWritten == 0) {
        LOG_ERROR("Failed to write censored config to file");
        return false;
    }
    
    LOG_DEBUGF("Config file censored successfully (%d bytes written)", bytesWritten);
    LOG_DEBUG("Credentials are now stored securely in flash memory");
    
    return true;
}

bool Config::migrateToSecureStorage(fs::FS &sd) {
    LOG("========================================");
    LOG("Starting credential migration to secure storage");
    LOG("========================================");
    
    // Step 1: Validate that credentials are not empty
    if (wifiPassword.isEmpty() && endpointPassword.isEmpty() && cloudClientSecret.isEmpty()) {
        LOG_WARN("All credentials are empty, skipping migration");
        return false;
    }
    
    if (wifiPassword.isEmpty()) {
        LOG_WARN("WiFi password is empty, will not migrate WIFI_PASS");
    }
    
    if (endpointPassword.isEmpty()) {
        LOG_WARN("Endpoint password is empty, will not migrate ENDPOINT_PASS");
    }
    
    if (cloudClientSecret.isEmpty()) {
        LOG_DEBUG("Cloud client secret is empty, will not migrate CLOUD_CLIENT_SECRET");
    }
    
    // Step 2: Store WIFI_PASS in Preferences
    bool wifiStored = false;
    if (!wifiPassword.isEmpty()) {
        LOG_DEBUG("Storing WiFi password in Preferences...");
        wifiStored = storeCredential(PREFS_KEY_WIFI_PASS, wifiPassword);
        
        if (!wifiStored) {
            LOG_ERROR("Failed to store WiFi password in Preferences");
            LOG("Migration aborted - keeping plain text credentials");
            return false;
        }
    } else {
        wifiStored = true;  // Skip if empty
    }
    
    // Step 3: Store ENDPOINT_PASS in Preferences
    bool endpointStored = false;
    if (!endpointPassword.isEmpty()) {
        LOG_DEBUG("Storing endpoint password in Preferences...");
        endpointStored = storeCredential(PREFS_KEY_ENDPOINT_PASS, endpointPassword);
        
        if (!endpointStored) {
            LOG_ERROR("Failed to store endpoint password in Preferences");
            LOG("Migration aborted - keeping plain text credentials");
            return false;
        }
    } else {
        endpointStored = true;  // Skip if empty
    }
    
    // Step 3b: Store CLOUD_CLIENT_SECRET in Preferences
    bool cloudSecretStored = false;
    if (!cloudClientSecret.isEmpty()) {
        LOG_DEBUG("Storing cloud client secret in Preferences...");
        cloudSecretStored = storeCredential(PREFS_KEY_CLOUD_SECRET, cloudClientSecret);
        
        if (!cloudSecretStored) {
            LOG_ERROR("Failed to store cloud client secret in Preferences");
            LOG("Migration aborted - keeping plain text credentials");
            return false;
        }
    } else {
        cloudSecretStored = true;  // Skip if empty
    }
    
    // Step 4: Verify credentials were stored correctly by reading back
    LOG_DEBUG("Verifying stored credentials...");
    bool verificationPassed = true;
    
    if (!wifiPassword.isEmpty()) {
        String verifyWifi = loadCredential(PREFS_KEY_WIFI_PASS, "");
        if (verifyWifi != wifiPassword) {
            LOG_ERROR("WiFi password verification failed - stored value does not match");
            verificationPassed = false;
        } else {
            LOG_DEBUG("WiFi password verification: PASSED");
        }
    }
    
    if (!endpointPassword.isEmpty()) {
        String verifyEndpoint = loadCredential(PREFS_KEY_ENDPOINT_PASS, "");
        if (verifyEndpoint != endpointPassword) {
            LOG_ERROR("Endpoint password verification failed - stored value does not match");
            verificationPassed = false;
        } else {
            LOG_DEBUG("Endpoint password verification: PASSED");
        }
    }
    
    if (!cloudClientSecret.isEmpty()) {
        String verifyCloud = loadCredential(PREFS_KEY_CLOUD_SECRET, "");
        if (verifyCloud != cloudClientSecret) {
            LOG_ERROR("Cloud client secret verification failed - stored value does not match");
            verificationPassed = false;
        } else {
            LOG_DEBUG("Cloud client secret verification: PASSED");
        }
    }
    
    if (!verificationPassed) {
        LOG_ERROR("Credential verification failed");
        LOG("Migration aborted - keeping plain text credentials");
        return false;
    }
    
    LOG_DEBUG("All credentials verified successfully");
    
    // Step 5: Censor config.json after successful Preferences storage
    LOG_DEBUG("Censoring config.json file...");
    if (!censorConfigFile(sd)) {
        LOG_ERROR("Failed to censor config.json");
        LOG_WARN("Credentials are stored in Preferences but config.json still contains plain text");
        LOG("Manual intervention may be required");
        return false;
    }
    
    // Step 6: Log success
    LOG("========================================");
    LOG("Credential migration completed successfully");
    LOG("Credentials are now stored securely in flash memory");
    LOG("config.json has been updated with censored values");
    LOG("========================================");
    
    return true;
}

template<typename T>
bool Config::migrateToSecureStorageWithDoc(fs::FS &sd, T& doc) {
    LOG("========================================");
    LOG("Starting credential migration to secure storage");
    LOG("========================================");
    
    // Step 1: Validate that credentials are not empty
    if (wifiPassword.isEmpty() && endpointPassword.isEmpty() && cloudClientSecret.isEmpty()) {
        LOG_WARN("All credentials are empty, skipping migration");
        return false;
    }
    
    if (wifiPassword.isEmpty()) {
        LOG_WARN("WiFi password is empty, will not migrate WIFI_PASS");
    }
    
    if (endpointPassword.isEmpty()) {
        LOG_WARN("Endpoint password is empty, will not migrate ENDPOINT_PASS");
    }
    
    if (cloudClientSecret.isEmpty()) {
        LOG_DEBUG("Cloud client secret is empty, will not migrate CLOUD_CLIENT_SECRET");
    }
    
    // Step 2: Store WIFI_PASS in Preferences
    bool wifiStored = false;
    if (!wifiPassword.isEmpty()) {
        LOG_DEBUG("Storing WiFi password in Preferences...");
        wifiStored = storeCredential(PREFS_KEY_WIFI_PASS, wifiPassword);
        
        if (!wifiStored) {
            LOG_ERROR("Failed to store WiFi password in Preferences");
            LOG("Migration aborted - keeping plain text credentials");
            return false;
        }
    } else {
        wifiStored = true;  // Skip if empty
    }
    
    // Step 3: Store ENDPOINT_PASS in Preferences
    bool endpointStored = false;
    if (!endpointPassword.isEmpty()) {
        LOG_DEBUG("Storing endpoint password in Preferences...");
        endpointStored = storeCredential(PREFS_KEY_ENDPOINT_PASS, endpointPassword);
        
        if (!endpointStored) {
            LOG_ERROR("Failed to store endpoint password in Preferences");
            LOG("Migration aborted - keeping plain text credentials");
            return false;
        }
    } else {
        endpointStored = true;  // Skip if empty
    }
    
    // Step 3b: Store CLOUD_CLIENT_SECRET in Preferences
    bool cloudSecretStored = false;
    if (!cloudClientSecret.isEmpty()) {
        LOG_DEBUG("Storing cloud client secret in Preferences...");
        cloudSecretStored = storeCredential(PREFS_KEY_CLOUD_SECRET, cloudClientSecret);
        
        if (!cloudSecretStored) {
            LOG_ERROR("Failed to store cloud client secret in Preferences");
            LOG("Migration aborted - keeping plain text credentials");
            return false;
        }
    } else {
        cloudSecretStored = true;  // Skip if empty
    }
    
    // Step 4: Verify credentials were stored correctly by reading back
    LOG_DEBUG("Verifying stored credentials...");
    bool verificationPassed = true;
    
    if (!wifiPassword.isEmpty()) {
        String verifyWifi = loadCredential(PREFS_KEY_WIFI_PASS, "");
        if (verifyWifi != wifiPassword) {
            LOG_ERROR("WiFi password verification failed - stored value does not match");
            verificationPassed = false;
        } else {
            LOG_DEBUG("WiFi password verification: PASSED");
        }
    }
    
    if (!endpointPassword.isEmpty()) {
        String verifyEndpoint = loadCredential(PREFS_KEY_ENDPOINT_PASS, "");
        if (verifyEndpoint != endpointPassword) {
            LOG_ERROR("Endpoint password verification failed - stored value does not match");
            verificationPassed = false;
        } else {
            LOG_DEBUG("Endpoint password verification: PASSED");
        }
    }
    
    if (!cloudClientSecret.isEmpty()) {
        String verifyCloud = loadCredential(PREFS_KEY_CLOUD_SECRET, "");
        if (verifyCloud != cloudClientSecret) {
            LOG_ERROR("Cloud client secret verification failed - stored value does not match");
            verificationPassed = false;
        } else {
            LOG_DEBUG("Cloud client secret verification: PASSED");
        }
    }
    
    if (!verificationPassed) {
        LOG_ERROR("Credential verification failed");
        LOG("Migration aborted - keeping plain text credentials");
        return false;
    }
    
    LOG_DEBUG("All credentials verified successfully");
    
    // Step 5: Censor config.json after successful Preferences storage
    // Reuse the existing JSON document to avoid double allocation
    LOG_DEBUG("Censoring config.json file...");
    if (!censorConfigFileWithDoc(sd, doc)) {
        LOG_ERROR("Failed to censor config.json");
        LOG_WARN("Credentials are stored in Preferences but config.json still contains plain text");
        LOG("Manual intervention may be required");
        return false;
    }
    
    // Step 6: Log success
    LOG("========================================");
    LOG("Credential migration completed successfully");
    LOG("Credentials are now stored securely in flash memory");
    LOG("config.json has been updated with censored values");
    LOG("========================================");
    
    return true;
}

bool Config::loadFromSD(fs::FS &sd) {
    LOG("========================================");
    LOG("Loading configuration from SD card");
    LOG("========================================");
    
    // Step 1: Read and parse config.json
    File configFile = sd.open("/config.json");
    if (!configFile) {
        LOG_ERROR("Failed to open config file");
        return false;
    }

    StaticJsonDocument<Config::JSON_FILE_MAX_SIZE> doc;
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();

    if (error) {
        LOGF("ERROR: Failed to parse config: %s", error.c_str());
        return false;
    }

    LOG("Config file parsed successfully");
    
    // Step 2: Parse STORE_CREDENTIALS_PLAIN_TEXT flag (defaults to false for secure mode)
    storePlainText = doc["STORE_CREDENTIALS_PLAIN_TEXT"] | false;
    
    if (storePlainText) {
        LOG_DEBUG("========================================");
        LOG_DEBUG("PLAIN TEXT MODE: Credentials will be stored in config.json");
        LOG_DEBUG("========================================");
    } else {
        LOG_DEBUG("========================================");
        LOG_DEBUG("SECURE MODE: Credentials will be stored in flash memory");
        LOG_DEBUG("========================================");
    }
    
    // Step 3: Load non-credential configuration fields
    wifiSSID = doc["WIFI_SSID"] | "";
    
    // Validate SSID length (WiFi standard limit is 32 characters)
    if (wifiSSID.length() > 32) {
        LOG_ERROR("SSID exceeds maximum length of 32 characters");
        LOGF("SSID length: %d characters", wifiSSID.length());
        LOG("Truncating SSID to 32 characters");
        wifiSSID = wifiSSID.substring(0, 32);
    }
    
    schedule = doc["SCHEDULE"] | "";
    endpoint = doc["ENDPOINT"] | "";
    endpointType = doc["ENDPOINT_TYPE"] | "";
    endpointUser = doc["ENDPOINT_USER"] | "";
    
    // Parse new configuration fields with defaults
    uploadHour = doc["UPLOAD_HOUR"] | 12;
    sessionDurationSeconds = doc["SESSION_DURATION_SECONDS"] | 30;
    maxRetryAttempts = doc["MAX_RETRY_ATTEMPTS"] | 3;
    gmtOffsetHours = doc["GMT_OFFSET_HOURS"] | 0;
    bootDelaySeconds = doc["BOOT_DELAY_SECONDS"] | 30;
    sdReleaseIntervalSeconds = doc["SD_RELEASE_INTERVAL_SECONDS"] | 2;
    sdReleaseWaitMs = doc["SD_RELEASE_WAIT_MS"] | 500;
    logToSdCard = doc["LOG_TO_SD_CARD"] | false;
    
    // Cloud upload settings
    cloudClientId = doc["CLOUD_CLIENT_ID"] | "";
    cloudTeamId = doc["CLOUD_TEAM_ID"] | "";
    cloudBaseUrl = doc["CLOUD_BASE_URL"] | "https://sleephq.com";
    cloudDeviceId = doc["CLOUD_DEVICE_ID"] | 0;
    maxDays = doc["MAX_DAYS"] | 0;
    uploadIntervalMinutes = doc["UPLOAD_INTERVAL_MINUTES"] | 0;
    cloudInsecureTls = doc["CLOUD_INSECURE_TLS"] | false;
    
    // Validate MAX_DAYS
    if (maxDays < 0) {
        LOG_WARN("MAX_DAYS cannot be negative, setting to 0 (all days)");
        maxDays = 0;
    }
    
    // Validate UPLOAD_INTERVAL_MINUTES
    if (uploadIntervalMinutes < 0) {
        LOG_WARN("UPLOAD_INTERVAL_MINUTES cannot be negative, setting to 0 (daily schedule)");
        uploadIntervalMinutes = 0;
    }
    
    // Power management settings with validation
    cpuSpeedMhz = doc["CPU_SPEED_MHZ"] | 240;
    if (cpuSpeedMhz < 80) {
        LOG_WARN("CPU_SPEED_MHZ below minimum (80MHz), setting to 80MHz");
        cpuSpeedMhz = 80;
    } else if (cpuSpeedMhz > 240) {
        LOG_WARN("CPU_SPEED_MHZ above maximum (240MHz), setting to 240MHz");
        cpuSpeedMhz = 240;
    }
    
    String wifiTxPowerStr = doc["WIFI_TX_PWR"] | "high";
    wifiTxPower = parseWifiTxPower(wifiTxPowerStr);
    
    String wifiPowerSavingStr = doc["WIFI_PWR_SAVING"] | "none";
    wifiPowerSaving = parseWifiPowerSaving(wifiPowerSavingStr);
    
    // Step 4: Load credentials based on storage mode
    if (storePlainText) {
        // Plain text mode: Load credentials directly from config.json
        LOG_DEBUG("Loading credentials from config.json (plain text mode)");
        wifiPassword = doc["WIFI_PASS"] | "";
        endpointPassword = doc["ENDPOINT_PASS"] | "";
        cloudClientSecret = doc["CLOUD_CLIENT_SECRET"] | "";
        credentialsInFlash = false;
        
        LOG_DEBUG("Credentials loaded from config.json");
        LOG_WARN("Credentials are stored in plain text");
    } else {
        // Secure mode: Check if credentials are censored and handle accordingly
        LOG_DEBUG("Checking credential storage status...");
        
        // Initialize Preferences for secure storage
        if (!initPreferences()) {
            LOG_ERROR("Failed to initialize Preferences");
            LOG("Falling back to plain text mode for this session");
            wifiPassword = doc["WIFI_PASS"] | "";
            endpointPassword = doc["ENDPOINT_PASS"] | "";
            cloudClientSecret = doc["CLOUD_CLIENT_SECRET"] | "";
            credentialsInFlash = false;
            storePlainText = true;  // Force plain text mode due to Preferences failure
        } else {
            // Read credential values from config.json
            String configWifiPass = doc["WIFI_PASS"] | "";
            String configEndpointPass = doc["ENDPOINT_PASS"] | "";
            String configCloudSecret = doc["CLOUD_CLIENT_SECRET"] | "";
            
            // Check if credentials are censored
            bool wifiCensored = isCensored(configWifiPass);
            bool endpointCensored = isCensored(configEndpointPass);
            bool cloudSecretCensored = isCensored(configCloudSecret);
            
            // PRIORITY: Always use config file credentials if they are not censored
            // Handle each credential individually - users can update one or both
            
            LOG_DEBUG("Processing credentials individually...");
            LOGF("WiFi password censored: %s", wifiCensored ? "YES" : "NO");
            LOGF("Endpoint password censored: %s", endpointCensored ? "YES" : "NO");
            if (!configCloudSecret.isEmpty() || cloudSecretCensored) {
                LOGF("Cloud client secret censored: %s", cloudSecretCensored ? "YES" : "NO");
            }
            
            // Handle WiFi password
            if (!wifiCensored && !configWifiPass.isEmpty()) {
                LOG("Using WiFi password from config.json (user provided new credential)");
                wifiPassword = configWifiPass;
            } else if (wifiCensored) {
                LOG("Loading WiFi password from flash memory (censored in config)");
                wifiPassword = loadCredential(PREFS_KEY_WIFI_PASS, "");
            } else {
                LOG_WARN("WiFi password is empty in config.json");
                wifiPassword = "";
            }
            
            // Handle endpoint password
            if (!endpointCensored && !configEndpointPass.isEmpty()) {
                LOG("Using endpoint password from config.json (user provided new credential)");
                endpointPassword = configEndpointPass;
            } else if (endpointCensored) {
                LOG("Loading endpoint password from flash memory (censored in config)");
                endpointPassword = loadCredential(PREFS_KEY_ENDPOINT_PASS, "");
            } else {
                LOG_WARN("Endpoint password is empty in config.json");
                endpointPassword = "";
            }
            
            // Handle cloud client secret
            if (!cloudSecretCensored && !configCloudSecret.isEmpty()) {
                LOG("Using cloud client secret from config.json (user provided new credential)");
                cloudClientSecret = configCloudSecret;
            } else if (cloudSecretCensored) {
                LOG("Loading cloud client secret from flash memory (censored in config)");
                cloudClientSecret = loadCredential(PREFS_KEY_CLOUD_SECRET, "");
            } else {
                cloudClientSecret = "";
            }
            
            // Determine if we have any credentials in flash
            credentialsInFlash = (wifiCensored || endpointCensored || cloudSecretCensored);
            
            // Check if user provided any new credentials that need migration
            bool hasNewWifiCred = (!wifiCensored && !configWifiPass.isEmpty());
            bool hasNewEndpointCred = (!endpointCensored && !configEndpointPass.isEmpty());
            bool hasNewCloudSecret = (!cloudSecretCensored && !configCloudSecret.isEmpty());
            
            if (hasNewWifiCred || hasNewEndpointCred || hasNewCloudSecret) {
                // Attempt migration of new credentials using existing doc to avoid double allocation
                if (migrateToSecureStorageWithDoc(sd, doc)) {
                    LOG("New credentials migrated to secure storage successfully");
                    credentialsInFlash = true;
                } else {
                    LOG_WARN("Migration failed - new credentials will remain in plain text");
                }
            } else if (wifiCensored || endpointCensored || cloudSecretCensored) {
                LOG("All credentials loaded from secure flash memory");
                LOG("Credential storage: SECURE (flash memory)");
            }
        }
    }
    
    // Step 5: Validate configuration
    // WIFI_SSID is always required
    // ENDPOINT is required for SMB; for CLOUD, CLOUD_CLIENT_ID is required instead
    bool hasValidEndpoint = false;
    if (hasSmbEndpoint()) {
        hasValidEndpoint = !endpoint.isEmpty();
        if (!hasValidEndpoint) {
            LOG_ERROR("SMB endpoint configured but ENDPOINT is empty");
        }
    }
    if (hasCloudEndpoint()) {
        bool cloudValid = !cloudClientId.isEmpty();
        if (!cloudValid) {
            LOG_ERROR("CLOUD endpoint configured but CLOUD_CLIENT_ID is empty");
        }
        hasValidEndpoint = hasValidEndpoint || cloudValid;
    }
    if (!hasSmbEndpoint() && !hasCloudEndpoint()) {
        // Legacy: require ENDPOINT for backward compatibility
        hasValidEndpoint = !endpoint.isEmpty();
    }
    
    isValid = !wifiSSID.isEmpty() && hasValidEndpoint;
    
    if (isValid) {
        LOG("========================================");
        LOG("Configuration loaded successfully");
        LOGF("Endpoint type: %s", endpointType.c_str());
        LOG_DEBUGF("Storage mode: %s", storePlainText ? "PLAIN TEXT" : "SECURE");
        LOG_DEBUGF("Credentials in flash: %s", credentialsInFlash ? "YES" : "NO");
        if (hasCloudEndpoint()) {
            LOGF("Cloud base URL: %s", cloudBaseUrl.c_str());
            LOGF("Cloud device ID: %d", cloudDeviceId);
            if (!cloudTeamId.isEmpty()) {
                LOGF("Cloud team ID: %s", cloudTeamId.c_str());
            } else {
                LOG("Cloud team ID: auto-discover");
            }
            if (cloudInsecureTls) {
                LOG_WARN("Cloud TLS: INSECURE (certificate validation disabled)");
            }
        }
        if (maxDays > 0) {
            LOGF("MAX_DAYS: %d (only upload last %d days of data)", maxDays, maxDays);
        }
        if (uploadIntervalMinutes > 0) {
            LOGF("Upload interval: every %d minutes", uploadIntervalMinutes);
        }
        LOG("========================================");
    } else {
        LOG_ERROR("Configuration validation failed");
        LOG("Check WIFI_SSID and ENDPOINT/CLOUD_CLIENT_ID settings");
    }
    
    return isValid;
}

const String& Config::getWifiSSID() const { return wifiSSID; }
const String& Config::getWifiPassword() const { return wifiPassword; }
const String& Config::getSchedule() const { return schedule; }
const String& Config::getEndpoint() const { return endpoint; }
const String& Config::getEndpointType() const { return endpointType; }
const String& Config::getEndpointUser() const { return endpointUser; }
const String& Config::getEndpointPassword() const { return endpointPassword; }
int Config::getUploadHour() const { return uploadHour; }
int Config::getSessionDurationSeconds() const { return sessionDurationSeconds; }
int Config::getMaxRetryAttempts() const { return maxRetryAttempts; }
int Config::getGmtOffsetHours() const { return gmtOffsetHours; }
int Config::getBootDelaySeconds() const { return bootDelaySeconds; }
int Config::getSdReleaseIntervalSeconds() const { return sdReleaseIntervalSeconds; }
int Config::getSdReleaseWaitMs() const { return sdReleaseWaitMs; }
bool Config::getLogToSdCard() const { return logToSdCard; }
bool Config::valid() const { return isValid; }

// Credential storage mode getters
bool Config::isStoringPlainText() const { return storePlainText; }
bool Config::areCredentialsInFlash() const { return credentialsInFlash; }

// Cloud upload getters
const String& Config::getCloudClientId() const { return cloudClientId; }
const String& Config::getCloudClientSecret() const { return cloudClientSecret; }
const String& Config::getCloudTeamId() const { return cloudTeamId; }
const String& Config::getCloudBaseUrl() const { return cloudBaseUrl; }
int Config::getCloudDeviceId() const { return cloudDeviceId; }
int Config::getMaxDays() const { return maxDays; }
int Config::getUploadIntervalMinutes() const { return uploadIntervalMinutes; }
bool Config::getCloudInsecureTls() const { return cloudInsecureTls; }

bool Config::hasCloudEndpoint() const {
    String upper = endpointType;
    upper.toUpperCase();
    return (upper.indexOf("CLOUD") >= 0 || upper.indexOf("SLEEPHQ") >= 0);
}

bool Config::hasSmbEndpoint() const {
    String upper = endpointType;
    upper.toUpperCase();
    return (upper.indexOf("SMB") >= 0);
}

// Power management getters
int Config::getCpuSpeedMhz() const { return cpuSpeedMhz; }
WifiTxPower Config::getWifiTxPower() const { return wifiTxPower; }
WifiPowerSaving Config::getWifiPowerSaving() const { return wifiPowerSaving; }

// Helper methods for enum conversion
WifiTxPower Config::parseWifiTxPower(const String& str) {
    String lowerStr = str;
    lowerStr.toLowerCase();
    
    if (lowerStr == "high") {
        return WifiTxPower::POWER_HIGH;
    } else if (lowerStr == "mid") {
        return WifiTxPower::POWER_MID;
    } else if (lowerStr == "low") {
        return WifiTxPower::POWER_LOW;
    } else {
        LOG_WARN("Invalid WIFI_TX_PWR value, defaulting to 'high'");
        return WifiTxPower::POWER_HIGH;
    }
}

WifiPowerSaving Config::parseWifiPowerSaving(const String& str) {
    String lowerStr = str;
    lowerStr.toLowerCase();
    
    if (lowerStr == "none") {
        return WifiPowerSaving::SAVE_NONE;
    } else if (lowerStr == "mid") {
        return WifiPowerSaving::SAVE_MID;
    } else if (lowerStr == "max") {
        return WifiPowerSaving::SAVE_MAX;
    } else {
        LOG_WARN("Invalid WIFI_PWR_SAVING value, defaulting to 'none'");
        return WifiPowerSaving::SAVE_NONE;
    }
}

String Config::wifiTxPowerToString(WifiTxPower power) {
    switch (power) {
        case WifiTxPower::POWER_HIGH: return "high";
        case WifiTxPower::POWER_MID: return "mid";
        case WifiTxPower::POWER_LOW: return "low";
        default: return "high";
    }
}

String Config::wifiPowerSavingToString(WifiPowerSaving saving) {
    switch (saving) {
        case WifiPowerSaving::SAVE_NONE: return "none";
        case WifiPowerSaving::SAVE_MID: return "mid";
        case WifiPowerSaving::SAVE_MAX: return "max";
        default: return "none";
    }
}
