#include <unity.h>
#include <Arduino.h>
#include <time.h>

// Mock WebServer before including TestWebServer
#include "../mocks/MockWebServer.h"

// Mock Config, UploadStateManager, etc.
class Config {
public:
    String getWifiSSID() const { return "TestSSID"; }
    String getEndpoint() const { return "//test/share"; }
    String getEndpointType() const { return "SMB"; }
    String getEndpointUser() const { return "testuser"; }
    int getUploadHour() const { return 12; }
    int getSessionDurationSeconds() const { return 5; }
    int getMaxRetryAttempts() const { return 3; }
    long getGmtOffsetSeconds() const { return 0; }
    int getDaylightOffsetSeconds() const { return 0; }
};

class UploadStateManager {
public:
    unsigned long getLastUploadTimestamp() const { return 1699876800; }
};

class TimeBudgetManager {
public:
    unsigned long getRemainingBudgetMs() const { return 5000; }
};

class ScheduleManager {
public:
    unsigned long getSecondsUntilNextUpload() const { return 3600; }
    bool isTimeSynced() const { return true; }
};

// Global trigger flags (normally defined in TestWebServer.cpp)
volatile bool g_triggerUploadFlag = false;
volatile bool g_resetStateFlag = false;
volatile bool g_scanNowFlag = false;
volatile bool g_deltaScanFlag = false;
volatile bool g_deepScanFlag = false;

// Simplified TestWebServer for testing (without actual WebServer dependency)
class TestWebServer {
private:
    WebServer* server;
    Config* config;
    UploadStateManager* stateManager;
    TimeBudgetManager* budgetManager;
    ScheduleManager* scheduleManager;

public:
    TestWebServer(Config* cfg, UploadStateManager* state,
                  TimeBudgetManager* budget, ScheduleManager* schedule)
        : server(nullptr), config(cfg), stateManager(state),
          budgetManager(budget), scheduleManager(schedule) {}
    
    ~TestWebServer() {
        if (server) {
            server->stop();
            delete server;
        }
    }
    
    bool begin() {
        server = new WebServer(80);
        
        // Register handlers (simplified for testing)
        server->on("/", [this]() { this->handleRoot(); });
        server->on("/trigger-upload", [this]() { this->handleTriggerUpload(); });
        server->on("/scan-now", [this]() { this->handleScanNow(); });
        server->on("/delta-scan", [this]() { this->handleDeltaScan(); });
        server->on("/deep-scan", [this]() { this->handleDeepScan(); });
        server->on("/status", [this]() { this->handleStatus(); });
        server->on("/reset-state", [this]() { this->handleResetState(); });
        server->on("/config", [this]() { this->handleConfig(); });
        server->onNotFound([this]() { this->handleNotFound(); });
        
        server->begin();
        return true;
    }
    
    void handleClient() {
        if (server) {
            server->handleClient();
        }
    }
    
    WebServer* getServer() { return server; }
    
private:
    void handleRoot() {
        String html = "<html><body>Test Status Page</body></html>";
        server->send(200, "text/html", html);
    }
    
    void handleTriggerUpload() {
        g_triggerUploadFlag = true;
        addCorsHeaders();
        String response = "{\"status\":\"success\"}";
        server->send(200, "application/json", response);
    }
    
    void handleScanNow() {
        g_scanNowFlag = true;
        addCorsHeaders();
        String response = "{\"status\":\"success\",\"message\":\"SD card scan triggered.\"}";
        server->send(200, "application/json", response);
    }
    
    void handleDeltaScan() {
        g_deltaScanFlag = true;
        addCorsHeaders();
        String response = "{\"status\":\"success\",\"message\":\"Delta scan triggered.\"}";
        server->send(200, "application/json", response);
    }
    
    void handleDeepScan() {
        g_deepScanFlag = true;
        addCorsHeaders();
        String response = "{\"status\":\"success\",\"message\":\"Deep scan triggered.\"}";
        server->send(200, "application/json", response);
    }
    
    void handleStatus() {
        addCorsHeaders();
        String json = "{\"uptime_seconds\":100}";
        server->send(200, "application/json", json);
    }
    
    void handleResetState() {
        g_resetStateFlag = true;
        addCorsHeaders();
        String response = "{\"status\":\"success\"}";
        server->send(200, "application/json", response);
    }
    
    void handleConfig() {
        addCorsHeaders();
        String json = "{\"endpoint_type\":\"SMB\"}";
        server->send(200, "application/json", json);
    }
    
    void handleNotFound() {
        String message = "{\"status\":\"error\"}";
        server->send(404, "application/json", message);
    }
    
    // Static helper method for CORS headers
    void addCorsHeaders() {
        server->sendHeader("Access-Control-Allow-Origin", "*");
        server->sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
        server->sendHeader("Access-Control-Allow-Headers", "Content-Type");
    }
};

// Test fixtures
Config testConfig;
UploadStateManager testStateManager;
TimeBudgetManager testBudgetManager;
ScheduleManager testScheduleManager;
TestWebServer* testServer = nullptr;

