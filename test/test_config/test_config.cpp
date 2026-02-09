#include <unity.h>
#include "Arduino.h"
#include "FS.h"
#include "ArduinoJson.h"

// Include mock implementations
#include "../mocks/Arduino.cpp"

// Mock Logger before including Config
#include "../mocks/MockLogger.h"
#define LOGGER_H  // Prevent real Logger.h from being included

// Mock Preferences before including Config
#include "../mocks/MockPreferences.h"

// Include the Config implementation
#include "Config.h"
#include "../../src/Config.cpp"

fs::FS mockSD;

void setUp(void) {
    // Clear the mock filesystem before each test
    mockSD.clear();
    
    // Clear Preferences data between tests
    Preferences::clearAll();
}

void tearDown(void) {
    // Cleanup after each test
    mockSD.clear();
    Preferences::clearAll();
}

// Test loading a valid configuration file
void test_config_load_valid() {
    // Create a valid config.json file
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "TestPassword123",
        "SCHEDULE": "DAILY",
        "ENDPOINT": "//192.168.1.100/share/uploads",
        "ENDPOINT_TYPE": "SMB",
        "ENDPOINT_USER": "testuser",
        "ENDPOINT_PASS": "testpass",
        "UPLOAD_HOUR": 14,
        "SESSION_DURATION_SECONDS": 10,
        "MAX_RETRY_ATTEMPTS": 5,
        "GMT_OFFSET_HOURS": -8
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    TEST_ASSERT_EQUAL_STRING("TestNetwork", config.getWifiSSID().c_str());
    TEST_ASSERT_EQUAL_STRING("TestPassword123", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("DAILY", config.getSchedule().c_str());
    TEST_ASSERT_EQUAL_STRING("//192.168.1.100/share/uploads", config.getEndpoint().c_str());
    TEST_ASSERT_EQUAL_STRING("SMB", config.getEndpointType().c_str());
    TEST_ASSERT_EQUAL_STRING("testuser", config.getEndpointUser().c_str());
    TEST_ASSERT_EQUAL_STRING("testpass", config.getEndpointPassword().c_str());
    TEST_ASSERT_EQUAL(14, config.getUploadHour());
    TEST_ASSERT_EQUAL(10, config.getSessionDurationSeconds());
    TEST_ASSERT_EQUAL(5, config.getMaxRetryAttempts());
    TEST_ASSERT_EQUAL(-8, config.getGmtOffsetHours());
}

// Test loading configuration with default values
void test_config_load_with_defaults() {
    // Create a minimal config.json with only required fields
    std::string configContent = R"({
        "WIFI_SSID": "MinimalNetwork",
        "ENDPOINT": "//server/share"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    TEST_ASSERT_EQUAL_STRING("MinimalNetwork", config.getWifiSSID().c_str());
    TEST_ASSERT_EQUAL_STRING("//server/share", config.getEndpoint().c_str());
    
    // Check default values
    TEST_ASSERT_EQUAL(12, config.getUploadHour());  // Default noon
    TEST_ASSERT_EQUAL(300, config.getSessionDurationSeconds());  // Default 300 seconds
    TEST_ASSERT_EQUAL(3, config.getMaxRetryAttempts());  // Default 3 attempts
    TEST_ASSERT_EQUAL(0, config.getGmtOffsetHours());  // Default UTC
    TEST_ASSERT_EQUAL(30, config.getBootDelaySeconds());  // Default 30 seconds
    TEST_ASSERT_EQUAL(2, config.getSdReleaseIntervalSeconds());  // Default 2 seconds
    TEST_ASSERT_EQUAL(500, config.getSdReleaseWaitMs());  // Default 500ms
    TEST_ASSERT_FALSE(config.getLogToSdCard());  // Default false (no SD logging)
}

// Test loading configuration with missing SSID (should fail)
void test_config_load_missing_ssid() {
    std::string configContent = R"({
        "WIFI_PASS": "password",
        "ENDPOINT": "//server/share"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_FALSE(loaded);
    TEST_ASSERT_FALSE(config.valid());
}

// Test loading configuration with missing endpoint (should fail)
void test_config_load_missing_endpoint() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "password"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_FALSE(loaded);
    TEST_ASSERT_FALSE(config.valid());
}

// Test loading when config file doesn't exist
void test_config_load_file_not_found() {
    // Don't add any file to the mock filesystem
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_FALSE(loaded);
    TEST_ASSERT_FALSE(config.valid());
}

// Test loading invalid JSON
void test_config_load_invalid_json() {
    std::string configContent = "{ invalid json content";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_FALSE(loaded);
    TEST_ASSERT_FALSE(config.valid());
}

// Test loading empty config file
void test_config_load_empty_file() {
    mockSD.addFile("/config.json", "");
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_FALSE(loaded);
    TEST_ASSERT_FALSE(config.valid());
}

