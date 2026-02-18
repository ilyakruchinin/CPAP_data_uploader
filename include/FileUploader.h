#ifndef FILE_UPLOADER_H
#define FILE_UPLOADER_H

#include <Arduino.h>
#include <FS.h>
#include <vector>
#include "Config.h"
#include "UploadStateManager.h"
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

// Result of an exclusive-access upload session
enum class UploadResult {
    COMPLETE,        // All eligible files uploaded
    TIMEOUT,         // X-minute timer expired (partial upload, not an error)
    ERROR            // Upload failure
};

// Filter for which data categories to upload
enum class DataFilter {
    FRESH_ONLY,  // Only fresh DATALOG + root/SETTINGS (mandatory)
    OLD_ONLY,    // Only old DATALOG folders + root/SETTINGS (mandatory)
    ALL_DATA     // Everything
};

class FileUploader {
private:
    Config* config;
    UploadStateManager* smbStateManager;    // tracks SMB-only uploads
    UploadStateManager* cloudStateManager;  // tracks Cloud-only uploads
    ScheduleManager* scheduleManager;
    WiFiManager* wifiManager;

#ifdef ENABLE_TEST_WEBSERVER
    TestWebServer* webServer;
#endif

    // Uploader instances
#ifdef ENABLE_SMB_UPLOAD
    SMBUploader* smbUploader;
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
    SleepHQUploader* sleephqUploader;
#endif

    // File scanning (sm = state manager used for completed/pending checks)
    std::vector<String> scanDatalogFolders(fs::FS &sd, UploadStateManager* sm,
                                           bool includeCompleted = false);
    std::vector<String> scanFolderFiles(fs::FS &sd, const String& folderPath);
    std::vector<String> scanSettingsFiles(fs::FS &sd);

    // ── SMB pass helpers ────────────────────────────────────────────────────
    bool uploadMandatoryFilesSmb(class SDCardManager* sdManager, fs::FS &sd);
    bool uploadSingleFileSmb(class SDCardManager* sdManager, const String& filePath,
                             bool force = false);
    bool uploadDatalogFolderSmb(class SDCardManager* sdManager, const String& folderName);

    // ── Cloud pass helpers ───────────────────────────────────────────────────
    bool uploadDatalogFolderCloud(class SDCardManager* sdManager, const String& folderName);
    bool uploadSingleFileCloud(class SDCardManager* sdManager, const String& filePath,
                               bool force = false);

    // Helper: check if a DATALOG folder name (YYYYMMDD) is within the recent window
    bool isRecentFolder(const String& folderName) const;

    // Cloud import session management
    bool ensureCloudImport();
    void finalizeCloudImport(class SDCardManager* sdManager, fs::FS &sd);
    bool cloudImportCreated;
    bool cloudImportFailed;
    int  cloudDatalogFilesUploaded;  // DATALOG files uploaded this cloud pass; 0 = skip finalize

    // Return the primary state manager (cloud if configured, else smb)
    UploadStateManager* primaryStateManager() const {
        if (cloudStateManager) return cloudStateManager;
        return smbStateManager;
    }

public:
    FileUploader(Config* cfg, WiFiManager* wifi);
    ~FileUploader();

    bool begin(fs::FS &sd);

    // FSM-driven exclusive access upload
    UploadResult uploadWithExclusiveAccess(class SDCardManager* sdManager, int maxMinutes,
                                           DataFilter filter);

    // Getters for internal components (for web interface access)
    UploadStateManager* getStateManager() { return primaryStateManager(); }
    ScheduleManager* getScheduleManager() { return scheduleManager; }
    bool hasIncompleteFolders() {
        bool smb   = smbStateManager   && smbStateManager->getIncompleteFoldersCount()   > 0;
        bool cloud = cloudStateManager && cloudStateManager->getIncompleteFoldersCount() > 0;
        return smb || cloud;
    }

#ifdef ENABLE_TEST_WEBSERVER
    void setWebServer(TestWebServer* server) { webServer = server; }
#endif
};

#endif // FILE_UPLOADER_H
