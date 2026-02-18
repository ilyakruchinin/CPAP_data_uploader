#ifndef TEST_WEB_SERVER_H
#define TEST_WEB_SERVER_H

#include <Arduino.h>
#include <WebServer.h>
#include "Config.h"
#include "UploadStateManager.h"
#include "ScheduleManager.h"
#include "WiFiManager.h"
#include "CPAPMonitor.h"
#include "TrafficMonitor.h"

#ifdef ENABLE_OTA_UPDATES
#include "OTAManager.h"
#endif

// Global trigger flags for upload and state reset
extern volatile bool g_triggerUploadFlag;
extern volatile bool g_resetStateFlag;
extern volatile bool g_monitorActivityFlag;
extern volatile bool g_stopMonitorFlag;

class TestWebServer {
private:
    WebServer* server;
    Config* config;
    UploadStateManager* stateManager;
    ScheduleManager* scheduleManager;
    WiFiManager* wifiManager;
    CPAPMonitor* cpapMonitor;
    TrafficMonitor* trafficMonitor;
    
#ifdef ENABLE_OTA_UPDATES
    OTAManager* otaManager;
#endif
    
    // Request handlers
    void handleRoot();
    void handleTriggerUpload();
    void handleStatusPage();      // HTML Status Page
    void handleApiStatus();       // JSON Status API
    void handleResetState();
    void handleConfigPage();      // HTML Config Page
    void handleApiConfig();       // JSON Config API
    void handleLogs();            // HTML Logs Viewer (AJAX)
    void handleApiLogs();         // Raw Logs API
    void handleNotFound();
    void handleMonitorStart();
    void handleMonitorStop();
    void handleSdActivity();
    void handleMonitorPage();
    
#ifdef ENABLE_OTA_UPDATES
    // OTA handlers
    void handleOTAPage();
    void handleOTAUpload();
    void handleOTAUploadComplete();
    void handleOTAURL();
#endif
    
    // Helper methods
    String getUptimeString();
    String getCurrentTimeString();
    int getPendingFilesCount();
    int getPendingFoldersCount();
    String escapeJson(const String& str);
    bool redirectToIpIfMdnsRequest();
    bool isUploadInProgress() const;
    bool rejectHeavyRequestDuringUpload(const char* endpoint);
    
    // Static helper methods
    static void addCorsHeaders(WebServer* server);

public:
    TestWebServer(Config* cfg, UploadStateManager* state, 
                  ScheduleManager* schedule, 
                  WiFiManager* wifi = nullptr, CPAPMonitor* monitor = nullptr);
    ~TestWebServer();
    
    bool begin();
    void handleClient();
    
    // Update manager references (needed after uploader recreation)
    void updateManagers(UploadStateManager* state, ScheduleManager* schedule);
    void setWiFiManager(WiFiManager* wifi);
    void setTrafficMonitor(TrafficMonitor* tm);
    
#ifdef ENABLE_OTA_UPDATES
    // OTA manager access
    void setOTAManager(OTAManager* ota);
#endif
};

#endif // TEST_WEB_SERVER_H
