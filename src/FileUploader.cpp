#include "FileUploader.h"
#include "Logger.h"
#include <SD_MMC.h>

#ifdef ENABLE_TEST_WEBSERVER
#include "TestWebServer.h"
#endif

// Constructor
FileUploader::FileUploader(Config* cfg, WiFiManager* wifi) 
    : config(cfg),
      stateManager(nullptr),
      budgetManager(nullptr),
      scheduleManager(nullptr),
      wifiManager(wifi),
#ifdef ENABLE_TEST_WEBSERVER
      webServer(nullptr),
#endif
      lastSdReleaseTime(0),
      cloudImportCreated(false)
#ifdef ENABLE_SMB_UPLOAD
      , smbUploader(nullptr)
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
      , webdavUploader(nullptr)
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
      , sleephqUploader(nullptr)
#endif
{
}

// Destructor
FileUploader::~FileUploader() {
    if (stateManager) delete stateManager;
    if (budgetManager) delete budgetManager;
    if (scheduleManager) delete scheduleManager;
#ifdef ENABLE_SMB_UPLOAD
    if (smbUploader) delete smbUploader;
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
    if (webdavUploader) delete webdavUploader;
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (sleephqUploader) delete sleephqUploader;
#endif
}

// Initialize all components and load upload state
bool FileUploader::begin(fs::FS &sd) {
    LOG("[FileUploader] Initializing components...");
    
    // Initialize UploadStateManager
    stateManager = new UploadStateManager();
    if (!stateManager->begin(sd)) {
        LOG("[FileUploader] WARNING: Failed to load upload state, starting fresh");
        // Continue anyway - stateManager will work with empty state
    }
    
    // Initialize TimeBudgetManager
    budgetManager = new TimeBudgetManager();
    
    // Initialize ScheduleManager
    scheduleManager = new ScheduleManager();
    if (!scheduleManager->begin(
            config->getUploadHour(),
            config->getGmtOffsetHours())) {
        LOG("[FileUploader] ERROR: Failed to initialize ScheduleManager");
        return false;
    }
    
    // Restore last upload timestamp from state
    scheduleManager->setLastUploadTimestamp(stateManager->getLastUploadTimestamp());
    
    // Initialize uploaders based on endpoint type and build flags
    // Supports comma-separated types (e.g., "SMB,CLOUD")
    String endpointType = config->getEndpointType();
    LOGF("[FileUploader] Endpoint type: %s", endpointType.c_str());
    
    bool anyBackendCreated = false;
    
#ifdef ENABLE_SMB_UPLOAD
    if (config->hasSmbEndpoint()) {
        smbUploader = new SMBUploader(
            config->getEndpoint(),
            config->getEndpointUser(),
            config->getEndpointPassword()
        );
        LOG("[FileUploader] SMBUploader created (will connect during upload)");
        anyBackendCreated = true;
    }
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
    if (endpointType == "WEBDAV") {
        webdavUploader = new WebDAVUploader(
            config->getEndpoint(),
            config->getEndpointUser(),
            config->getEndpointPassword()
        );
        LOG("[FileUploader] WebDAVUploader created (will connect during upload)");
        anyBackendCreated = true;
    }
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (config->hasCloudEndpoint()) {
        sleephqUploader = new SleepHQUploader(config);
        LOG("[FileUploader] SleepHQUploader created (will connect during upload)");
        anyBackendCreated = true;
    }
#endif
    
    if (!anyBackendCreated) {
        LOGF("[FileUploader] ERROR: No uploader created for endpoint type: %s", endpointType.c_str());
        LOG("[FileUploader] Supported types (based on build flags):");
#ifdef ENABLE_SMB_UPLOAD
        LOG("[FileUploader]   - SMB (enabled)");
#else
        LOG("[FileUploader]   - SMB (disabled - compile with -DENABLE_SMB_UPLOAD)");
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
        LOG("[FileUploader]   - WEBDAV (enabled)");
#else
        LOG("[FileUploader]   - WEBDAV (disabled - compile with -DENABLE_WEBDAV_UPLOAD)");
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
        LOG("[FileUploader]   - CLOUD/SLEEPHQ (enabled)");
#else
        LOG("[FileUploader]   - CLOUD/SLEEPHQ (disabled - compile with -DENABLE_SLEEPHQ_UPLOAD)");
#endif
        return false;
    }
    
    LOG("[FileUploader] Initialization complete");
    return true;
}

// Check if it's time to upload
bool FileUploader::shouldUpload() {
    if (!scheduleManager) {
        return false;
    }
    return scheduleManager->isUploadTime();
}



// Check if it's time to periodically release SD card control
// Returns true if SD was released and retaken successfully
// Returns false if unable to retake control (should abort upload)
bool FileUploader::checkAndReleaseSD(SDCardManager* sdManager) {
    unsigned long now = millis();
    unsigned long intervalMs = config->getSdReleaseIntervalSeconds() * 1000;
    
    // Check if it's time to release
    if (now - lastSdReleaseTime < intervalMs) {
        return true;  // Not time yet, continue
    }
    
    LOG_DEBUG("[FileUploader] Periodic SD card release - giving CPAP priority access");
    
    // Pause active time tracking (only if an upload session is active)
    bool hasActiveSession = budgetManager->getRemainingBudgetMs() > 0 || budgetManager->getActiveTimeMs() > 0;
    if (hasActiveSession) {
        budgetManager->pauseActiveTime();
    }
    
    // Release SD card
    sdManager->releaseControl();
    
    // Wait configured time, handling web requests during the wait
    unsigned long waitMs = config->getSdReleaseWaitMs();
    LOG_DEBUGF("[FileUploader] Waiting %lu ms before retaking control...", waitMs);
    
#ifdef ENABLE_TEST_WEBSERVER
    // Handle web requests during wait to keep interface responsive
    if (webServer) {
        unsigned long waitStart = millis();
        while (millis() - waitStart < waitMs) {
            webServer->handleClient();
            delay(10);  // Small delay to prevent tight loop
        }
    } else {
        delay(waitMs);
    }
#else
    delay(waitMs);
#endif
    
    // Retake control
    LOG_DEBUG("[FileUploader] Attempting to retake SD card control...");
    if (!sdManager->takeControl()) {
        LOG_ERROR("[FileUploader] Failed to retake SD card control");
        LOG_WARN("[FileUploader] CPAP machine may be actively using SD card");
        return false;  // Abort upload
    }
    
    // Resume active time tracking (only if an upload session is active)
    if (hasActiveSession) {
        budgetManager->resumeActiveTime();
    }
    
    // Reset release timer
    lastSdReleaseTime = millis();
    
    LOG_DEBUG("[FileUploader] SD card control reacquired, resuming upload");
    return true;
}