// Test WebDAV endpoint type
void test_config_webdav_endpoint() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT": "https://cloud.example.com/remote.php/dav/files/user/",
        "ENDPOINT_TYPE": "WEBDAV",
        "ENDPOINT_USER": "webdavuser",
        "ENDPOINT_PASS": "webdavpass"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    TEST_ASSERT_EQUAL_STRING("WEBDAV", config.getEndpointType().c_str());
    TEST_ASSERT_EQUAL_STRING("https://cloud.example.com/remote.php/dav/files/user/", config.getEndpoint().c_str());
}

// Test SleepHQ endpoint type (cloud endpoint requires CLOUD_CLIENT_ID)
void test_config_sleephq_endpoint() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT_TYPE": "SLEEPHQ",
        "CLOUD_CLIENT_ID": "test_client_id",
        "CLOUD_CLIENT_SECRET": "test_secret"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    TEST_ASSERT_EQUAL_STRING("SLEEPHQ", config.getEndpointType().c_str());
    TEST_ASSERT_TRUE(config.hasCloudEndpoint());
}

// Test configuration with negative GMT offset (e.g., PST)
void test_config_negative_gmt_offset() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT": "//server/share",
        "GMT_OFFSET_HOURS": -8
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL(-8, config.getGmtOffsetHours());  // -8 hours (PST)
}

// Test configuration with positive GMT offset (e.g., CET)
void test_config_positive_gmt_offset() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT": "//server/share",
        "GMT_OFFSET_HOURS": 1
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL(1, config.getGmtOffsetHours());  // +1 hour (CET)
}

// Test configuration with various upload hours
void test_config_upload_hours() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT": "//server/share",
        "UPLOAD_HOUR": 23
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL(23, config.getUploadHour());
}

// Test configuration with long session duration
void test_config_long_session_duration() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT": "//server/share",
        "SESSION_DURATION_SECONDS": 300
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL(300, config.getSessionDurationSeconds());  // 5 minutes
}

// Test configuration with high retry attempts
void test_config_high_retry_attempts() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT": "//server/share",
        "MAX_RETRY_ATTEMPTS": 10
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL(10, config.getMaxRetryAttempts());
}

// Test new boot delay and SD release configuration
void test_config_boot_delay_and_sd_release() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT": "//server/share",
        "BOOT_DELAY_SECONDS": 60,
        "SD_RELEASE_INTERVAL_SECONDS": 5,
        "SD_RELEASE_WAIT_MS": 1000,
        "LOG_TO_SD_CARD": true
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL(60, config.getBootDelaySeconds());
    TEST_ASSERT_EQUAL(5, config.getSdReleaseIntervalSeconds());
    TEST_ASSERT_EQUAL(1000, config.getSdReleaseWaitMs());
    TEST_ASSERT_TRUE(config.getLogToSdCard());
}

// Test configuration with all timing fields
void test_config_all_timing_fields() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT": "//server/share",
        "SESSION_DURATION_SECONDS": 60,
        "MAX_RETRY_ATTEMPTS": 5,
        "BOOT_DELAY_SECONDS": 45,
        "SD_RELEASE_INTERVAL_SECONDS": 3,
        "SD_RELEASE_WAIT_MS": 750,
        "LOG_TO_SD_CARD": false
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_EQUAL(60, config.getSessionDurationSeconds());
    TEST_ASSERT_EQUAL(5, config.getMaxRetryAttempts());
    TEST_ASSERT_EQUAL(45, config.getBootDelaySeconds());
    TEST_ASSERT_EQUAL(3, config.getSdReleaseIntervalSeconds());
    TEST_ASSERT_EQUAL(750, config.getSdReleaseWaitMs());
    TEST_ASSERT_FALSE(config.getLogToSdCard());
}


// ============================================================================
// CREDENTIAL SECURITY TESTS (Preferences-based secure storage)
// ============================================================================

// Test loading config with plain text mode enabled
void test_config_plain_text_mode() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "PlainTextPassword",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "PlainEndpointPass",
        "STORE_CREDENTIALS_PLAIN_TEXT": true
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE_MESSAGE(loaded, "Config should load successfully");
    TEST_ASSERT_TRUE_MESSAGE(config.valid(), "Config should be valid");
    TEST_ASSERT_TRUE_MESSAGE(config.isStoringPlainText(), "Should be in plain text mode");
    TEST_ASSERT_FALSE_MESSAGE(config.areCredentialsInFlash(), "Credentials should not be in flash");
    TEST_ASSERT_EQUAL_STRING("PlainTextPassword", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("PlainEndpointPass", config.getEndpointPassword().c_str());
}

