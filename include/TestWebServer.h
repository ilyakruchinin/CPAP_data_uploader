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
#include "WebStatus.h"
#include "SDCardManager.h"

#ifdef ENABLE_OTA_UPDATES
#include "OTAManager.h"
#endif

// Global trigger flags for upload and state reset
extern volatile bool g_triggerUploadFlag;
extern volatile bool g_resetStateFlag;
extern volatile bool g_softRebootFlag;
extern volatile bool g_monitorActivityFlag;
extern volatile bool g_stopMonitorFlag;

// Config edit lock — set by web UI to pause FSM uploads while user edits config
extern bool g_configEditLock;
extern unsigned long g_configEditLockAt;

class TestWebServer {
private:
    WebServer* server;
    Config* config;
    UploadStateManager* stateManager;     // cloud (or sole) state manager
    UploadStateManager* smbStateManager;  // SMB state manager (may be null)
    ScheduleManager* scheduleManager;
    WiFiManager* wifiManager;
    CPAPMonitor* cpapMonitor;
    TrafficMonitor* trafficMonitor;
    SDCardManager* sdManager;
    
#ifdef ENABLE_OTA_UPDATES
    OTAManager* otaManager;
#endif
    
    // Request handlers
    void handleRoot();
    void handleTriggerUpload();
    void handleStatusPage();      // HTML Status Page
    void handleApiStatus();       // JSON Status API
    void handleSoftReboot();
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
    void handleApiConfigRawGet();   // GET /api/config-raw
    void handleApiConfigRawPost();  // POST /api/config-raw
    void handleApiConfigLock();     // POST /api/config-lock

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
    void setSmbStateManager(UploadStateManager* sm);
    void setWiFiManager(WiFiManager* wifi);
    void setTrafficMonitor(TrafficMonitor* tm);
    void setSdManager(SDCardManager* sd);

    // Zero-heap status snapshot — call from main loop every ~2-3 s
    void updateStatusSnapshot();
    // Config snapshot — call once after Config is loaded at boot
    void initConfigSnapshot();
    
#ifdef ENABLE_OTA_UPDATES
    // OTA manager access
    void setOTAManager(OTAManager* ota);
#endif
};

#endif // TEST_WEB_SERVER_H