// Main upload orchestration
bool FileUploader::uploadNewFiles(SDCardManager* sdManager, bool forceUpload) {
    fs::FS &sd = sdManager->getFS();
    LOG("[FileUploader] Starting upload orchestration...");
    
    // Check WiFi connection first
    if (!wifiManager || !wifiManager->isConnected()) {
        LOG_ERROR("[FileUploader] WiFi not connected - cannot upload");
        LOG_ERROR("[FileUploader] Please ensure WiFi connection is established before upload");
        return false;
    }
    
    // Check if it's time to upload (unless forced or retrying incomplete folders)
    bool hasIncompleteFolders = (stateManager->getIncompleteFoldersCount() > 0);
    if (!forceUpload && !hasIncompleteFolders && !shouldUpload()) {
        unsigned long secondsUntilNext = scheduleManager->getSecondsUntilNextUpload();
        LOG_DEBUGF("[FileUploader] Not upload time yet. Next upload in %lu hours", secondsUntilNext / 3600);
        return false;
    }
    
    if (hasIncompleteFolders && !forceUpload) {
        LOG("[FileUploader] Resuming upload for incomplete folders (retry)");
    }
    
    if (forceUpload) {
        LOG("[FileUploader] FORCED UPLOAD - bypassing schedule check");
    }
    LOG("[FileUploader] Upload time - starting session");
    
    // Start upload session with time budget
    if (!startUploadSession(sd)) {
        LOG_ERROR("[FileUploader] Failed to start upload session");
        return false;
    }
    
    bool anyUploaded = false;
    
    // Phase 1: Process DATALOG folders (newest first)
    LOG("[FileUploader] Phase 1: Processing DATALOG folders");
    std::vector<String> datalogFolders = scanDatalogFolders(sd);
    
    // Update total folders count for progress tracking
    stateManager->setTotalFoldersCount(datalogFolders.size() + stateManager->getCompletedFoldersCount() + stateManager->getPendingFoldersCount());
    
    for (const String& folderName : datalogFolders) {
        // Check if we still have budget
        if (!budgetManager->hasBudget()) {
            LOG("[FileUploader] Time budget exhausted during DATALOG processing");
            break;
        }
        
        // Check for periodic SD card release
        if (!checkAndReleaseSD(sdManager)) {
            LOG_ERROR("[FileUploader] Failed to retake SD card control, aborting upload");
            break;
        }
        
        // Upload the folder
        if (uploadDatalogFolder(sdManager, folderName)) {
            anyUploaded = true;
            LOGF("[FileUploader] Completed folder: %s", folderName.c_str());
        } else {
            LOGF("[FileUploader] Folder upload interrupted: %s", folderName.c_str());
            // Budget exhausted or error - stop processing
            break;
        }
        
#ifdef ENABLE_TEST_WEBSERVER
        // Handle web requests between folder uploads
        if (webServer) {
            webServer->handleClient();
        }
#endif
    }
    
    // Phase 2: Process root and SETTINGS files (if budget remains)
    if (budgetManager->hasBudget()) {
        LOG("[FileUploader] Phase 2: Processing root and SETTINGS files");
        std::vector<String> rootSettingsFiles = scanRootAndSettingsFiles(sd);
        
        for (const String& filePath : rootSettingsFiles) {
            // Check if we still have budget
            if (!budgetManager->hasBudget()) {
                LOG("[FileUploader] Time budget exhausted during root/SETTINGS processing");
                break;
            }
            
            // Check for periodic SD card release
            if (!checkAndReleaseSD(sdManager)) {
                LOG_ERROR("[FileUploader] Failed to retake SD card control, aborting upload");
                break;
            }
            
            // Upload the file
            if (uploadSingleFile(sdManager, filePath)) {
                anyUploaded = true;
            }
            
#ifdef ENABLE_TEST_WEBSERVER
            // Handle web requests between file uploads
            if (webServer) {
                webServer->handleClient();
            }
#endif
        }
    } else {
        LOG("[FileUploader] Skipping root/SETTINGS files - no budget remaining");
    }
    
    // End upload session and save state
    endUploadSession(sd);
    
    // Return true only if all folders are completed
    bool allComplete = (stateManager->getIncompleteFoldersCount() == 0);
    LOG_DEBUGF("[FileUploader] Upload session complete. All folders done: %s", allComplete ? "Yes" : "No");
    
    return allComplete;
}

// Scan SD card for pending folders without uploading
bool FileUploader::scanPendingFolders(SDCardManager* sdManager) {
    LOG("[FileUploader] Scanning SD card for pending folders...");
    
    fs::FS &sd = sdManager->getFS();
    
    // Scan DATALOG folders
    std::vector<String> datalogFolders = scanDatalogFolders(sd);
    
    // Update total folders count for progress tracking
    stateManager->setTotalFoldersCount(datalogFolders.size() + stateManager->getCompletedFoldersCount() + stateManager->getPendingFoldersCount());
    
    LOG_DEBUGF("[FileUploader] Found %d incomplete folders", datalogFolders.size());
    LOG_DEBUGF("[FileUploader] Total folders: %d (completed: %d, incomplete: %d, pending: %d)", 
         stateManager->getCompletedFoldersCount() + datalogFolders.size() + stateManager->getPendingFoldersCount(),
         stateManager->getCompletedFoldersCount(),
         datalogFolders.size(),
         stateManager->getPendingFoldersCount());
    
    return true;
}