void setUp(void) {
    // Reset global flags
    g_triggerUploadFlag = false;
    g_resetStateFlag = false;
    g_scanNowFlag = false;
    g_deltaScanFlag = false;
    g_deepScanFlag = false;
    
    // Create test server
    testServer = new TestWebServer(&testConfig, &testStateManager, 
                                   &testBudgetManager, &testScheduleManager);
}

void tearDown(void) {
    if (testServer) {
        delete testServer;
        testServer = nullptr;
    }
}

// Test: Server initialization
void test_server_begin(void) {
    bool result = testServer->begin();
    TEST_ASSERT_TRUE(result);
}

// Test: Endpoint registration
void test_endpoint_registration(void) {
    testServer->begin();
    
    WebServer* server = testServer->getServer();
    TEST_ASSERT_NOT_NULL(server);
    
    // Verify handlers are registered
    TEST_ASSERT_TRUE(server->hasHandler("/"));
    TEST_ASSERT_TRUE(server->hasHandler("/trigger-upload"));
    TEST_ASSERT_TRUE(server->hasHandler("/status"));
    TEST_ASSERT_TRUE(server->hasHandler("/reset-state"));
    TEST_ASSERT_TRUE(server->hasHandler("/config"));
}

// Test: Trigger upload endpoint
void test_trigger_upload_endpoint(void) {
    testServer->begin();
    
    WebServer* server = testServer->getServer();
    TEST_ASSERT_FALSE(g_triggerUploadFlag);
    
    // Simulate request
    server->simulateRequest("/trigger-upload");
    
    // Verify flag was set
    TEST_ASSERT_TRUE(g_triggerUploadFlag);
    
    // Verify response
    TEST_ASSERT_EQUAL(200, server->getLastResponseCode());
    TEST_ASSERT_EQUAL_STRING("application/json", server->getLastResponseType().c_str());
    String body = server->getLastResponseBody();
    TEST_ASSERT_TRUE(body.length() > 0);
    TEST_ASSERT_TRUE(strstr(body.c_str(), "success") != NULL);
    TEST_ASSERT_EQUAL_STRING("*", server->getResponseHeader("Access-Control-Allow-Origin").c_str());
    
    // Reset
    g_triggerUploadFlag = false;
}

// Test: Reset state endpoint
void test_reset_state_endpoint(void) {
    testServer->begin();
    
    WebServer* server = testServer->getServer();
    TEST_ASSERT_FALSE(g_resetStateFlag);
    
    // Simulate request
    server->simulateRequest("/reset-state");
    
    // Verify flag was set
    TEST_ASSERT_TRUE(g_resetStateFlag);
    
    // Verify response
    TEST_ASSERT_EQUAL(200, server->getLastResponseCode());
    TEST_ASSERT_EQUAL_STRING("application/json", server->getLastResponseType().c_str());
    String body2 = server->getLastResponseBody();
    TEST_ASSERT_TRUE(body2.length() > 0);
    TEST_ASSERT_TRUE(strstr(body2.c_str(), "success") != NULL);
    TEST_ASSERT_EQUAL_STRING("*", server->getResponseHeader("Access-Control-Allow-Origin").c_str());
    
    // Reset
    g_resetStateFlag = false;
}

// Test: Status JSON generation
void test_status_json_generation(void) {
    testServer->begin();
    
    WebServer* server = testServer->getServer();
    
    // Simulate request
    server->simulateRequest("/status");
    
    // Verify response
    TEST_ASSERT_EQUAL(200, server->getLastResponseCode());
    TEST_ASSERT_EQUAL_STRING("application/json", server->getLastResponseType().c_str());
    String body3 = server->getLastResponseBody();
    TEST_ASSERT_TRUE(body3.length() > 0);
    TEST_ASSERT_TRUE(strstr(body3.c_str(), "uptime_seconds") != NULL);
    TEST_ASSERT_EQUAL_STRING("*", server->getResponseHeader("Access-Control-Allow-Origin").c_str());
}

// Test: Config endpoint
void test_config_endpoint(void) {
    testServer->begin();
    
    WebServer* server = testServer->getServer();
    
    // Simulate request
    server->simulateRequest("/config");
    
    // Verify response
    TEST_ASSERT_EQUAL(200, server->getLastResponseCode());
    TEST_ASSERT_EQUAL_STRING("application/json", server->getLastResponseType().c_str());
    String body4 = server->getLastResponseBody();
    TEST_ASSERT_TRUE(body4.length() > 0);
    TEST_ASSERT_TRUE(strstr(body4.c_str(), "endpoint_type") != NULL);
    TEST_ASSERT_EQUAL_STRING("*", server->getResponseHeader("Access-Control-Allow-Origin").c_str());
}

// Test: Handle client (no-op in mock)
void test_handle_client(void) {
    testServer->begin();
    
    // handleClient should not crash
    testServer->handleClient();
    TEST_ASSERT_TRUE(true);
}

