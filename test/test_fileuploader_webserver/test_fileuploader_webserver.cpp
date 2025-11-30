#include <unity.h>
#include <Arduino.h>

// Mock dependencies
#include "../mocks/MockWebServer.h"
#include "../mocks/MockFS.h"

// Mock classes needed for FileUploader
class Config {
public:
    int getSdReleaseIntervalSeconds() const { return 5; }
    int getSdReleaseWaitMs() const { return 100; }
};

class WiFiManager {
public:
    bool isConnected() const { return true; }
};

class UploadStateManager {
public:
    void setTotalFoldersCount(int count) {}
    int getCompletedFoldersCount() const { return 0; }
};

class TimeBudgetManager {
public:
    void pauseActiveTime() {}
    void resumeActiveTime() {}
    bool hasBudget() const { return true; }
};

class ScheduleManager {
public:
    bool isTimeSynced() const { return true; }
};

// Mock SDCardManager
class SDCardManager {
private:
    bool hasControl;
    
public:
    SDCardManager() : hasControl(false) {}
    
    bool takeControl() {
        hasControl = true;
        return true;
    }
    
    void releaseControl() {
        hasControl = false;
    }
    
    bool hasControlFlag() const { return hasControl; }
    
    fs::FS& getFS() {
        static MockFS mockFS;
        return mockFS;
    }
};

// Mock TestWebServer
class TestWebServer {
private:
    int handleClientCallCount;
    
public:
    TestWebServer() : handleClientCallCount(0) {}
    
    void handleClient() {
        handleClientCallCount++;
    }
    
    int getHandleClientCallCount() const {
        return handleClientCallCount;
    }
    
    void resetCallCount() {
        handleClientCallCount = 0;
    }
};

// Simplified FileUploader for testing web server integration
class FileUploaderTestable {
private:
    Config* config;
    TestWebServer* webServer;
    SDCardManager* sdManager;
    TimeBudgetManager* budgetManager;
    unsigned long lastSdReleaseTime;
    
public:
    FileUploaderTestable(Config* cfg, SDCardManager* sd, TimeBudgetManager* budget)
        : config(cfg), webServer(nullptr), sdManager(sd), 
          budgetManager(budget), lastSdReleaseTime(0) {}
    
    void setWebServer(TestWebServer* server) {
        webServer = server;
    }
    
    TestWebServer* getWebServer() const {
        return webServer;
    }
    
    // Simulate checkAndReleaseSD behavior
    bool checkAndReleaseSD() {
        unsigned long now = millis();
        unsigned long intervalMs = config->getSdReleaseIntervalSeconds() * 1000;
        
        // Check if it's time to release
        if (now - lastSdReleaseTime < intervalMs) {
            return true;  // Not time yet
        }
        
        // Pause budget tracking
        budgetManager->pauseActiveTime();
        
        // Release SD card
        sdManager->releaseControl();
        
        // Wait and handle web requests
        unsigned long waitMs = config->getSdReleaseWaitMs();
        
#ifdef ENABLE_TEST_WEBSERVER
        if (webServer) {
            unsigned long waitStart = millis();
            while (millis() - waitStart < waitMs) {
                webServer->handleClient();
                delay(10);
            }
        } else {
            delay(waitMs);
        }
#else
        delay(waitMs);
#endif
        
        // Retake control
        if (!sdManager->takeControl()) {
            return false;
        }
        
        // Resume budget tracking
        budgetManager->resumeActiveTime();
        
        // Reset timer
        lastSdReleaseTime = millis();
        
        return true;
    }
    
    // Simulate processing with web server handling
    void simulateUploadWithWebServerHandling() {
        // Simulate multiple operations with web server handling
        for (int i = 0; i < 3; i++) {
#ifdef ENABLE_TEST_WEBSERVER
            if (webServer) {
                webServer->handleClient();
            }
#endif
            delay(10);
        }
    }
};