// Scan DATALOG folders and sort by date (newest first)
std::vector<String> FileUploader::scanDatalogFolders(fs::FS &sd, bool includeCompleted) {
    std::vector<String> folders;
    
    File root = sd.open("/DATALOG");
    if (!root) {
        LOG_ERROR("[FileUploader] Cannot open /DATALOG folder");
        LOG_ERROR("[FileUploader] SD card may be in use by CPAP or not properly mounted");
        LOG_ERROR("[FileUploader] If DATALOG exists, this scan will be retried");
        return folders;  // Return empty - indicates scan failure
    }
    
    if (!root.isDirectory()) {
        LOG_ERROR("[FileUploader] /DATALOG exists but is not a directory");
        root.close();
        return folders;
    }
    
    // Calculate MAX_DAYS cutoff date if configured
    String maxDaysCutoff = "";
    int maxDays = config->getMaxDays();
    if (maxDays > 0) {
        time_t now = time(nullptr);
        if (now > 24 * 3600) {  // Valid NTP time
            time_t cutoff = now - (maxDays * 86400L);
            struct tm cutoffTm;
            localtime_r(&cutoff, &cutoffTm);
            char cutoffStr[9];
            snprintf(cutoffStr, sizeof(cutoffStr), "%04d%02d%02d",
                     cutoffTm.tm_year + 1900, cutoffTm.tm_mon + 1, cutoffTm.tm_mday);
            maxDaysCutoff = String(cutoffStr);
            LOGF("[FileUploader] MAX_DAYS=%d: only processing folders >= %s", maxDays, cutoffStr);
        } else {
            LOG_WARN("[FileUploader] MAX_DAYS configured but NTP time not available, processing all folders");
        }
    }
    
    // Scan for folders
    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            String folderName = String(file.name());
            
            // Extract just the folder name (remove path prefix if present)
            int lastSlash = folderName.lastIndexOf('/');
            if (lastSlash >= 0) {
                folderName = folderName.substring(lastSlash + 1);
            }
            
            // Apply MAX_DAYS filter (folder names are in YYYYMMDD format)
            if (!maxDaysCutoff.isEmpty() && folderName < maxDaysCutoff) {
                LOG_DEBUGF("[FileUploader] Skipping old folder (MAX_DAYS): %s", folderName.c_str());
                file.close();
                file = root.openNextFile();
                continue;
            }
            
            // Check if folder is already completed
            if (stateManager->isFolderCompleted(folderName)) {
                if (includeCompleted) {
                    // For delta/deep scans, include completed folders
                    folders.push_back(folderName);
                    LOG_INFOF("[FileUploader] Found completed DATALOG folder: %s", folderName.c_str());
                } else if (isRecentFolder(folderName)) {
                    // Recent completed folders are re-scanned to detect files
                    // that changed (e.g. CPAP still writing to today's data)
                    folders.push_back(folderName);
                    LOG_DEBUGF("[FileUploader] Re-checking recent completed folder: %s", folderName.c_str());
                } else {
                    LOG_DEBUGF("[FileUploader] Skipping completed folder: %s", folderName.c_str());
                }
            } else if (stateManager->isPendingFolder(folderName)) {
                // Check if pending folder now has files (was empty but now has content)
                String folderPath = "/DATALOG/" + folderName;
                std::vector<String> folderFiles = scanFolderFiles(sd, folderPath);
                
                if (!folderFiles.empty()) {
                    // Folder now has files - remove from pending state immediately and process normally
                    LOG_DEBUGF("[FileUploader] Pending folder now has files, removing from pending: %s", folderName.c_str());
                    stateManager->removeFolderFromPending(folderName);
                    folders.push_back(folderName);
                } else {
                    // Still empty - check if pending folder has timed out
                    unsigned long currentTime = time(NULL);
                    if (currentTime >= 1000000000 && stateManager->shouldPromotePendingToCompleted(folderName, currentTime)) {
                        // Timed out pending folder - include in scan for promotion
                        folders.push_back(folderName);
                        LOG_DEBUGF("[FileUploader] Found timed-out pending folder: %s", folderName.c_str());
                    } else {
                        // Still pending, skip for now
                        LOG_DEBUGF("[FileUploader] Skipping pending folder (within 7-day window): %s", folderName.c_str());
                    }
                }
            } else {
                // Regular incomplete folder
                folders.push_back(folderName);
                LOG_DEBUGF("[FileUploader] Found incomplete DATALOG folder: %s", folderName.c_str());
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    
    // Sort folders by date (newest first) - folders are in YYYYMMDD format
    std::sort(folders.begin(), folders.end(), [](const String& a, const String& b) {
        return a > b;  // Descending order (newest first)
    });
    
    if (folders.empty()) {
        LOG("[FileUploader] No incomplete DATALOG folders found");
        LOG_DEBUG("[FileUploader] Either all folders are uploaded or DATALOG is empty");
    } else {
        LOG_DEBUGF("[FileUploader] Found %d incomplete DATALOG folders", folders.size());
    }
    
    return folders;
}

// Scan files in a specific folder
// Returns empty vector on error - caller must check if scan was successful
std::vector<String> FileUploader::scanFolderFiles(fs::FS &sd, const String& folderPath) {
    std::vector<String> files;
    
    File folder = sd.open(folderPath);
    if (!folder) {
        LOG_ERRORF("[FileUploader] Failed to open folder: %s", folderPath.c_str());
        LOG_ERROR("[FileUploader] SD card may be in use by CPAP or experiencing read errors");
        LOG_ERROR("[FileUploader] This folder will be retried in the next upload session");
        return files;  // Return empty - caller should treat as error
    }
    
    if (!folder.isDirectory()) {
        LOG_ERRORF("[FileUploader] Path exists but is not a directory: %s", folderPath.c_str());
        folder.close();
        return files;
    }
    
    // Scan for .edf files
    File file = folder.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String fileName = String(file.name());
            
            // Extract just the file name (remove path prefix if present)
            int lastSlash = fileName.lastIndexOf('/');
            if (lastSlash >= 0) {
                fileName = fileName.substring(lastSlash + 1);
            }
            
            // Check if it's an .edf file
            if (fileName.endsWith(".edf") || fileName.endsWith(".EDF")) {
                files.push_back(fileName);
            }
        }
        file.close();
        file = folder.openNextFile();
    }
    folder.close();
    
    LOG_DEBUGF("[FileUploader] Found %d .edf files in %s", files.size(), folderPath.c_str());
    
    return files;
}

// Scan root and SETTINGS files that need tracking
std::vector<String> FileUploader::scanRootAndSettingsFiles(fs::FS &sd) {
    std::vector<String> files;
    
    // Root files to track
    std::vector<String> rootFiles = {
        "/Identification.json",
        "/Identification.crc",
        "/Identification.tgt",
        "/STR.edf",
        "/journal.jnl"
    };
    
    for (const String& file : rootFiles) {
        if (sd.exists(file)) {
            // Check if file has changed
            if (stateManager->hasFileChanged(sd, file)) {
                files.push_back(file);
                LOG_DEBUGF("[FileUploader] Root file changed: %s", file.c_str());
            }
        }
    }
    
    // SETTINGS files: scan entire /SETTINGS/ directory (supports legacy and modern formats)
    File settingsDir = sd.open("/SETTINGS");
    if (settingsDir && settingsDir.isDirectory()) {
        File settingsFile = settingsDir.openNextFile();
        while (settingsFile) {
            if (!settingsFile.isDirectory()) {
                String settingsFileName = String(settingsFile.name());
                // Extract just the filename
                int lastSlash = settingsFileName.lastIndexOf('/');
                if (lastSlash >= 0) {
                    settingsFileName = settingsFileName.substring(lastSlash + 1);
                }
                String settingsPath = "/SETTINGS/" + settingsFileName;
                
                if (stateManager->hasFileChanged(sd, settingsPath)) {
                    files.push_back(settingsPath);
                    LOG_DEBUGF("[FileUploader] SETTINGS file changed: %s", settingsPath.c_str());
                }
            }
            settingsFile.close();
            settingsFile = settingsDir.openNextFile();
        }
        settingsDir.close();
    } else {
        LOG_DEBUG("[FileUploader] /SETTINGS directory not found or not accessible");
    }
    
    LOG_DEBUGF("[FileUploader] Found %d changed root/SETTINGS files", files.size());
    
    return files;
}