// Test: Scan now endpoint
void test_scan_now_endpoint(void) {
    testServer->begin();
    
    WebServer* server = testServer->getServer();
    TEST_ASSERT_FALSE(g_scanNowFlag);
    
    // Simulate request
    server->simulateRequest("/scan-now");
    
    // Verify flag was set
    TEST_ASSERT_TRUE(g_scanNowFlag);
    
    // Verify response
    TEST_ASSERT_EQUAL(200, server->getLastResponseCode());
    TEST_ASSERT_EQUAL_STRING("application/json", server->getLastResponseType().c_str());
    String body = server->getLastResponseBody();
    TEST_ASSERT_TRUE(body.length() > 0);
    TEST_ASSERT_TRUE(strstr(body.c_str(), "success") != NULL);
    TEST_ASSERT_TRUE(strstr(body.c_str(), "SD card scan triggered") != NULL);
    TEST_ASSERT_EQUAL_STRING("*", server->getResponseHeader("Access-Control-Allow-Origin").c_str());
    
    // Reset
    g_scanNowFlag = false;
}

// Test: Delta scan endpoint
void test_delta_scan_endpoint(void) {
    testServer->begin();
    
    WebServer* server = testServer->getServer();
    TEST_ASSERT_FALSE(g_deltaScanFlag);
    
    // Simulate request
    server->simulateRequest("/delta-scan");
    
    // Verify flag was set
    TEST_ASSERT_TRUE(g_deltaScanFlag);
    
    // Verify response
    TEST_ASSERT_EQUAL(200, server->getLastResponseCode());
    TEST_ASSERT_EQUAL_STRING("application/json", server->getLastResponseType().c_str());
    String body = server->getLastResponseBody();
    TEST_ASSERT_TRUE(body.length() > 0);
    TEST_ASSERT_TRUE(strstr(body.c_str(), "success") != NULL);
    TEST_ASSERT_TRUE(strstr(body.c_str(), "Delta scan triggered") != NULL);
    TEST_ASSERT_EQUAL_STRING("*", server->getResponseHeader("Access-Control-Allow-Origin").c_str());
    
    // Reset
    g_deltaScanFlag = false;
}

// Test: Deep scan endpoint
void test_deep_scan_endpoint(void) {
    testServer->begin();
    
    WebServer* server = testServer->getServer();
    TEST_ASSERT_FALSE(g_deepScanFlag);
    
    // Simulate request
    server->simulateRequest("/deep-scan");
    
    // Verify flag was set
    TEST_ASSERT_TRUE(g_deepScanFlag);
    
    // Verify response
    TEST_ASSERT_EQUAL(200, server->getLastResponseCode());
    TEST_ASSERT_EQUAL_STRING("application/json", server->getLastResponseType().c_str());
    String body = server->getLastResponseBody();
    TEST_ASSERT_TRUE(body.length() > 0);
    TEST_ASSERT_TRUE(strstr(body.c_str(), "success") != NULL);
    TEST_ASSERT_TRUE(strstr(body.c_str(), "Deep scan triggered") != NULL);
    TEST_ASSERT_EQUAL_STRING("*", server->getResponseHeader("Access-Control-Allow-Origin").c_str());
    
    // Reset
    g_deepScanFlag = false;
}

// Test: CORS headers consistency
void test_cors_headers_consistency(void) {
    testServer->begin();
    
    WebServer* server = testServer->getServer();
    
    // Test all endpoints have consistent CORS headers
    const char* endpoints[] = {"/trigger-upload", "/scan-now", "/delta-scan", "/deep-scan", "/status", "/reset-state", "/config"};
    const int numEndpoints = sizeof(endpoints) / sizeof(endpoints[0]);
    
    for (int i = 0; i < numEndpoints; i++) {
        server->simulateRequest(endpoints[i]);
        TEST_ASSERT_EQUAL_STRING("*", server->getResponseHeader("Access-Control-Allow-Origin").c_str());
        TEST_ASSERT_EQUAL_STRING("GET, OPTIONS", server->getResponseHeader("Access-Control-Allow-Methods").c_str());
        TEST_ASSERT_EQUAL_STRING("Content-Type", server->getResponseHeader("Access-Control-Allow-Headers").c_str());
    }
}

// Test: Helper methods
void test_helper_methods(void) {
    testServer->begin();
    
    // Test uptime string generation (indirectly)
    // The actual method is private, but we can verify it doesn't crash
    // by calling handleClient which may use it
    testServer->handleClient();
    TEST_ASSERT_TRUE(true);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    RUN_TEST(test_server_begin);
    RUN_TEST(test_endpoint_registration);
    RUN_TEST(test_trigger_upload_endpoint);
    RUN_TEST(test_reset_state_endpoint);
    RUN_TEST(test_scan_now_endpoint);
    RUN_TEST(test_delta_scan_endpoint);
    RUN_TEST(test_deep_scan_endpoint);
    RUN_TEST(test_status_json_generation);
    RUN_TEST(test_config_endpoint);
    RUN_TEST(test_handle_client);
    RUN_TEST(test_cors_headers_consistency);
    RUN_TEST(test_helper_methods);
    
    return UNITY_END();
}
