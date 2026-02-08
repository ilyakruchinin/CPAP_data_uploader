#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <FS.h>

// Power management enums
enum class WifiTxPower {
    POWER_HIGH,
    POWER_MID,
    POWER_LOW
};

enum class WifiPowerSaving {
    SAVE_NONE,
    SAVE_MID,
    SAVE_MAX
};

// Conditionally include Preferences for ESP32 or use mock for testing
#ifdef UNIT_TEST
    #include "MockPreferences.h"
#else
    #include <Preferences.h>
#endif

class Config {
public:
    // JSON buffer size constant (public for testing)
    static const size_t JSON_FILE_MAX_SIZE = 4096;

private:
    String wifiSSID;
    String wifiPassword;
    String schedule;
    String endpoint;
    String endpointType;  // SMB, CLOUD, SMB,CLOUD
    String endpointUser;
    String endpointPassword;
    int uploadHour;
    int sessionDurationSeconds;
    int maxRetryAttempts;
    int gmtOffsetHours;
    int bootDelaySeconds;
    int sdReleaseIntervalSeconds;
    int sdReleaseWaitMs;
    bool logToSdCard;
    bool isValid;
    
    // Cloud upload settings
    String cloudClientId;
    String cloudClientSecret;
    String cloudTeamId;
    String cloudBaseUrl;
    int cloudDeviceId;
    int maxDays;
    int uploadIntervalMinutes;
    bool cloudInsecureTls;
    
    // Power management settings
    int cpuSpeedMhz;
    WifiTxPower wifiTxPower;
    WifiPowerSaving wifiPowerSaving;
    
    // Credential storage mode flags
    bool storePlainText;
    bool credentialsInFlash;
    
    // Preferences object for secure credential storage
    Preferences preferences;
    
    // Preferences constants
    static const char* PREFS_NAMESPACE;
    static const char* PREFS_KEY_WIFI_PASS;
    static const char* PREFS_KEY_ENDPOINT_PASS;
    static const char* PREFS_KEY_CLOUD_SECRET;
    static const char* CENSORED_VALUE;
    
    // Preferences initialization and cleanup methods
    bool initPreferences();
    void closePreferences();
    
    // Credential storage and retrieval methods
    bool storeCredential(const char* key, const String& value);
    String loadCredential(const char* key, const String& defaultValue);
    bool isCensored(const String& value);
    
    // Config file censoring method (forward declaration for JsonDocument)
    template<typename T>
    bool censorConfigFileWithDoc(fs::FS &sd, T& doc);
    bool censorConfigFile(fs::FS &sd);
    
    // Credential migration method (takes existing JSON doc to avoid double allocation)
    template<typename T>
    bool migrateToSecureStorageWithDoc(fs::FS &sd, T& doc);
    bool migrateToSecureStorage(fs::FS &sd);

public:
    Config();
    ~Config();
    
    bool loadFromSD(fs::FS &sd);
    
    const String& getWifiSSID() const;
    const String& getWifiPassword() const;
    const String& getSchedule() const;
    const String& getEndpoint() const;
    const String& getEndpointType() const;
    const String& getEndpointUser() const;
    const String& getEndpointPassword() const;
    int getUploadHour() const;
    int getSessionDurationSeconds() const;
    int getMaxRetryAttempts() const;
    int getGmtOffsetHours() const;
    int getBootDelaySeconds() const;
    int getSdReleaseIntervalSeconds() const;
    int getSdReleaseWaitMs() const;
    bool getLogToSdCard() const;
    bool valid() const;
    
    // Cloud upload getters
    const String& getCloudClientId() const;
    const String& getCloudClientSecret() const;
    const String& getCloudTeamId() const;
    const String& getCloudBaseUrl() const;
    int getCloudDeviceId() const;
    int getMaxDays() const;
    int getUploadIntervalMinutes() const;
    bool getCloudInsecureTls() const;
    bool hasCloudEndpoint() const;
    bool hasSmbEndpoint() const;
    
    // Power management getters
    int getCpuSpeedMhz() const;
    WifiTxPower getWifiTxPower() const;
    WifiPowerSaving getWifiPowerSaving() const;
    
    // Credential storage mode getters
    bool isStoringPlainText() const;
    bool areCredentialsInFlash() const;
    
private:
    // Helper methods for enum conversion
    static WifiTxPower parseWifiTxPower(const String& str);
    static WifiPowerSaving parseWifiPowerSaving(const String& str);
    static String wifiTxPowerToString(WifiTxPower power);
    static String wifiPowerSavingToString(WifiPowerSaving saving);
};

#endif // CONFIG_H