// Check if a DATALOG folder name (YYYYMMDD) is within the recent window
// Recent folders are re-scanned on interval uploads to detect changed files
bool FileUploader::isRecentFolder(const String& folderName) const {
    int recentDays = config->getRecentFolderDays();
    if (recentDays <= 0) return false;
    
    time_t now = time(nullptr);
    if (now < 24 * 3600) return false;  // NTP not synced
    
    time_t cutoff = now - ((long)recentDays * 86400L);
    struct tm cutoffTm;
    localtime_r(&cutoff, &cutoffTm);
    char cutoffStr[9];
    snprintf(cutoffStr, sizeof(cutoffStr), "%04d%02d%02d",
             cutoffTm.tm_year + 1900, cutoffTm.tm_mon + 1, cutoffTm.tm_mday);
    
    return folderName >= String(cutoffStr);
}

// Lazily create a cloud import session on first actual upload
// Returns true if import is ready (already created or just created)
bool FileUploader::ensureCloudImport() {
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (cloudImportCreated) return true;
    if (!sleephqUploader || !config->hasCloudEndpoint()) return true;  // No cloud = OK
    
    if (!sleephqUploader->isConnected()) {
        LOG("[FileUploader] Connecting cloud uploader for import session...");
        if (!sleephqUploader->begin()) {
            LOG_ERROR("[FileUploader] Failed to initialize cloud uploader");
            LOG_WARN("[FileUploader] Cloud uploads will be skipped this session");
            return false;
        }
    }
    if (sleephqUploader->isConnected()) {
        if (!sleephqUploader->createImport()) {
            LOG_ERROR("[FileUploader] Failed to create cloud import");
            LOG_WARN("[FileUploader] Cloud uploads will be skipped this session");
            return false;
        }
        cloudImportCreated = true;
    }
    return cloudImportCreated;
#else
    return true;
#endif
}

// Start upload session with time budget
bool FileUploader::startUploadSession(fs::FS &sd) {
    LOG("[FileUploader] Starting upload session");
    
    // Get session duration from config
    unsigned long sessionDuration = config->getSessionDurationSeconds();
    
    // Check if we need to apply retry multiplier
    int retryCount = stateManager->getCurrentRetryCount();
    
    // Apply multiplier for any retry attempt to increase budget
    if (retryCount > 0) {
        int multiplier = retryCount + 1;  // +1 so first retry gets 2x budget
        LOG_DEBUGF("[FileUploader] Applying retry multiplier: %dx (retry count: %d)", multiplier, retryCount);
        budgetManager->startSession(sessionDuration, multiplier);
    } else {
        budgetManager->startSession(sessionDuration);
    }
    
    LOG_DEBUGF("[FileUploader] Session budget: %lu ms (active time only)", budgetManager->getRemainingBudgetMs());
    LOG_DEBUGF("[FileUploader] Periodic SD release: every %d seconds", config->getSdReleaseIntervalSeconds());
    
    // Initialize periodic release timer
    lastSdReleaseTime = millis();
    
    // Cloud import is created lazily via ensureCloudImport() on first actual upload
    // This avoids creating empty imports when no files have changed
    cloudImportCreated = false;
    
    return true;
}

// End upload session and save state
void FileUploader::endUploadSession(fs::FS &sd) {
    LOG("[FileUploader] Ending upload session");
    
    // Process cloud import if active
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (sleephqUploader && config->hasCloudEndpoint()) {
        if (!sleephqUploader->getCurrentImportId().isEmpty()) {
            if (!sleephqUploader->processImport()) {
                LOG_WARN("[FileUploader] Failed to process cloud import");
            }
        }
    }
#endif
    
    // Save upload state
    if (!stateManager->save(sd)) {
        LOG_ERROR("[FileUploader] Failed to save upload state");
        LOG_WARN("[FileUploader] Upload progress may be lost - will retry from last saved state");
    }
    
    // Only mark upload as completed if there are no incomplete folders
    bool hasIncompleteFolders = (stateManager->getIncompleteFoldersCount() > 0);
    if (!hasIncompleteFolders) {
        // Update last upload timestamp
        time_t now;
        time(&now);
        stateManager->setLastUploadTimestamp((unsigned long)now);
        scheduleManager->markUploadCompleted();
        LOG("[FileUploader] All folders completed - upload session marked as done");
    } else {
        LOG("[FileUploader] Incomplete folders remain - upload will retry");
    }
    
    // Calculate wait time
    unsigned long waitTimeMs = budgetManager->getWaitTimeMs();
    LOG_DEBUGF("[FileUploader] Wait time before next session: %lu seconds", waitTimeMs / 1000);
    
    // Save state again with updated timestamp
    if (!stateManager->save(sd)) {
        LOG_ERROR("[FileUploader] Failed to save final state with timestamp");
        LOG_WARN("[FileUploader] Next upload may occur sooner than scheduled");
    }
}

