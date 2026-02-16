#include <unity.h>
#include "Arduino.h"
#include "FS.h"

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
    mockSD.clear();
    Preferences::clearAll();
}

void tearDown(void) {
    mockSD.clear();
    Preferences::clearAll();
}

// Test: Plain text credentials trigger migration to secure storage
void test_migration_plain_to_secure() {
    std::string configContent = 
        "WIFI_SSID = TestNetwork\n"
        "WIFI_PASSWORD = MyWifiPass123\n"
        "ENDPOINT = //server/share\n"
        "ENDPOINT_PASSWORD = MyEndpointPass456\n";
    
    mockSD.addFile("/config.txt", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.areCredentialsInFlash());
    TEST_ASSERT_EQUAL_STRING("MyWifiPass123", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("MyEndpointPass456", config.getEndpointPassword().c_str());
    
    // Verify config.txt was censored
    std::vector<uint8_t> updatedBytes = mockSD.getFileContent("/config.txt");
    std::string updatedConfig(updatedBytes.begin(), updatedBytes.end());
    TEST_ASSERT_TRUE(updatedConfig.find("***STORED_IN_FLASH***") != std::string::npos);
    TEST_ASSERT_TRUE(updatedConfig.find("MyWifiPass123") == std::string::npos);
    TEST_ASSERT_TRUE(updatedConfig.find("MyEndpointPass456") == std::string::npos);
}

// Test: Already censored credentials load from Preferences
void test_migration_already_migrated() {
    // Pre-populate Preferences with credentials
    Preferences prefs;
    prefs.begin("cpap_creds", false);
    prefs.putString("wifi_pass", "StoredWifiPass");
    prefs.putString("endpoint_pass", "StoredEndpointPass");
    prefs.end();
    
    std::string configContent = 
        "WIFI_SSID = TestNetwork\n"
        "WIFI_PASSWORD = ***STORED_IN_FLASH***\n"
        "ENDPOINT = //server/share\n"
        "ENDPOINT_PASSWORD = ***STORED_IN_FLASH***\n";
    
    mockSD.addFile("/config.txt", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.areCredentialsInFlash());
    TEST_ASSERT_EQUAL_STRING("StoredWifiPass", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("StoredEndpointPass", config.getEndpointPassword().c_str());
}

// Test: Plain text mode bypasses migration
void test_migration_plain_text_mode() {
    std::string configContent = 
        "WIFI_SSID = TestNetwork\n"
        "WIFI_PASSWORD = PlainWifiPass\n"
        "ENDPOINT = //server/share\n"
        "ENDPOINT_PASSWORD = PlainEndpointPass\n"
        "STORE_CREDENTIALS_PLAIN_TEXT = true\n";
    
    mockSD.addFile("/config.txt", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_FALSE(config.areCredentialsInFlash());
    TEST_ASSERT_TRUE(config.isStoringPlainText());
    TEST_ASSERT_EQUAL_STRING("PlainWifiPass", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("PlainEndpointPass", config.getEndpointPassword().c_str());
    
    // Verify config.txt was NOT censored
    std::vector<uint8_t> updatedBytes = mockSD.getFileContent("/config.txt");
    std::string updatedConfig(updatedBytes.begin(), updatedBytes.end());
    TEST_ASSERT_TRUE(updatedConfig.find("PlainWifiPass") != std::string::npos);
    TEST_ASSERT_TRUE(updatedConfig.find("PlainEndpointPass") != std::string::npos);
}

// Test: Empty credentials skip migration
void test_migration_empty_credentials() {
    std::string configContent = 
        "WIFI_SSID = TestNetwork\n"
        "WIFI_PASSWORD = \n"
        "ENDPOINT = //server/share\n"
        "ENDPOINT_PASSWORD = \n";
    
    mockSD.addFile("/config.txt", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_FALSE(config.areCredentialsInFlash());
    TEST_ASSERT_EQUAL_STRING("", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("", config.getEndpointPassword().c_str());
}

// Test: Credential persistence across Config instances
void test_migration_persistence() {
    std::string configContent = 
        "WIFI_SSID = TestNetwork\n"
        "WIFI_PASSWORD = PersistentPass123\n"
        "ENDPOINT = //server/share\n"
        "ENDPOINT_PASSWORD = PersistentEndpoint456\n";
    
    mockSD.addFile("/config.txt", configContent);
    
    // First instance triggers migration
    {
        Config config1;
        bool loaded = config1.loadFromSD(mockSD);
        TEST_ASSERT_TRUE(loaded);
        TEST_ASSERT_TRUE(config1.areCredentialsInFlash());
    }
    
    // Second instance loads from Preferences
    {
        Config config2;
        bool loaded = config2.loadFromSD(mockSD);
        TEST_ASSERT_TRUE(loaded);
        TEST_ASSERT_TRUE(config2.areCredentialsInFlash());
        TEST_ASSERT_EQUAL_STRING("PersistentPass123", config2.getWifiPassword().c_str());
        TEST_ASSERT_EQUAL_STRING("PersistentEndpoint456", config2.getEndpointPassword().c_str());
    }
}

// Test: Mixed credential state (one censored, one not)
void test_migration_mixed_state() {
    // Pre-populate Preferences with WiFi password only
    Preferences prefs;
    prefs.begin("cpap_creds", false);
    prefs.putString("wifi_pass", "StoredWifiPass");
    prefs.end();
    
    std::string configContent = 
        "WIFI_SSID = TestNetwork\n"
        "WIFI_PASSWORD = ***STORED_IN_FLASH***\n"
        "ENDPOINT = //server/share\n"
        "ENDPOINT_PASSWORD = PlainEndpointPass\n";
    
    mockSD.addFile("/config.txt", configContent);
    
    Config config;
    bool loaded = config.loadFromSD(mockSD);
    
    TEST_ASSERT_TRUE(loaded);
    TEST_ASSERT_TRUE(config.areCredentialsInFlash());
    TEST_ASSERT_EQUAL_STRING("StoredWifiPass", config.getWifiPassword().c_str());
    TEST_ASSERT_EQUAL_STRING("PlainEndpointPass", config.getEndpointPassword().c_str());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    RUN_TEST(test_migration_plain_to_secure);
    RUN_TEST(test_migration_already_migrated);
    RUN_TEST(test_migration_plain_text_mode);
    RUN_TEST(test_migration_empty_credentials);
    RUN_TEST(test_migration_persistence);
    RUN_TEST(test_migration_mixed_state);
    
    UNITY_END();
    
    return 0;
}