// Test loading config with secure mode (default behavior)
void test_config_secure_mode_migration() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "SecurePassword123",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "SecureEndpointPass"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    TEST_ASSERT_FALSE(config.isStoringPlainText());
    TEST_ASSERT_TRUE(config.areCredentialsInFlash());
    
    // Credentials should be loaded from Preferences
    TEST_ASSERT_EQUAL_STRING("SecurePassword123", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("SecureEndpointPass", config.getEndpointPassword().c_str());
    
    // Config file should be censored
    std::vector<uint8_t> contentVec = mockSD.getFileContent(String("/config.json"));
    std::string updatedConfig(contentVec.begin(), contentVec.end());
    TEST_ASSERT_TRUE(updatedConfig.find("***STORED_IN_FLASH***") != std::string::npos);
}

// Test loading config with already censored credentials
void test_config_secure_mode_already_censored() {
    // First, create a config and let it migrate
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "OriginalPassword",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "OriginalEndpointPass"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    {
        Config config1;
        bool loaded1 = config1.loadFromSD(mockSD);
        TEST_ASSERT_TRUE(loaded1);
        // config1 destructor called here, closing Preferences
    }
    
    // Now create a new config object and load again (simulating reboot)
    // The config file should now be censored and Preferences should have the data
    Config config2;
    bool loaded = config2.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config2.valid());
    TEST_ASSERT_FALSE(config2.isStoringPlainText());
    TEST_ASSERT_TRUE(config2.areCredentialsInFlash());
    
    // Should load credentials from Preferences
    TEST_ASSERT_EQUAL_STRING("OriginalPassword", config2.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("OriginalEndpointPass", config2.getEndpointPassword().c_str());
}

// Test credential storage with various string lengths
void test_config_credential_storage_various_lengths() {
    // Test short password
    std::string shortConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "abc",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "123"
    })";
    
    mockSD.clear();
    mockSD.addFile("/config.json", shortConfig);
    
    Config config1;
    bool loaded1 = config1.loadFromSD(mockSD);
    TEST_ASSERT_TRUE(loaded1);
    TEST_ASSERT_EQUAL_STRING("abc", config1.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("123", config1.getEndpointPassword().c_str());
    
    // Test long password (64 characters)
    std::string longPassword = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@";
    std::string longConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": ")" + longPassword + R"(",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": ")" + longPassword + R"("
    })";
    
    mockSD.clear();
    mockSD.addFile("/config.json", longConfig);
    
    Config config2;
    bool loaded2 = config2.loadFromSD(mockSD);
    TEST_ASSERT_TRUE(loaded2);
    TEST_ASSERT_EQUAL_STRING(longPassword.c_str(), config2.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING(longPassword.c_str(), config2.getEndpointPassword().c_str());
    
    // Test password with special characters
    std::string specialPass = "P@ssw0rd!#$%^&*()";
    std::string specialEndpoint = "End!@#$%^&*()_+";
    std::string specialConfig = "{\"WIFI_SSID\":\"TestNetwork\",\"WIFI_PASS\":\"" + specialPass + 
                                "\",\"ENDPOINT\":\"//server/share\",\"ENDPOINT_PASS\":\"" + specialEndpoint + "\"}";
    
    mockSD.clear();
    mockSD.addFile("/config.json", specialConfig);
    
    Config config3;
    bool loaded3 = config3.loadFromSD(mockSD);
    TEST_ASSERT_TRUE(loaded3);
    TEST_ASSERT_EQUAL_STRING(specialPass.c_str(), config3.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING(specialEndpoint.c_str(), config3.getEndpointPassword().c_str());
}

// Test credential retrieval with non-existing keys
void test_config_credential_retrieval_missing_keys() {
    // Create config with censored credentials but no Preferences data
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "***STORED_IN_FLASH***",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "***STORED_IN_FLASH***"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    // Clear any existing Preferences data for this test
    Preferences::clearAll();
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    // Should still load but with empty credentials (fallback behavior)
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    TEST_ASSERT_TRUE(config.areCredentialsInFlash());
}

// Test empty credential handling
void test_config_empty_credentials() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": ""
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    // Empty credentials should be handled gracefully
    TEST_ASSERT_EQUAL_STRING("", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("", config.getEndpointPassword().c_str());
}

// Test switching from plain text to secure mode
void test_config_switch_plain_to_secure() {
    // First load with plain text mode
    std::string plainConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "PlainPassword",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "PlainEndpointPass",
        "STORE_CREDENTIALS_PLAIN_TEXT": true
    })";
    
    mockSD.addFile("/config.json", plainConfig);
    
    {
        Config config1;
        bool loaded1 = config1.loadFromSD(mockSD);
        TEST_ASSERT_TRUE_MESSAGE(loaded1, "Plain text config should load");
        TEST_ASSERT_TRUE_MESSAGE(config1.isStoringPlainText(), "Should be in plain text mode");
    }
    
    // Now switch to secure mode by changing the flag
    std::string secureConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "PlainPassword",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "PlainEndpointPass",
        "STORE_CREDENTIALS_PLAIN_TEXT": false
    })";
    
    mockSD.clear();
    mockSD.addFile("/config.json", secureConfig);
    
    Config config2;
    bool loaded2 = config2.loadFromSD(mockSD);
    TEST_ASSERT_TRUE_MESSAGE(loaded2, "Secure config should load");
    TEST_ASSERT_FALSE_MESSAGE(config2.isStoringPlainText(), "Should not be in plain text mode");
    TEST_ASSERT_TRUE_MESSAGE(config2.areCredentialsInFlash(), "Credentials should be in flash");
    
    // Credentials should be migrated to Preferences
    TEST_ASSERT_EQUAL_STRING("PlainPassword", config2.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("PlainEndpointPass", config2.getEndpointPassword().c_str());
}