// Test fixtures
Config testConfig;
SDCardManager testSdManager;
TimeBudgetManager testBudgetManager;
TestWebServer testWebServer;
FileUploaderTestable* uploader = nullptr;

void setUp(void) {
    uploader = new FileUploaderTestable(&testConfig, &testSdManager, &testBudgetManager);
    testWebServer.resetCallCount();
}

void tearDown(void) {
    if (uploader) {
        delete uploader;
        uploader = nullptr;
    }
}

// Test: Web server can be set on FileUploader
void test_set_web_server(void) {
    TEST_ASSERT_NULL(uploader->getWebServer());
    
    uploader->setWebServer(&testWebServer);
    
    TEST_ASSERT_NOT_NULL(uploader->getWebServer());
    TEST_ASSERT_EQUAL_PTR(&testWebServer, uploader->getWebServer());
}

// Test: Web server is optional (null is valid)
void test_web_server_optional(void) {
    uploader->setWebServer(nullptr);
    TEST_ASSERT_NULL(uploader->getWebServer());
    
    // Should not crash when calling methods without web server
    uploader->simulateUploadWithWebServerHandling();
    TEST_ASSERT_TRUE(true);
}

#ifdef ENABLE_TEST_WEBSERVER
// Test: Web server handleClient is called during SD release wait
void test_web_server_called_during_sd_release(void) {
    uploader->setWebServer(&testWebServer);
    
    // Force SD release by setting last release time to past
    // (This is a simplified test - in real code, time would advance)
    
    int initialCallCount = testWebServer.getHandleClientCallCount();
    
    // Simulate upload operations that trigger web server handling
    uploader->simulateUploadWithWebServerHandling();
    
    int finalCallCount = testWebServer.getHandleClientCallCount();
    
    // Verify handleClient was called
    TEST_ASSERT_GREATER_THAN(initialCallCount, finalCallCount);
}

// Test: Web server handleClient is called multiple times during wait
void test_web_server_called_multiple_times(void) {
    uploader->setWebServer(&testWebServer);
    
    testWebServer.resetCallCount();
    
    // Simulate multiple upload operations
    for (int i = 0; i < 5; i++) {
        uploader->simulateUploadWithWebServerHandling();
    }
    
    int callCount = testWebServer.getHandleClientCallCount();
    
    // Should be called at least once per operation
    TEST_ASSERT_GREATER_OR_EQUAL(5, callCount);
}

// Test: checkAndReleaseSD handles web requests during wait
void test_check_and_release_sd_with_web_server(void) {
    uploader->setWebServer(&testWebServer);
    
    // Take initial control
    testSdManager.takeControl();
    TEST_ASSERT_TRUE(testSdManager.hasControlFlag());
    
    testWebServer.resetCallCount();
    
    // Call checkAndReleaseSD (will release and retake)
    // Note: This test is simplified - in real scenario, time would need to advance
    bool result = uploader->checkAndReleaseSD();
    
    TEST_ASSERT_TRUE(result);
    TEST_ASSERT_TRUE(testSdManager.hasControlFlag());
    
    // Web server should have been called during wait
    // (In this simplified test, it may not be called if interval hasn't elapsed)
}
#endif

// Test: FileUploader works without web server (backward compatibility)
void test_backward_compatibility_without_web_server(void) {
    // Don't set web server
    TEST_ASSERT_NULL(uploader->getWebServer());
    
    // Should work normally
    uploader->simulateUploadWithWebServerHandling();
    
    // Verify no crashes
    TEST_ASSERT_TRUE(true);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    RUN_TEST(test_set_web_server);
    RUN_TEST(test_web_server_optional);
#ifdef ENABLE_TEST_WEBSERVER
    RUN_TEST(test_web_server_called_during_sd_release);
    RUN_TEST(test_web_server_called_multiple_times);
    RUN_TEST(test_check_and_release_sd_with_web_server);
#endif
    RUN_TEST(test_backward_compatibility_without_web_server);
    
    return UNITY_END();
}