// Upload all files in a DATALOG folder
bool FileUploader::uploadDatalogFolder(SDCardManager* sdManager, const String& folderName) {
    fs::FS &sd = sdManager->getFS();
    
    // Get retry count BEFORE setting current folder (in case it's a different folder)
    String currentRetryFolder = stateManager->getCurrentRetryFolder();
    int retryCount = (currentRetryFolder == folderName) ? stateManager->getCurrentRetryCount() : 0;
    
    // Set this as the current retry folder
    stateManager->setCurrentRetryFolder(folderName);
    
    // Log appropriately based on retry status
    if (retryCount > 0) {
        LOGF("[FileUploader] RETRY ATTEMPT %d: Uploading DATALOG folder: %s", retryCount + 1, folderName.c_str());
    } else {
        LOGF("[FileUploader] Uploading DATALOG folder: %s", folderName.c_str());
    }
    
    // Build folder path
    String folderPath = "/DATALOG/" + folderName;
    
    // Verify folder exists before scanning
    File folderCheck = sd.open(folderPath);
    if (!folderCheck) {
        LOG_ERRORF("[FileUploader] Cannot access folder: %s", folderPath.c_str());
        LOG_ERROR("[FileUploader] SD card may be in use by CPAP machine");
        LOG_ERROR("[FileUploader] Folder will be retried in next upload session");
        stateManager->incrementCurrentRetryCount();
        stateManager->save(sd);
        return false;  // Treat as error, not completion
    }
    
    if (!folderCheck.isDirectory()) {
        LOG_ERRORF("[FileUploader] Path is not a directory: %s", folderPath.c_str());
        folderCheck.close();
        stateManager->incrementCurrentRetryCount();
        stateManager->save(sd);
        return false;
    }
    folderCheck.close();
    
    // Scan for files in the folder
    std::vector<String> files = scanFolderFiles(sd, folderPath);
    
    // If this was a pending folder but now has files, remove it from pending state
    if (stateManager->isPendingFolder(folderName) && !files.empty()) {
        LOG_DEBUGF("[FileUploader] Removing folder from pending state (now has files): %s", folderName.c_str());
        stateManager->removeFolderFromPending(folderName);
    }
    
    if (files.empty()) {
        // Need to distinguish between "truly empty" and "scan failed"
        // Try to open the folder again to verify it's accessible
        File verifyFolder = sd.open(folderPath);
        if (!verifyFolder) {
            LOG_ERROR("[FileUploader] Folder scan returned empty but folder is not accessible");
            LOG_ERROR("[FileUploader] This indicates SD card read error or CPAP interference");
            LOG_ERROR("[FileUploader] Folder will be retried in next upload session");
            stateManager->incrementCurrentRetryCount();
            stateManager->save(sd);
            return false;  // Treat as error
        }
        verifyFolder.close();
        
        // Folder is accessible but truly empty - handle with pending state
        LOG_WARN("[FileUploader] No .edf files found in folder (folder is empty)");
        
        // Check if NTP time is valid before tracking pending folders
        unsigned long currentTime = time(NULL);
        if (currentTime < 1000000000) {  // Invalid NTP time (before year 2001)
            LOG_WARN("[FileUploader] NTP time not available - cannot track empty folder timing");
            LOG_WARN("[FileUploader] Empty folder will be rechecked in next upload session");
            stateManager->incrementCurrentRetryCount();
            stateManager->save(sd);
            return false;  // Will retry when NTP is available
        }
        
        // Check if folder is already in pending state
        if (stateManager->isPendingFolder(folderName)) {
            // Check if 7-day timeout has elapsed
            if (stateManager->shouldPromotePendingToCompleted(folderName, currentTime)) {
                // Promote to completed after 7 days of being empty
                stateManager->promotePendingToCompleted(folderName);
                stateManager->clearCurrentRetry();
                stateManager->save(sd);
                return true;
            } else {
                // Still within 7-day window, skip for now
                LOG_DEBUGF("[FileUploader] Pending folder still within 7-day window: %s", folderName.c_str());
                stateManager->clearCurrentRetry();
                return true;  // Don't increment retry count
            }
        } else {
            // First time seeing this empty folder - mark as pending
            stateManager->markFolderPending(folderName, currentTime);
            LOG_DEBUGF("[FileUploader] Marked empty folder as pending: %s", folderName.c_str());
            stateManager->clearCurrentRetry();
            stateManager->save(sd);
            return true;
        }
    }
    
    // Check if this is a recently completed folder being re-scanned
    bool isRecentRescan = stateManager->isFolderCompleted(folderName) && isRecentFolder(folderName);
    
    // Upload each file
    int uploadedCount = 0;
    int skippedUnchanged = 0;
    for (const String& fileName : files) {
        // Check for periodic SD card release before each file
        if (!checkAndReleaseSD(sdManager)) {
            LOG_ERROR("[FileUploader] Failed to retake SD card control during folder upload");
            stateManager->incrementCurrentRetryCount();
            stateManager->save(sd);
            return false;
        }
        
        // Check time budget before uploading
        String localPath = folderPath + "/" + fileName;
        
        // For recent folder re-scans, check if file has changed via checksum
        if (isRecentRescan) {
            if (!stateManager->hasFileChanged(sd, localPath)) {
                skippedUnchanged++;
                continue;  // File unchanged since last upload
            }
            LOG_DEBUGF("[FileUploader] File changed in recent folder: %s", fileName.c_str());
        }
        
        // Get file size
        File file = sd.open(localPath);
        if (!file) {
            LOG_ERRORF("[FileUploader] Cannot open file for reading: %s", localPath.c_str());
            LOG_ERROR("[FileUploader] File may be corrupted or SD card has read errors");
            LOG_WARN("[FileUploader] Skipping this file and continuing with next file");
            continue;  // Skip this file but continue with others
        }
        
        unsigned long fileSize = file.size();
        
        // Sanity check file size
        if (fileSize == 0) {
            LOG_WARNF("[FileUploader] File is empty: %s", localPath.c_str());
            file.close();
            // Mark empty file as processed to avoid re-scanning
            // Use a special checksum marker for empty files
            stateManager->markFileUploaded(localPath, "empty_file");
            continue;  // Skip empty files
        }
        
        file.close();
        
        // Check if we have budget for this file
        if (!budgetManager->canUploadFile(fileSize)) {
            LOG("[FileUploader] Insufficient time budget for remaining files");
            LOGF("[FileUploader] Successfully uploaded %d of %d files before budget exhaustion", uploadedCount, files.size());
            LOG("[FileUploader] This is normal - upload will resume in next session");
            
            // Increment retry count for this folder (partial upload)
            stateManager->incrementCurrentRetryCount();
            if (!stateManager->save(sd)) {
                LOG("[FileUploader] WARNING: Failed to save state after partial upload");
            }
            return false;  // Session interrupted due to budget
        }
        
        // Lazily create cloud import on first actual upload (avoids empty imports)
        if (!ensureCloudImport()) {
            LOG_WARN("[FileUploader] Cloud import not available, skipping cloud uploads this session");
        }
        
        // Upload the file
        String remotePath = folderPath + "/" + fileName;
        unsigned long bytesTransferred = 0;
        unsigned long uploadStartTime = millis();
        
        // Log file upload with retry information if applicable
        int currentRetryCount = stateManager->getCurrentRetryCount();
        if (currentRetryCount > 0) {
            LOGF("[FileUploader] RETRY ATTEMPT %d: Uploading file: %s (%lu bytes)", 
                 currentRetryCount + 1, fileName.c_str(), fileSize);
        } else {
            LOGF("[FileUploader] Uploading file: %s (%lu bytes)", fileName.c_str(), fileSize);
        }
        
        bool uploadSuccess = true;
        bool anyBackendConfigured = false;
        
        // Upload to all active backends
#ifdef ENABLE_SMB_UPLOAD
        if (smbUploader && config->hasSmbEndpoint()) {
            anyBackendConfigured = true;
            if (!smbUploader->isConnected()) {
                LOG_DEBUG("[FileUploader] SMB not connected, attempting to connect...");
                if (!smbUploader->begin()) {
                    LOG_ERROR("[FileUploader] Failed to connect to SMB share");
                    stateManager->incrementCurrentRetryCount();
                    stateManager->save(sd);
                    return false;
                }
            }
            unsigned long smbBytes = 0;
            if (!smbUploader->upload(localPath, remotePath, sd, smbBytes)) {
                LOG_ERRORF("[FileUploader] SMB upload failed for: %s", localPath.c_str());
                uploadSuccess = false;
            } else {
                bytesTransferred = smbBytes;
            }
        }
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
        if (webdavUploader && config->getEndpointType() == "WEBDAV") {
            anyBackendConfigured = true;
            if (!webdavUploader->isConnected()) {
                LOG_DEBUG("[FileUploader] WebDAV not connected, attempting to connect...");
                if (!webdavUploader->begin()) {
                    LOG_ERROR("[FileUploader] Failed to connect to WebDAV server");
                    stateManager->incrementCurrentRetryCount();
                    stateManager->save(sd);
                    return false;
                }
            }
            unsigned long davBytes = 0;
            if (!webdavUploader->upload(localPath, remotePath, sd, davBytes)) {
                LOG_ERRORF("[FileUploader] WebDAV upload failed for: %s", localPath.c_str());
                uploadSuccess = false;
            } else if (bytesTransferred == 0) {
                bytesTransferred = davBytes;
            }
        }
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
        if (sleephqUploader && config->hasCloudEndpoint()) {
            anyBackendConfigured = true;
            if (!sleephqUploader->isConnected()) {
                LOG_DEBUG("[FileUploader] Cloud not connected, attempting to connect...");
                if (!sleephqUploader->begin()) {
                    LOG_ERROR("[FileUploader] Failed to connect to cloud service");
                    stateManager->incrementCurrentRetryCount();
                    stateManager->save(sd);
                    return false;
                }
            }
            unsigned long cloudBytes = 0;
            if (!sleephqUploader->upload(localPath, remotePath, sd, cloudBytes)) {
                LOG_ERRORF("[FileUploader] Cloud upload failed for: %s", localPath.c_str());
                uploadSuccess = false;
            } else if (bytesTransferred == 0) {
                bytesTransferred = cloudBytes;
            }
        }
#endif
        
        if (!anyBackendConfigured) {
            LOG_ERROR("[FileUploader] No uploader available for configured endpoint type");
            stateManager->incrementCurrentRetryCount();
            stateManager->save(sd);
            return false;
        }
        
        if (!uploadSuccess) {
            LOG_ERRORF("[FileUploader] Failed to upload file: %s", localPath.c_str());
            LOG_WARNF("[FileUploader] Successfully uploaded %d files before failure", uploadedCount);
            
            // Don't mark folder as completed, will retry
            stateManager->incrementCurrentRetryCount();
            if (!stateManager->save(sd)) {
                LOG_WARN("[FileUploader] Failed to save state after upload error");
            }
            return false;  // Stop processing this folder
        }
        
        // Record upload for transmission rate calculation (skip small files < 5KB)
        unsigned long uploadTime = millis() - uploadStartTime;
        if (bytesTransferred >= 5120) {  // 5KB minimum for rate calculation
            budgetManager->recordUpload(bytesTransferred, uploadTime);
        }
        
        // Store per-file checksum so hasFileChanged() can detect changes on re-scan
        String checksum = stateManager->calculateChecksum(sd, localPath);
        if (!checksum.isEmpty()) {
            stateManager->markFileUploaded(localPath, checksum);
        }
        
        uploadedCount++;
        LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), bytesTransferred);
        LOG_DEBUGF("[FileUploader] Budget remaining: %lu ms", budgetManager->getRemainingBudgetMs());
    }
    
    // All files processed successfully
    if (isRecentRescan) {
        LOGF("[FileUploader] Re-scan complete: %d uploaded, %d unchanged in folder", uploadedCount, skippedUnchanged);
    } else {
        LOGF("[FileUploader] Successfully uploaded all %d files in folder", uploadedCount);
    }
    
    // Mark folder as completed
    stateManager->markFolderCompleted(folderName);
    
    // Reset retry count for this folder
    stateManager->clearCurrentRetry();
    
    // Save state
    stateManager->save(sd);
    
    return true;
}