// Test config file censoring accuracy
void test_config_censoring_accuracy() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "ShouldBeCensored",
        "ENDPOINT": "//server/share",
        "ENDPOINT_TYPE": "SMB",
        "ENDPOINT_USER": "testuser",
        "ENDPOINT_PASS": "AlsoCensored",
        "UPLOAD_HOUR": 12
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    TEST_ASSERT_TRUE_MESSAGE(loaded, "Config should load successfully");
    
    // Read back the config file
    std::vector<uint8_t> content = mockSD.getFileContent(String("/config.json"));
    std::string fileContent(content.begin(), content.end());
    
    // Verify credentials are censored
    TEST_ASSERT_TRUE_MESSAGE(fileContent.find("***STORED_IN_FLASH***") != std::string::npos, 
                             "Should contain censored placeholder");
    TEST_ASSERT_TRUE_MESSAGE(fileContent.find("ShouldBeCensored") == std::string::npos, 
                             "Should not contain original WiFi password");
    TEST_ASSERT_TRUE_MESSAGE(fileContent.find("AlsoCensored") == std::string::npos, 
                             "Should not contain original endpoint password");
    
    // Verify other fields are preserved
    TEST_ASSERT_TRUE_MESSAGE(fileContent.find("TestNetwork") != std::string::npos, 
                             "Should preserve SSID");
    TEST_ASSERT_TRUE_MESSAGE(fileContent.find("testuser") != std::string::npos, 
                             "Should preserve username");
    TEST_ASSERT_TRUE_MESSAGE(fileContent.find("SMB") != std::string::npos, 
                             "Should preserve endpoint type");
}

// Test multiple Config instances with Preferences
void test_config_multiple_instances() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "SharedPassword",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "SharedEndpointPass"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    // Create first config instance and let it migrate
    {
        Config config1;
        bool loaded1 = config1.loadFromSD(mockSD);
        TEST_ASSERT_TRUE(loaded1);
        TEST_ASSERT_EQUAL_STRING("SharedPassword", config1.getWifiPassword().c_str());
        // config1 destructor called here
    }
    
    // Create second config instance (should read from same Preferences)
    // The config file is now censored, so it should load from Preferences
    Config config2;
    bool loaded2 = config2.loadFromSD(mockSD);
    TEST_ASSERT_TRUE(loaded2);
    TEST_ASSERT_EQUAL_STRING("SharedPassword", config2.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("SharedEndpointPass", config2.getEndpointPassword().c_str());
}

// Test config with only WiFi password (no endpoint password)
void test_config_wifi_only_secure() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "WiFiOnlyPassword",
        "ENDPOINT": "//server/share"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    TEST_ASSERT_TRUE(config.areCredentialsInFlash());
    TEST_ASSERT_EQUAL_STRING("WiFiOnlyPassword", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("", config.getEndpointPassword().c_str());
}

// Test config with only endpoint password (no WiFi password)
void test_config_endpoint_only_secure() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "EndpointOnlyPassword"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    TEST_ASSERT_TRUE(config.areCredentialsInFlash());
    TEST_ASSERT_EQUAL_STRING("", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("EndpointOnlyPassword", config.getEndpointPassword().c_str());
}

// ============================================================================
// INDIVIDUAL CREDENTIAL UPDATE TESTS
// ============================================================================

// Test updating only WiFi password (endpoint remains censored)
void test_config_update_wifi_only() {
    // First, create initial config and let it migrate to secure storage
    std::string initialConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "OriginalWiFiPass",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "OriginalEndpointPass"
    })";
    
    mockSD.addFile("/config.json", initialConfig);
    
    {
        Config config1;
        bool loaded1 = config1.loadFromSD(mockSD);
        TEST_ASSERT_TRUE(loaded1);
        // Let migration happen and config file get censored
    }
    
    // Now simulate user updating only WiFi password in config.json
    std::string updatedConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "NewWiFiPassword123",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "***STORED_IN_FLASH***"
    })";
    
    mockSD.clear();
    mockSD.addFile("/config.json", updatedConfig);
    
    Config config2;
    bool loaded2 = config2.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE_MESSAGE(loaded2, "Config should load successfully");
    TEST_ASSERT_TRUE_MESSAGE(config2.valid(), "Config should be valid");
    
    // Should use new WiFi password from config, stored endpoint password from flash
    TEST_ASSERT_EQUAL_STRING("NewWiFiPassword123", config2.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("OriginalEndpointPass", config2.getEndpointPassword().c_str());
    TEST_ASSERT_TRUE_MESSAGE(config2.areCredentialsInFlash(), "Should have credentials in flash");
}

