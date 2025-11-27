#ifndef TEST_WEB_SERVER_H
#define TEST_WEB_SERVER_H

#include <Arduino.h>
#include <WebServer.h>
#include "Config.h"
#include "UploadStateManager.h"
#include "TimeBudgetManager.h"
#include "ScheduleManager.h"

// Global trigger flags for upload and state reset
extern volatile bool g_triggerUploadFlag;
extern volatile bool g_resetStateFlag;

class TestWebServer {
private:
    WebServer* server;
    Config* config;
    UploadStateManager* stateManager;
    TimeBudgetManager* budgetManager;
    ScheduleManager* scheduleManager;
    
    // Request handlers
    void handleRoot();
    void handleTriggerUpload();
    void handleStatus();
    void handleResetState();
    void handleConfig();
    void handleLogs();
    void handleNotFound();
    
    // Helper methods
    String getUptimeString();
    String getCurrentTimeString();
    int getPendingFilesCount();
    String escapeJson(const String& str);

public:
    TestWebServer(Config* cfg, UploadStateManager* state, 
                  TimeBudgetManager* budget, ScheduleManager* schedule);
    ~TestWebServer();
    
    bool begin();
    void handleClient();
    
    // Update manager references (needed after uploader recreation)
    void updateManagers(UploadStateManager* state, TimeBudgetManager* budget, ScheduleManager* schedule);
};

#endif // TEST_WEB_SERVER_H