// Upload a single file (for root and SETTINGS files)
bool FileUploader::uploadSingleFile(SDCardManager* sdManager, const String& filePath) {
    fs::FS &sd = sdManager->getFS();
    LOGF("[FileUploader] Uploading single file: %s", filePath.c_str());
    
    // Check if file exists
    if (!sd.exists(filePath)) {
        LOG_ERRORF("[FileUploader] File does not exist: %s", filePath.c_str());
        LOG_WARN("[FileUploader] File may have been deleted or SD card structure changed");
        return false;
    }
    
    // Get file size
    File file = sd.open(filePath);
    if (!file) {
        LOG_ERRORF("[FileUploader] Cannot open file for reading: %s", filePath.c_str());
        LOG_ERROR("[FileUploader] File may be corrupted or SD card has read errors");
        return false;
    }
    
    unsigned long fileSize = file.size();
    
    // Sanity check file size
    if (fileSize == 0) {
        LOG_WARNF("[FileUploader] File is empty: %s", filePath.c_str());
        file.close();
        return true;  // Consider empty file as "uploaded" (skip it)
    }
    
    file.close();
    
    // Check if we have budget for this file
    if (!budgetManager->canUploadFile(fileSize)) {
        LOGF("[FileUploader] Insufficient time budget for file: %s", filePath.c_str());
        LOG("[FileUploader] File will be uploaded in next session");
        return false;  // Not an error, just out of budget
    }
    
    // Check if file has changed (checksum comparison)
    if (!stateManager->hasFileChanged(sd, filePath)) {
        LOG_DEBUG("[FileUploader] File unchanged, skipping upload");
        return true;  // Not an error, just no need to upload
    }
    
    // Lazily create cloud import on first actual upload (avoids empty imports)
    if (!ensureCloudImport()) {
        LOG_WARN("[FileUploader] Cloud import not available, skipping cloud uploads this session");
    }
    
    // Upload the file
    unsigned long bytesTransferred = 0;
    unsigned long uploadStartTime = millis();
    
    bool uploadSuccess = true;
    bool anyBackendConfigured = false;
    
    // Upload to all active backends
#ifdef ENABLE_SMB_UPLOAD
    if (smbUploader && config->hasSmbEndpoint()) {
        anyBackendConfigured = true;
        if (!smbUploader->isConnected()) {
            LOG_DEBUG("[FileUploader] SMB not connected, attempting to connect...");
            if (!smbUploader->begin()) {
                LOG_ERROR("[FileUploader] Failed to connect to SMB share");
                return false;
            }
        }
        unsigned long smbBytes = 0;
        if (!smbUploader->upload(filePath, filePath, sd, smbBytes)) {
            LOG_ERRORF("[FileUploader] SMB upload failed for: %s", filePath.c_str());
            uploadSuccess = false;
        } else {
            bytesTransferred = smbBytes;
        }
    }
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
    if (webdavUploader && config->getEndpointType() == "WEBDAV") {
        anyBackendConfigured = true;
        if (!webdavUploader->isConnected()) {
            if (!webdavUploader->begin()) {
                LOG_ERROR("[FileUploader] Failed to connect to WebDAV server");
                return false;
            }
        }
        unsigned long davBytes = 0;
        if (!webdavUploader->upload(filePath, filePath, sd, davBytes)) {
            uploadSuccess = false;
        } else if (bytesTransferred == 0) {
            bytesTransferred = davBytes;
        }
    }
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (sleephqUploader && config->hasCloudEndpoint()) {
        anyBackendConfigured = true;
        if (!sleephqUploader->isConnected()) {
            LOG_DEBUG("[FileUploader] Cloud not connected, attempting to connect...");
            if (!sleephqUploader->begin()) {
                LOG_ERROR("[FileUploader] Failed to connect to cloud service");
                return false;
            }
        }
        unsigned long cloudBytes = 0;
        if (!sleephqUploader->upload(filePath, filePath, sd, cloudBytes)) {
            LOG_ERRORF("[FileUploader] Cloud upload failed for: %s", filePath.c_str());
            uploadSuccess = false;
        } else if (bytesTransferred == 0) {
            bytesTransferred = cloudBytes;
        }
    }
#endif
    
    if (!anyBackendConfigured) {
        LOG_ERROR("[FileUploader] No uploader available for configured endpoint type");
        return false;
    }
    
    if (!uploadSuccess) {
        LOG_ERROR("[FileUploader] Failed to upload file to one or more backends");
        return false;
    }
    
    // Record upload for transmission rate calculation (skip small files < 5KB)
    unsigned long uploadTime = millis() - uploadStartTime;
    if (bytesTransferred >= 5120) {  // 5KB minimum for rate calculation
        budgetManager->recordUpload(bytesTransferred, uploadTime);
    }
    
    // Calculate and store checksum so hasFileChanged() won't flag this file next session
    String checksum = stateManager->calculateChecksum(sd, filePath);
    if (!checksum.isEmpty()) {
        stateManager->markFileUploaded(filePath, checksum);
        stateManager->save(sd);
    }
    
    LOGF("[FileUploader] Successfully uploaded: %s (%lu bytes)", filePath.c_str(), bytesTransferred);
    
    return true;
}
// Perform delta scan - compare remote vs local file counts and re-upload if different
bool FileUploader::performDeltaScan(SDCardManager* sdManager) {
    LOG("[FileUploader] Starting delta scan - comparing remote vs local file counts");
    
    if (!wifiManager || !wifiManager->isConnected()) {
        LOG_ERROR("[FileUploader] WiFi not connected - cannot perform delta scan");
        return false;
    }
    
    fs::FS &sd = sdManager->getFS();
    
    // Only support SMB for now (can be extended for other protocols)
#ifdef ENABLE_SMB_UPLOAD
    if (!smbUploader || !config->hasSmbEndpoint()) {
        LOG_ERROR("[FileUploader] Delta scan only supported for SMB endpoints");
        return false;
    }
    
    // Ensure SMB connection is established
    if (!smbUploader->isConnected()) {
        LOG("[FileUploader] Connecting to SMB share for delta scan...");
        if (!smbUploader->begin()) {
            LOG_ERROR("[FileUploader] Failed to connect to SMB share for delta scan");
            return false;
        }
    }
#else
    LOG_ERROR("[FileUploader] Delta scan requires SMB support (compile with -DENABLE_SMB_UPLOAD)");
    return false;
#endif
    
    // Scan all DATALOG folders
    std::vector<String> datalogFolders = scanDatalogFolders(sd, true);
    
    int foldersChecked = 0;
    int foldersWithDifferences = 0;
    std::vector<String> foldersToReupload;
    
    LOG_INFOF("[FileUploader] Checking %d DATALOG folders for differences", datalogFolders.size());
    
    for (const String& folderName : datalogFolders) {
        // Check for periodic SD card release
        if (!checkAndReleaseSD(sdManager)) {
            LOG_ERROR("[FileUploader] Failed to retake SD card control during delta scan");
            return false;
        }
        
        String localFolderPath = "/DATALOG/" + folderName;
        String remoteFolderPath = "/DATALOG/" + folderName;
        
        // Count local files
        std::vector<String> localFiles = scanFolderFiles(sd, localFolderPath);
        int localFileCount = localFiles.size();
        
        // Count remote files
#ifdef ENABLE_SMB_UPLOAD
        int remoteFileCount = smbUploader->countRemoteFiles(remoteFolderPath);
#else
        int remoteFileCount = -1;
#endif
        
        foldersChecked++;
        
        if (remoteFileCount < 0) {
            LOG_INFOF("[FileUploader] Could not access remote folder: %s (may not exist)", remoteFolderPath.c_str());
            // If remote folder doesn't exist, we need to upload
            if (localFileCount > 0) {
                LOG_INFOF("[FileUploader] Local folder has %d files, remote doesn't exist - marking for re-upload", localFileCount);
                foldersWithDifferences++;
                foldersToReupload.push_back(folderName);
            }
        } else if (localFileCount != remoteFileCount) {
            LOG_INFOF("[FileUploader] File count mismatch in %s: local=%d, remote=%d - marking for re-upload", 
                      folderName.c_str(), localFileCount, remoteFileCount);
            foldersWithDifferences++;
            foldersToReupload.push_back(folderName);
        } else {
            LOG_INFOF("[FileUploader] File count matches in %s: %d files", folderName.c_str(), localFileCount);
        }
        
        // Yield to prevent watchdog timeout
        yield();
    }
    
    LOG_INFOF("[FileUploader] Delta scan complete: checked %d folders, found %d with differences", 
              foldersChecked, foldersWithDifferences);
    
    if (foldersWithDifferences == 0) {
        LOG("[FileUploader] No differences found - all folders match remote");
        return true;
    }
    
    // Mark folders for re-upload by removing them from completed state
    LOG_INFOF("[FileUploader] Marking %d folders for re-upload", foldersWithDifferences);
    
    for (const String& folderName : foldersToReupload) {
        LOG_INFOF("[FileUploader] Processing folder for re-upload: %s", folderName.c_str());
        
        if (stateManager->isFolderCompleted(folderName)) {
            LOG_INFOF("[FileUploader] Removing completed status from folder: %s", folderName.c_str());
            stateManager->removeFolderFromCompleted(folderName);
        } else {
            LOG_INFOF("[FileUploader] Folder %s was not in completed status", folderName.c_str());
        }
        
        // Also remove from pending if it was there
        if (stateManager->isPendingFolder(folderName)) {
            LOG_INFOF("[FileUploader] Removing pending status from folder: %s", folderName.c_str());
            stateManager->removeFolderFromPending(folderName);
        }
    }
    
    // Save state changes
    if (!stateManager->save(sd)) {
        LOG_WARN("[FileUploader] Failed to save state after delta scan");
    }
    
    LOG_INFOF("[FileUploader] Delta scan complete: %d folders marked for re-upload", foldersWithDifferences);
    LOG("[FileUploader] Folders will be re-uploaded in the next upload session");
    
    return true;
}