// Test updating only endpoint password (WiFi remains censored)
void test_config_update_endpoint_only() {
    // First, create initial config and let it migrate to secure storage
    std::string initialConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "OriginalWiFiPass",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "OriginalEndpointPass"
    })";
    
    mockSD.addFile("/config.json", initialConfig);
    
    {
        Config config1;
        bool loaded1 = config1.loadFromSD(mockSD);
        TEST_ASSERT_TRUE(loaded1);
        // Let migration happen and config file get censored
    }
    
    // Now simulate user updating only endpoint password in config.json
    std::string updatedConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "***STORED_IN_FLASH***",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "NewEndpointPassword456"
    })";
    
    mockSD.clear();
    mockSD.addFile("/config.json", updatedConfig);
    
    Config config2;
    bool loaded2 = config2.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE_MESSAGE(loaded2, "Config should load successfully");
    TEST_ASSERT_TRUE_MESSAGE(config2.valid(), "Config should be valid");
    
    // Should use stored WiFi password from flash, new endpoint password from config
    TEST_ASSERT_EQUAL_STRING("OriginalWiFiPass", config2.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("NewEndpointPassword456", config2.getEndpointPassword().c_str());
    TEST_ASSERT_TRUE_MESSAGE(config2.areCredentialsInFlash(), "Should have credentials in flash");
}

// Test updating both credentials (both plain text in config)
void test_config_update_both_credentials() {
    // First, create initial config and let it migrate to secure storage
    std::string initialConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "OriginalWiFiPass",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "OriginalEndpointPass"
    })";
    
    mockSD.addFile("/config.json", initialConfig);
    
    {
        Config config1;
        bool loaded1 = config1.loadFromSD(mockSD);
        TEST_ASSERT_TRUE(loaded1);
        // Let migration happen and config file get censored
    }
    
    // Now simulate user updating both passwords in config.json
    std::string updatedConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "NewWiFiPassword123",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "NewEndpointPassword456"
    })";
    
    mockSD.clear();
    mockSD.addFile("/config.json", updatedConfig);
    
    Config config2;
    bool loaded2 = config2.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE_MESSAGE(loaded2, "Config should load successfully");
    TEST_ASSERT_TRUE_MESSAGE(config2.valid(), "Config should be valid");
    
    // Should use both new passwords from config
    TEST_ASSERT_EQUAL_STRING("NewWiFiPassword123", config2.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("NewEndpointPassword456", config2.getEndpointPassword().c_str());
    TEST_ASSERT_TRUE_MESSAGE(config2.areCredentialsInFlash(), "Should have credentials in flash after migration");
}

// Test mixed state: WiFi password new, endpoint censored (starting from fresh)
void test_config_mixed_state_wifi_new() {
    // Pre-populate Preferences with endpoint password
    {
        Preferences prefs;
        prefs.begin("cpap_creds", false);
        prefs.putString("endpoint_pass", "StoredEndpointPass");
        prefs.end();
    }
    
    std::string mixedConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "NewWiFiPassword",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "***STORED_IN_FLASH***"
    })";
    
    mockSD.addFile("/config.json", mixedConfig);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE_MESSAGE(loaded, "Config should load successfully");
    TEST_ASSERT_TRUE_MESSAGE(config.valid(), "Config should be valid");
    
    // Should use new WiFi password from config, stored endpoint password from flash
    TEST_ASSERT_EQUAL_STRING("NewWiFiPassword", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("StoredEndpointPass", config.getEndpointPassword().c_str());
    TEST_ASSERT_TRUE_MESSAGE(config.areCredentialsInFlash(), "Should have credentials in flash");
}

// Test mixed state: endpoint password new, WiFi censored (starting from fresh)
void test_config_mixed_state_endpoint_new() {
    // Pre-populate Preferences with WiFi password
    {
        Preferences prefs;
        prefs.begin("cpap_creds", false);
        prefs.putString("wifi_pass", "StoredWiFiPass");
        prefs.end();
    }
    
    std::string mixedConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "***STORED_IN_FLASH***",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "NewEndpointPassword"
    })";
    
    mockSD.addFile("/config.json", mixedConfig);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE_MESSAGE(loaded, "Config should load successfully");
    TEST_ASSERT_TRUE_MESSAGE(config.valid(), "Config should be valid");
    
    // Should use stored WiFi password from flash, new endpoint password from config
    TEST_ASSERT_EQUAL_STRING("StoredWiFiPass", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("NewEndpointPassword", config.getEndpointPassword().c_str());
    TEST_ASSERT_TRUE_MESSAGE(config.areCredentialsInFlash(), "Should have credentials in flash");
}

