#ifndef FILE_UPLOADER_H
#define FILE_UPLOADER_H

#include <Arduino.h>
#include <FS.h>
#include <vector>
#include "Config.h"
#include "UploadStateManager.h"
#include "TimeBudgetManager.h"
#include "ScheduleManager.h"
#include "WiFiManager.h"
#include "SDCardManager.h"

// Forward declaration to avoid circular dependency
#ifdef ENABLE_TEST_WEBSERVER
class TestWebServer;
#endif

// Include uploader implementations based on feature flags
#ifdef ENABLE_SMB_UPLOAD
#include "SMBUploader.h"
#endif

#ifdef ENABLE_WEBDAV_UPLOAD
#include "WebDAVUploader.h"
#endif

#ifdef ENABLE_SLEEPHQ_UPLOAD
#include "SleepHQUploader.h"
#endif

class FileUploader {
private:
    Config* config;
    UploadStateManager* stateManager;
    TimeBudgetManager* budgetManager;
    ScheduleManager* scheduleManager;
    WiFiManager* wifiManager;
    
#ifdef ENABLE_TEST_WEBSERVER
    TestWebServer* webServer;  // Optional web server for handling requests during uploads
#endif
    
    // Periodic SD card release tracking
    unsigned long lastSdReleaseTime;
    
    // Helper method for periodic SD card release
    bool checkAndReleaseSD(class SDCardManager* sdManager);
    
    // Uploader instances (only compiled if feature flag is enabled)
#ifdef ENABLE_SMB_UPLOAD
    SMBUploader* smbUploader;
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
    WebDAVUploader* webdavUploader;
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
    SleepHQUploader* sleephqUploader;
#endif
    
    // File scanning
    std::vector<String> scanDatalogFolders(fs::FS &sd, bool includeCompleted = false);
    std::vector<String> scanFolderFiles(fs::FS &sd, const String& folderPath);
    std::vector<String> scanRootAndSettingsFiles(fs::FS &sd);
    
    // Upload logic
    bool uploadDatalogFolder(class SDCardManager* sdManager, const String& folderName);
    bool uploadSingleFile(class SDCardManager* sdManager, const String& filePath);
    
    // Helper: check if a DATALOG folder name (YYYYMMDD) is within the recent window
    bool isRecentFolder(const String& folderName) const;
    
    // Helper: lazily create cloud import session on first actual upload
    bool ensureCloudImport();
    bool cloudImportCreated;
    
    // Session management
    bool startUploadSession(fs::FS &sd);
    void endUploadSession(fs::FS &sd);

public:
    FileUploader(Config* cfg, WiFiManager* wifi);
    ~FileUploader();
    
    bool begin(fs::FS &sd);
    bool shouldUpload();
    bool uploadNewFiles(class SDCardManager* sdManager, bool forceUpload = false);
    bool scanPendingFolders(class SDCardManager* sdManager);  // Scan SD card without uploading
    bool performDeltaScan(class SDCardManager* sdManager);    // Compare remote vs local file counts
    bool performDeepScan(class SDCardManager* sdManager);     // Compare remote vs local file sizes
    
    // Getters for internal components (for web interface access)
    UploadStateManager* getStateManager() { return stateManager; }
    TimeBudgetManager* getBudgetManager() { return budgetManager; }
    ScheduleManager* getScheduleManager() { return scheduleManager; }
    bool hasIncompleteFolders() { return stateManager && stateManager->getIncompleteFoldersCount() > 0; }
    
#ifdef ENABLE_TEST_WEBSERVER
    // Set web server for handling requests during uploads
    void setWebServer(TestWebServer* server) { webServer = server; }
#endif
};

#endif // FILE_UPLOADER_H