// Perform deep scan - compare remote vs local file sizes and re-upload if different
bool FileUploader::performDeepScan(SDCardManager* sdManager) {
    LOG("[FileUploader] Starting deep scan - comparing remote vs local file sizes");
    
    if (!wifiManager || !wifiManager->isConnected()) {
        LOG_ERROR("[FileUploader] WiFi not connected - cannot perform deep scan");
        return false;
    }
    
    fs::FS &sd = sdManager->getFS();
    
    // Only support SMB for now (can be extended for other protocols)
#ifdef ENABLE_SMB_UPLOAD
    if (!smbUploader || !config->hasSmbEndpoint()) {
        LOG_ERROR("[FileUploader] Deep scan only supported for SMB endpoints");
        return false;
    }
    
    // Ensure SMB connection is established
    if (!smbUploader->isConnected()) {
        LOG("[FileUploader] Connecting to SMB share for deep scan...");
        if (!smbUploader->begin()) {
            LOG_ERROR("[FileUploader] Failed to connect to SMB share for deep scan");
            return false;
        }
    }
#else
    LOG_ERROR("[FileUploader] Deep scan requires SMB support (compile with -DENABLE_SMB_UPLOAD)");
    return false;
#endif
    
    // Scan all DATALOG folders (including completed ones for deep scan)
    std::vector<String> datalogFolders = scanDatalogFolders(sd, true);
    
    int foldersChecked = 0;
    int foldersWithDifferences = 0;
    int filesWithSizeDifferences = 0;
    std::vector<String> foldersToReupload;
    
    LOG_INFOF("[FileUploader] Checking %d DATALOG folders for file size differences", datalogFolders.size());
    
    for (const String& folderName : datalogFolders) {
        // Check for periodic SD card release
        if (!checkAndReleaseSD(sdManager)) {
            LOG_ERROR("[FileUploader] Failed to retake SD card control during deep scan");
            return false;
        }
        
        String localFolderPath = "/DATALOG/" + folderName;
        String remoteFolderPath = "/DATALOG/" + folderName;
        
        // Get local file information
        std::vector<String> localFiles = scanFolderFiles(sd, localFolderPath);
        std::map<String, size_t> localFileInfo;
        
        for (const String& fileName : localFiles) {
            String fullLocalPath = localFolderPath + "/" + fileName;
            File localFile = sd.open(fullLocalPath, "r");
            if (localFile) {
                localFileInfo[fileName] = localFile.size();
                localFile.close();
            } else {
                LOG_WARN("[FileUploader] Could not open local file for size check: " + fullLocalPath);
            }
        }
        
        // Get remote file information
#ifdef ENABLE_SMB_UPLOAD
        std::map<String, size_t> remoteFileInfo;
        bool remoteSuccess = smbUploader->getRemoteFileInfo(remoteFolderPath, remoteFileInfo);
#else
        bool remoteSuccess = false;
        std::map<String, size_t> remoteFileInfo;
#endif
        
        foldersChecked++;
        bool folderHasDifferences = false;
        int folderFileDifferences = 0;
        
        if (!remoteSuccess) {
            LOG_DEBUGF("[FileUploader] Could not access remote folder: %s", remoteFolderPath.c_str());
            // If we can't access remote folder, mark for re-upload if local has files
            if (!localFileInfo.empty()) {
                LOG_DEBUGF("[FileUploader] Local folder has %d files, remote inaccessible - marking for re-upload", localFileInfo.size());
                folderHasDifferences = true;
            }
        } else {
            // Compare file counts first
            if (localFileInfo.size() != remoteFileInfo.size()) {
                LOG_DEBUGF("[FileUploader] File count mismatch in %s: local=%d, remote=%d", 
                          folderName.c_str(), localFileInfo.size(), remoteFileInfo.size());
                folderHasDifferences = true;
            } else {
                // Compare individual file sizes
                for (const auto& localFile : localFileInfo) {
                    const String& fileName = localFile.first;
                    size_t localSize = localFile.second;
                    
                    auto remoteIt = remoteFileInfo.find(fileName);
                    if (remoteIt == remoteFileInfo.end()) {
                        LOG_DEBUGF("[FileUploader] File missing on remote: %s/%s (%u bytes)", 
                                  folderName.c_str(), fileName.c_str(), localSize);
                        folderHasDifferences = true;
                        folderFileDifferences++;
                    } else {
                        size_t remoteSize = remoteIt->second;
                        if (localSize != remoteSize) {
                            LOG_INFOF("[FileUploader] File size mismatch: %s/%s local=%u bytes, remote=%u bytes", 
                                      folderName.c_str(), fileName.c_str(), localSize, remoteSize);
                            folderHasDifferences = true;
                            folderFileDifferences++;
                        }
                    }
                }
                
                // Check for files that exist on remote but not local (shouldn't happen normally)
                for (const auto& remoteFile : remoteFileInfo) {
                    const String& fileName = remoteFile.first;
                    if (localFileInfo.find(fileName) == localFileInfo.end()) {
                        LOG_DEBUGF("[FileUploader] Extra file on remote: %s/%s (%u bytes)", 
                                  folderName.c_str(), fileName.c_str(), remoteFile.second);
                        // This is unusual but not necessarily an error - don't mark for re-upload
                    }
                }
            }
        }
        
        if (folderHasDifferences) {
            foldersWithDifferences++;
            filesWithSizeDifferences += folderFileDifferences;
            foldersToReupload.push_back(folderName);
            LOG_DEBUGF("[FileUploader] Folder %s has differences (%d files affected)", 
                      folderName.c_str(), folderFileDifferences);
        } else {
            LOG_INFOF("[FileUploader] File sizes match in %s: %d files", folderName.c_str(), localFileInfo.size());
        }
        
        // Yield to prevent watchdog timeout
        yield();
    }
    
    LOG_INFOF("[FileUploader] Deep scan complete: checked %d folders, found %d with differences (%d files affected)", 
              foldersChecked, foldersWithDifferences, filesWithSizeDifferences);
    
    if (foldersWithDifferences == 0) {
        LOG("[FileUploader] No differences found - all file sizes match remote");
        return true;
    }
    
    // Mark folders for re-upload by removing them from completed state
    LOG_DEBUGF("[FileUploader] Marking %d folders for re-upload due to file size differences", foldersWithDifferences);
    
    for (const String& folderName : foldersToReupload) {
        if (stateManager->isFolderCompleted(folderName)) {
            LOG_INFOF("[FileUploader] Removing completed status from folder: %s", folderName.c_str());
            stateManager->removeFolderFromCompleted(folderName);
        }
        
        // Also remove from pending if it was there
        if (stateManager->isPendingFolder(folderName)) {
            LOG_INFOF("[FileUploader] Removing pending status from folder: %s", folderName.c_str());
            stateManager->removeFolderFromPending(folderName);
        }
    }
    
    // Save state changes
    if (!stateManager->save(sd)) {
        LOG_WARN("[FileUploader] Failed to save state after deep scan");
    }
    
    LOG_INFOF("[FileUploader] Deep scan complete: %d folders marked for re-upload (%d files with size differences)", 
              foldersWithDifferences, filesWithSizeDifferences);
    LOG("[FileUploader] Folders will be re-uploaded in the next upload session");
    
    return true;
}