// Test mixed state: both passwords new (overriding stored ones)
void test_config_mixed_state_both_new() {
    // Pre-populate Preferences with old passwords (should be overridden)
    {
        Preferences prefs;
        prefs.begin("cpap_creds", false);
        prefs.putString("wifi_pass", "OldStoredWiFiPass");
        prefs.putString("endpoint_pass", "OldStoredEndpointPass");
        prefs.end();
    }
    
    std::string mixedConfig = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "NewWiFiPassword",
        "ENDPOINT": "//server/share",
        "ENDPOINT_PASS": "NewEndpointPassword"
    })";
    
    mockSD.addFile("/config.json", mixedConfig);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE_MESSAGE(loaded, "Config should load successfully");
    TEST_ASSERT_TRUE_MESSAGE(config.valid(), "Config should be valid");
    
    // Should use new passwords from config (prioritized over stored ones)
    TEST_ASSERT_EQUAL_STRING("NewWiFiPassword", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("NewEndpointPassword", config.getEndpointPassword().c_str());
    TEST_ASSERT_TRUE_MESSAGE(config.areCredentialsInFlash(), "Should have credentials in flash after migration");
}

// Test power management configuration with default values
void test_config_power_management_defaults() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "password",
        "ENDPOINT": "//server/share",
        "ENDPOINT_TYPE": "SMB",
        "ENDPOINT_USER": "user",
        "ENDPOINT_PASS": "pass"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    
    // Test default power management values
    TEST_ASSERT_EQUAL(240, config.getCpuSpeedMhz());
    TEST_ASSERT_EQUAL(WifiTxPower::POWER_HIGH, config.getWifiTxPower());
    TEST_ASSERT_EQUAL(WifiPowerSaving::SAVE_NONE, config.getWifiPowerSaving());
}

// Test power management configuration with custom values
void test_config_power_management_custom() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "password",
        "ENDPOINT": "//server/share",
        "ENDPOINT_TYPE": "SMB",
        "ENDPOINT_USER": "user",
        "ENDPOINT_PASS": "pass",
        "CPU_SPEED_MHZ": 160,
        "WIFI_TX_PWR": "mid",
        "WIFI_PWR_SAVING": "max"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    
    // Test custom power management values
    TEST_ASSERT_EQUAL(160, config.getCpuSpeedMhz());
    TEST_ASSERT_EQUAL(WifiTxPower::POWER_MID, config.getWifiTxPower());
    TEST_ASSERT_EQUAL(WifiPowerSaving::SAVE_MAX, config.getWifiPowerSaving());
}

// Test power management configuration with case insensitive values
void test_config_power_management_case_insensitive() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "password",
        "ENDPOINT": "//server/share",
        "ENDPOINT_TYPE": "SMB",
        "ENDPOINT_USER": "user",
        "ENDPOINT_PASS": "pass",
        "CPU_SPEED_MHZ": 80,
        "WIFI_TX_PWR": "LOW",
        "WIFI_PWR_SAVING": "MID"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    
    // Test case insensitive parsing
    TEST_ASSERT_EQUAL(80, config.getCpuSpeedMhz());
    TEST_ASSERT_EQUAL(WifiTxPower::POWER_LOW, config.getWifiTxPower());
    TEST_ASSERT_EQUAL(WifiPowerSaving::SAVE_MID, config.getWifiPowerSaving());
}

// Test power management configuration with invalid values (should use defaults)
void test_config_power_management_invalid_values() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "password",
        "ENDPOINT": "//server/share",
        "ENDPOINT_TYPE": "SMB",
        "ENDPOINT_USER": "user",
        "ENDPOINT_PASS": "pass",
        "CPU_SPEED_MHZ": 50,
        "WIFI_TX_PWR": "invalid",
        "WIFI_PWR_SAVING": "unknown"
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    
    // Test validation and fallback to defaults
    TEST_ASSERT_EQUAL(80, config.getCpuSpeedMhz());  // Should be clamped to minimum
    TEST_ASSERT_EQUAL(WifiTxPower::POWER_HIGH, config.getWifiTxPower());  // Should fallback to default
    TEST_ASSERT_EQUAL(WifiPowerSaving::SAVE_NONE, config.getWifiPowerSaving());  // Should fallback to default
}

// Test power management configuration with boundary values
void test_config_power_management_boundaries() {
    std::string configContent = R"({
        "WIFI_SSID": "TestNetwork",
        "WIFI_PASS": "password",
        "ENDPOINT": "//server/share",
        "ENDPOINT_TYPE": "SMB",
        "ENDPOINT_USER": "user",
        "ENDPOINT_PASS": "pass",
        "CPU_SPEED_MHZ": 300
    })";
    
    mockSD.addFile("/config.json", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.valid());
    
    // Test boundary validation (should be clamped to maximum)
    TEST_ASSERT_EQUAL(240, config.getCpuSpeedMhz());
}

// Test that the default config.json.example fits within JSON_FILE_MAX_SIZE
void test_config_default_example_fits_in_buffer() {
    // This is the default config from docs/config.json.example
    std::string defaultConfig = R"({
  "WIFI_SSID": "YourNetworkName",
  "WIFI_PASS": "YourNetworkPassword",

  "ENDPOINT": "//192.168.1.100/cpap_backups",
  "ENDPOINT_TYPE": "SMB",
  "ENDPOINT_USER": "username",
  "ENDPOINT_PASS": "password",
  "UPLOAD_HOUR": 12,

  "SESSION_DURATION_SECONDS": 30,
  "MAX_RETRY_ATTEMPTS": 3,
  "_comment_timezone_1": "GMT_OFFSET_HOURS: Offset from GMT in hours. Examples: PST=-8, EST=-5, UTC=0, CET=+1, JST=+9",
  "GMT_OFFSET_HOURS": 0,

  "LOG_TO_SD_CARD": false,

  "CPU_SPEED_MHZ": 240,
  "WIFI_TX_PWR": "high",
  "WIFI_PWR_SAVING": "none"
})";
    
    // Verify the default config size is well within JSON_FILE_MAX_SIZE
    size_t configSize = defaultConfig.length();
    
    TEST_ASSERT_LESS_THAN_MESSAGE(Config::JSON_FILE_MAX_SIZE, configSize, 
        "Default config.json.example must fit within JSON_FILE_MAX_SIZE buffer");
    
    // Also verify it can be parsed successfully
    mockSD.addFile("/config.json", defaultConfig);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE_MESSAGE(loaded, "Default config should load successfully");
    TEST_ASSERT_TRUE_MESSAGE(config.valid(), "Default config should be valid");
    
    // Verify some key default values are parsed correctly
    TEST_ASSERT_EQUAL_STRING("YourNetworkName", config.getWifiSSID().c_str());
    TEST_ASSERT_EQUAL_STRING("//192.168.1.100/cpap_backups", config.getEndpoint().c_str());
    TEST_ASSERT_EQUAL_STRING("SMB", config.getEndpointType().c_str());
    TEST_ASSERT_EQUAL(12, config.getUploadHour());
    TEST_ASSERT_EQUAL(30, config.getSessionDurationSeconds());
    TEST_ASSERT_EQUAL(3, config.getMaxRetryAttempts());
    TEST_ASSERT_EQUAL(0, config.getGmtOffsetHours());
    TEST_ASSERT_FALSE(config.getLogToSdCard());
    TEST_ASSERT_EQUAL(240, config.getCpuSpeedMhz());
    TEST_ASSERT_EQUAL(WifiTxPower::POWER_HIGH, config.getWifiTxPower());
    TEST_ASSERT_EQUAL(WifiPowerSaving::SAVE_NONE, config.getWifiPowerSaving());
}

// Test worst-case config with maximum-length strings (128 chars for endpoint and password)
void test_config_worst_case_max_size() {
    // Create maximum-length strings (128 characters each)
    std::string maxEndpoint(128, 'E');  // 128 'E' characters
    std::string maxEndpointPass(128, 'P');  // 128 'P' characters
    std::string maxWifiSSID(32, 'W');  // WiFi SSID max is 32 chars
    std::string maxWifiPass(128, 'X');  // 128 'X' characters
    std::string maxEndpointUser(128, 'U');  // 128 'U' characters
    std::string maxSchedule(128, 'S');  // 128 'S' characters
    
    // Build worst-case config with all fields at maximum length
    std::string worstCaseConfig = 
        "{\n"
        "  \"WIFI_SSID\": \"" + maxWifiSSID + "\",\n"
        "  \"WIFI_PASS\": \"" + maxWifiPass + "\",\n"
        "  \"SCHEDULE\": \"" + maxSchedule + "\",\n"
        "  \"ENDPOINT\": \"" + maxEndpoint + "\",\n"
        "  \"ENDPOINT_TYPE\": \"WEBDAV\",\n"
        "  \"ENDPOINT_USER\": \"" + maxEndpointUser + "\",\n"
        "  \"ENDPOINT_PASS\": \"" + maxEndpointPass + "\",\n"
        "  \"UPLOAD_HOUR\": 23,\n"
        "  \"SESSION_DURATION_SECONDS\": 300,\n"
        "  \"MAX_RETRY_ATTEMPTS\": 10,\n"
        "  \"_comment_timezone_1\": \"GMT_OFFSET_HOURS: Offset from GMT in hours. Examples: PST=-8, EST=-5, UTC=0, CET=+1, JST=+9\",\n"
        "  \"GMT_OFFSET_HOURS\": -12,\n"
        "  \"BOOT_DELAY_SECONDS\": 120,\n"
        "  \"SD_RELEASE_INTERVAL_SECONDS\": 10,\n"
        "  \"SD_RELEASE_WAIT_MS\": 2000,\n"
        "  \"LOG_TO_SD_CARD\": true,\n"
        "  \"CPU_SPEED_MHZ\": 160,\n"
        "  \"WIFI_TX_PWR\": \"low\",\n"
        "  \"WIFI_PWR_SAVING\": \"max\",\n"
        "  \"STORE_CREDENTIALS_PLAIN_TEXT\": true\n"
        "}";
    
    size_t worstCaseSize = worstCaseConfig.length();
    
    // Verify worst-case config fits within JSON_FILE_MAX_SIZE
    TEST_ASSERT_LESS_THAN_MESSAGE(Config::JSON_FILE_MAX_SIZE, worstCaseSize,
        "Worst-case config with 128-char strings must fit within JSON_FILE_MAX_SIZE buffer");
    
    // Also verify it can be parsed and loaded successfully
    mockSD.addFile("/config.json", worstCaseConfig);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE_MESSAGE(loaded, "Worst-case config should load successfully");
    TEST_ASSERT_TRUE_MESSAGE(config.valid(), "Worst-case config should be valid");
    
    // Verify the maximum-length values are preserved
    TEST_ASSERT_EQUAL_STRING(maxWifiSSID.c_str(), config.getWifiSSID().c_str());
    TEST_ASSERT_EQUAL_STRING(maxWifiPass.c_str(), config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING(maxSchedule.c_str(), config.getSchedule().c_str());
    TEST_ASSERT_EQUAL_STRING(maxEndpoint.c_str(), config.getEndpoint().c_str());
    TEST_ASSERT_EQUAL_STRING(maxEndpointUser.c_str(), config.getEndpointUser().c_str());
    TEST_ASSERT_EQUAL_STRING(maxEndpointPass.c_str(), config.getEndpointPassword().c_str());
    
    // Log the actual size for reference
    char sizeMsg[128];
    snprintf(sizeMsg, sizeof(sizeMsg), "Worst-case config size: %zu bytes (limit: %zu bytes, margin: %zu bytes)",
             worstCaseSize, Config::JSON_FILE_MAX_SIZE, Config::JSON_FILE_MAX_SIZE - worstCaseSize);
    TEST_MESSAGE(sizeMsg);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Basic loading tests
    RUN_TEST(test_config_load_valid);
    RUN_TEST(test_config_load_with_defaults);
    RUN_TEST(test_config_load_missing_ssid);
    RUN_TEST(test_config_load_missing_endpoint);
    RUN_TEST(test_config_load_file_not_found);
    RUN_TEST(test_config_load_invalid_json);
    RUN_TEST(test_config_load_empty_file);
    
    // Endpoint type tests
    RUN_TEST(test_config_webdav_endpoint);
    RUN_TEST(test_config_sleephq_endpoint);
    
    // Configuration value tests
    RUN_TEST(test_config_negative_gmt_offset);
    RUN_TEST(test_config_positive_gmt_offset);
    RUN_TEST(test_config_upload_hours);
    RUN_TEST(test_config_long_session_duration);
    RUN_TEST(test_config_high_retry_attempts);
    RUN_TEST(test_config_boot_delay_and_sd_release);
    RUN_TEST(test_config_all_timing_fields);
    
    // Credential security tests (Preferences-based)
    RUN_TEST(test_config_plain_text_mode);
    RUN_TEST(test_config_secure_mode_migration);
    RUN_TEST(test_config_secure_mode_already_censored);
    RUN_TEST(test_config_credential_storage_various_lengths);
    RUN_TEST(test_config_credential_retrieval_missing_keys);
    RUN_TEST(test_config_empty_credentials);
    RUN_TEST(test_config_switch_plain_to_secure);
    RUN_TEST(test_config_censoring_accuracy);
    RUN_TEST(test_config_multiple_instances);
    RUN_TEST(test_config_wifi_only_secure);
    RUN_TEST(test_config_endpoint_only_secure);
    
    // Individual credential update tests
    RUN_TEST(test_config_update_wifi_only);
    RUN_TEST(test_config_update_endpoint_only);
    RUN_TEST(test_config_update_both_credentials);
    RUN_TEST(test_config_mixed_state_wifi_new);
    RUN_TEST(test_config_mixed_state_endpoint_new);
    RUN_TEST(test_config_mixed_state_both_new);
    
    // Power management tests
    RUN_TEST(test_config_power_management_defaults);
    RUN_TEST(test_config_power_management_custom);
    RUN_TEST(test_config_power_management_case_insensitive);
    RUN_TEST(test_config_power_management_invalid_values);
    RUN_TEST(test_config_power_management_boundaries);
    
    // Config size validation tests
    RUN_TEST(test_config_default_example_fits_in_buffer);
    RUN_TEST(test_config_worst_case_max_size);
    
    return UNITY_END();
}
