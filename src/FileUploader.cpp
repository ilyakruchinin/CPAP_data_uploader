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
      cloudImportCreated(false),
      cloudImportFailed(false),
      companionFilesUploaded(false)
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
bool FileUploader::begin(fs::FS &sd, SDCardManager* sdManager) {
    LOG("[FileUploader] Initializing components...");
    
    // Initialize UploadStateManager — needs SD for state file
    stateManager = new UploadStateManager();
    if (!stateManager->begin(sd)) {
        LOG("[FileUploader] WARNING: Failed to load upload state, starting fresh");
        // Continue anyway - stateManager will work with empty state
    }
    
    // Release SD — everything below is in-memory or network (NTP sync).
    // NTP sync can take 20-30 seconds; CPAP needs SD access during this time.
    if (sdManager && sdManager->hasControl()) {
        sdManager->releaseControl();
        LOG_DEBUG("[FileUploader] SD released before NTP sync");
    }
    
    // Initialize TimeBudgetManager
    budgetManager = new TimeBudgetManager();
    
    // Initialize ScheduleManager — includes NTP sync (network-only, ~20s)
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
    if (config->hasWebdavEndpoint()) {
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
    
    LOG("[FileUploader] Periodic SD card release - giving CPAP priority access");
    
    // Pause active time tracking (only if an upload session is active)
    bool hasActiveSession = budgetManager->getRemainingBudgetMs() > 0 || budgetManager->getActiveTimeMs() > 0;
    if (hasActiveSession) {
        budgetManager->pauseActiveTime();
    }
    
    // Release SD card
    sdManager->releaseControl();
    
    // Wait configured time, handling web requests during the wait
    unsigned long waitMs = config->getSdReleaseWaitMs();
    LOGF("[FileUploader] CPAP has %lu ms to use SD card before retake...", waitMs);
    
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
    LOG("[FileUploader] Attempting to retake SD card control...");
    if (!sdManager->takeControl()) {
        LOG_ERROR("[FileUploader] Failed to retake SD card control");
        LOG_WARN("[FileUploader] CPAP machine may be actively using SD card - aborting to avoid data corruption");
        return false;  // Abort upload
    }
    
    // Resume active time tracking (only if an upload session is active)
    if (hasActiveSession) {
        budgetManager->resumeActiveTime();
    }
    
    // Reset release timer
    lastSdReleaseTime = millis();
    
    LOG("[FileUploader] SD card control reacquired, resuming upload");
    return true;
}

// Ensure SD card is held (retake if a backend released it during network I/O),
// then give CPAP a guaranteed access window between file uploads.
// This is called AFTER each file upload completes, before the next file.
bool FileUploader::ensureSdAndReleaseBetweenFiles(SDCardManager* sdManager) {
    // If SD was released by a backend (e.g. SleepHQ during HTTP POST),
    // CPAP already had access during that time. Just retake.
    if (!sdManager->hasControl()) {
        LOG_DEBUG("[FileUploader] Retaking SD card after backend released it");
        if (!sdManager->takeControl()) {
            LOG_ERROR("[FileUploader] Failed to retake SD card after backend release");
            return false;
        }
    }
    
    // Pause budget during CPAP window
    bool hasActiveSession = budgetManager->getRemainingBudgetMs() > 0 || budgetManager->getActiveTimeMs() > 0;
    if (hasActiveSession) {
        budgetManager->pauseActiveTime();
    }
    
    // Release SD card to give CPAP a guaranteed window between files
    sdManager->releaseControl();
    
    unsigned long waitMs = config->getSdReleaseWaitMs();
    LOG_DEBUGF("[FileUploader] CPAP access window: %lu ms between files", waitMs);
    
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
    
    // Retake SD for next file
    if (!sdManager->takeControl()) {
        LOG_ERROR("[FileUploader] Failed to retake SD card after inter-file CPAP window");
        if (hasActiveSession) {
            budgetManager->resumeActiveTime();
        }
        return false;
    }
    
    if (hasActiveSession) {
        budgetManager->resumeActiveTime();
    }
    
    // Reset the periodic release timer so checkAndReleaseSD doesn't
    // trigger immediately after this inter-file release
    lastSdReleaseTime = millis();
    
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
    
    // Reset SD card statistics for this session
    sdManager->resetStatistics();
    
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
    // Note: datalogFolders may include recent completed folders (RECENT_FOLDER_DAYS)
    // that are re-scanned for changes. Exclude those to avoid double-counting.
    int newInScan = 0;
    for (const String& f : datalogFolders) {
        if (!stateManager->isFolderCompleted(f) && !stateManager->isPendingFolder(f)) {
            newInScan++;
        }
    }
    stateManager->setTotalFoldersCount(newInScan + stateManager->getCompletedFoldersCount() + stateManager->getPendingFoldersCount());
    
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
    
    // Free TLS connection between phases — the ~32KB TLS buffers + 49KB batch
    // buffer can't coexist with SD_MMC FAT mount buffers. Phase 2 will reconnect.
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (sleephqUploader) {
        sleephqUploader->disconnectTls();
    }
#endif
    
    // Phase 2: Process root and SETTINGS files (if budget remains)
    if (budgetManager->hasBudget()) {
        LOG("[FileUploader] Phase 2: Processing root and SETTINGS files");
        
        // Ensure SD is mounted — Phase 1 may have lost SD (transient mount failure).
        // Without SD, scanRootAndSettingsFiles finds 0 files and the import
        // gets processed without mandatory companion files → SleepHQ rejects it.
        bool phase2SdOk = sdManager->hasControl();
        if (!phase2SdOk) {
            LOG_WARN("[FileUploader] SD not held after Phase 1, retaking for Phase 2...");
            if (sdManager->takeControl()) {
                phase2SdOk = true;
            } else {
                delay(1000);  // Transient mount failures often clear after brief wait
                if (sdManager->takeControl()) {
                    phase2SdOk = true;
                }
            }
            if (phase2SdOk) {
                LOG("[FileUploader] SD retaken for Phase 2");
            } else {
                LOG_ERROR("[FileUploader] Cannot retake SD for Phase 2, skipping root/SETTINGS files");
            }
        }
        
        if (phase2SdOk) do {
        // When a cloud import is active, force-include all root/SETTINGS files
        // even if unchanged locally. SleepHQ requires STR.edf, Identification.*,
        // and SETTINGS/ alongside DATALOG data to process an import.
        bool forceForCloud = cloudImportCreated;
        std::vector<String> rootSettingsFiles = scanRootAndSettingsFiles(sd, forceForCloud);
        
        if (!forceForCloud && !rootSettingsFiles.empty() && config->hasCloudEndpoint()) {
            LOG("[FileUploader] Root files changed - force-including all companion files for cloud import");
            forceForCloud = true;
            rootSettingsFiles = scanRootAndSettingsFiles(sd, true);
        }
        
        LOGF("[FileUploader] %s%d root/SETTINGS files to upload%s",
             forceForCloud ? "Cloud import active - including all " : "Found ",
             (int)rootSettingsFiles.size(),
             forceForCloud ? " (forced for cloud import)" : "");
        
        // ─── Batch optimization for root/SETTINGS (cloud-only) ─────────
        bool rootBatchMode = false;
#ifdef ENABLE_SLEEPHQ_UPLOAD
        rootBatchMode = sleephqUploader && config->hasCloudEndpoint() && !cloudImportFailed && forceForCloud;
#endif
#ifdef ENABLE_SMB_UPLOAD
        if (smbUploader && config->hasSmbEndpoint()) rootBatchMode = false;
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
        if (webdavUploader && config->hasWebdavEndpoint()) rootBatchMode = false;
#endif
        
#ifdef ENABLE_SLEEPHQ_UPLOAD
        if (rootBatchMode) {
            if (!ensureCloudImport(sdManager)) {
                LOG_WARN("[FileUploader] Cloud not available, using per-file for root files");
                rootBatchMode = false;
            }
        }
        if (rootBatchMode) {
            const size_t ROOT_BATCH_SIZE = 49152;
            uint8_t* rootBuf = (uint8_t*)malloc(ROOT_BATCH_SIZE);
            if (!rootBuf) {
                LOG_WARN("[FileUploader] Root batch buffer alloc failed, using per-file");
                rootBatchMode = false;
            } else {
                struct RootBatchEntry {
                    String filePath;
                    String fileName;
                    size_t offset;
                    size_t size;
                };
                std::vector<RootBatchEntry> rootBatch;
                size_t rootOffset = 0;
                std::vector<String> largeFiles;
                
                // Phase A: Batch-read small files
                for (const String& fp : rootSettingsFiles) {
                    File f = sd.open(fp, FILE_READ);
                    if (!f) continue;
                    size_t fsize = f.size();
                    if (fsize == 0) { f.close(); continue; }
                    
                    String fn = fp;
                    int sl = fn.lastIndexOf('/');
                    if (sl >= 0) fn = fn.substring(sl + 1);
                    
                    if (fsize + rootOffset <= ROOT_BATCH_SIZE) {
                        size_t br = f.read(rootBuf + rootOffset, fsize);
                        f.close();
                        if (br == fsize) {
                            LOGF("[FileUploader] Batched root: %s (%u bytes)", fn.c_str(), fsize);
                            rootBatch.push_back({fp, fn, rootOffset, fsize});
                            rootOffset += fsize;
                        }
                    } else {
                        f.close();
                        largeFiles.push_back(fp);
                    }
                }
                
                // Phase B: Release SD, upload batch from RAM
                if (!rootBatch.empty()) {
                    LOGF("[FileUploader] Uploading root batch: %d files, %u bytes",
                         (int)rootBatch.size(), (unsigned)rootOffset);
                    if (sdManager->hasControl()) sdManager->releaseControl();
                    
                    if (!ensureCloudImport(sdManager)) {
                        LOG_WARN("[FileUploader] Cloud import not available");
                    }
                    
                    int rootUploaded = 0;
                    for (size_t ri = 0; ri < rootBatch.size(); ri++) {
                        auto& entry = rootBatch[ri];
                        unsigned long cBytes = 0;
                        if (sleephqUploader->uploadFromBuffer(
                                rootBuf + entry.offset, entry.size,
                                entry.fileName, entry.filePath, cBytes)) {
                            anyUploaded = true;
                            rootUploaded++;
                        } else {
                            LOG_ERRORF("[FileUploader] Root batch upload failed: %s", entry.filePath.c_str());
                            break;
                        }
                    }
                    
                    // Phase C: Pre-calculate checksums from buffer, then free buffer
                    // BEFORE remounting SD. Same heap pressure fix as DATALOG batch:
                    // 49KB buffer + 32KB TLS = insufficient heap for FAT mount.
                    std::vector<String> rootChecksums;
                    for (int ri = 0; ri < rootUploaded; ri++) {
                        auto& entry = rootBatch[ri];
                        rootChecksums.push_back(
                            stateManager->calculateChecksumFromBuffer(
                                rootBuf + entry.offset, entry.size));
                    }
                    free(rootBuf);
                    rootBuf = nullptr;
                    
                    if (!sdManager->hasControl()) {
                        if (!sdManager->takeControl()) {
                            LOG_ERROR("[FileUploader] Failed to retake SD after root batch");
                            if (rootUploaded > 0) companionFilesUploaded = true;
                            break;  // break out of do-while
                        }
                    }
                    
                    for (int ri = 0; ri < rootUploaded; ri++) {
                        auto& entry = rootBatch[ri];
                        if (!rootChecksums[ri].isEmpty()) {
                            stateManager->markFileUploaded(entry.filePath, rootChecksums[ri]);
                        }
                        LOGF("[FileUploader] Successfully uploaded: %s (%u bytes)", entry.filePath.c_str(), entry.size);
                    }
                    if (rootUploaded > 0) {
                        companionFilesUploaded = true;
                        stateManager->save(sd);
                    }
                }
                
                if (rootBuf) free(rootBuf);
                
                // Phase D: Handle large files via per-file path
                for (const String& fp : largeFiles) {
                    if (!budgetManager->hasBudget()) break;
                    if (!checkAndReleaseSD(sdManager)) break;
                    if (uploadSingleFile(sdManager, fp, forceForCloud)) {
                        anyUploaded = true;
                    }
                }
                
                break;  // break out of do-while (batch path done)
            }
        }
#endif
        
        // Per-file fallback (multi-backend or no cloud)
        for (const String& filePath : rootSettingsFiles) {
            if (!budgetManager->hasBudget()) {
                LOG("[FileUploader] Time budget exhausted during root/SETTINGS processing");
                break;
            }
            
            if (!checkAndReleaseSD(sdManager)) {
                LOG_ERROR("[FileUploader] Failed to retake SD card control, aborting upload");
                break;
            }
            
            if (uploadSingleFile(sdManager, filePath, forceForCloud)) {
                anyUploaded = true;
                if (forceForCloud) companionFilesUploaded = true;
            }
            
#ifdef ENABLE_TEST_WEBSERVER
            if (webServer) {
                webServer->handleClient();
            }
#endif
        }
        
        } while (0);  // end Phase 2 do-while block
    } else {
        LOG("[FileUploader] Skipping root/SETTINGS files - no budget remaining");
    }
    
    // Print SD card statistics before ending session
    sdManager->printStatistics();
    
    // End upload session and save state
    endUploadSession(sd, sdManager);
    
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
    // Note: datalogFolders may include recent completed folders (RECENT_FOLDER_DAYS)
    // that are re-scanned for changes. Exclude those to avoid double-counting.
    int newInScan = 0;
    for (const String& f : datalogFolders) {
        if (!stateManager->isFolderCompleted(f) && !stateManager->isPendingFolder(f)) {
            newInScan++;
        }
    }
    stateManager->setTotalFoldersCount(newInScan + stateManager->getCompletedFoldersCount() + stateManager->getPendingFoldersCount());
    
    LOG_DEBUGF("[FileUploader] Found %d incomplete folders", newInScan);
    LOG_DEBUGF("[FileUploader] Total folders: %d (completed: %d, incomplete: %d, pending: %d)", 
         stateManager->getCompletedFoldersCount() + newInScan + stateManager->getPendingFoldersCount(),
         stateManager->getCompletedFoldersCount(),
         newInScan,
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
    
    // Diagnostic: log current date and check for today's/yesterday's folders
    {
        time_t now = time(nullptr);
        if (now > 24 * 3600) {
            struct tm nowTm;
            localtime_r(&now, &nowTm);
            char todayStr[9];
            snprintf(todayStr, sizeof(todayStr), "%04d%02d%02d",
                     nowTm.tm_year + 1900, nowTm.tm_mon + 1, nowTm.tm_mday);
            
            time_t yesterday = now - 86400L;
            struct tm yesterdayTm;
            localtime_r(&yesterday, &yesterdayTm);
            char yesterdayStr[9];
            snprintf(yesterdayStr, sizeof(yesterdayStr), "%04d%02d%02d",
                     yesterdayTm.tm_year + 1900, yesterdayTm.tm_mon + 1, yesterdayTm.tm_mday);
            
            LOGF("[FileUploader] Current local date: %s, yesterday: %s", todayStr, yesterdayStr);
            
            String todayPath = "/DATALOG/" + String(todayStr);
            String yesterdayPath = "/DATALOG/" + String(yesterdayStr);
            LOGF("[FileUploader] Today's folder %s: %s", todayStr, 
                 sd.exists(todayPath) ? "EXISTS" : "NOT FOUND");
            LOGF("[FileUploader] Yesterday's folder %s: %s", yesterdayStr,
                 sd.exists(yesterdayPath) ? "EXISTS" : "NOT FOUND");
        }
    }
    
    // Diagnostic: raw listing of ALL entries in /DATALOG
    {
        File diagRoot = sd.open("/DATALOG");
        if (diagRoot && diagRoot.isDirectory()) {
            String allEntries = "";
            int entryCount = 0;
            File entry = diagRoot.openNextFile();
            while (entry) {
                String name = String(entry.name());
                int lastSlash = name.lastIndexOf('/');
                if (lastSlash >= 0) name = name.substring(lastSlash + 1);
                if (entryCount > 0) allEntries += ", ";
                allEntries += name;
                if (!entry.isDirectory()) allEntries += "(file)";
                entryCount++;
                entry.close();
                entry = diagRoot.openNextFile();
            }
            diagRoot.close();
            LOGF("[FileUploader] Raw /DATALOG listing (%d entries): %s", entryCount, allEntries.c_str());
        }
    }
    
    // Scan for folders
    int totalDirsOnCard = 0;
    int skippedOld = 0;
    int skippedCompleted = 0;
    File file = root.openNextFile();
    while (file) {
        if (file.isDirectory()) {
            String folderName = String(file.name());
            
            // Extract just the folder name (remove path prefix if present)
            int lastSlash = folderName.lastIndexOf('/');
            if (lastSlash >= 0) {
                folderName = folderName.substring(lastSlash + 1);
            }
            
            totalDirsOnCard++;
            
            // Apply MAX_DAYS filter (folder names are in YYYYMMDD format)
            if (!maxDaysCutoff.isEmpty() && folderName < maxDaysCutoff) {
                skippedOld++;
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
                    LOGF("[FileUploader] Re-checking recent completed folder: %s", folderName.c_str());
                } else {
                    skippedCompleted++;
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
                LOGF("[FileUploader] Found NEW incomplete DATALOG folder: %s", folderName.c_str());
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    
    // Log SD card folder summary for diagnostics
    LOGF("[FileUploader] SD card: %d total dirs in /DATALOG, %d skipped (old), %d skipped (completed non-recent), %d to process",
         totalDirsOnCard, skippedOld, skippedCompleted, (int)folders.size());
    
    // Sort folders by date (newest first) - folders are in YYYYMMDD format
    std::sort(folders.begin(), folders.end(), [](const String& a, const String& b) {
        return a > b;  // Descending order (newest first)
    });
    
    if (folders.empty()) {
        LOG("[FileUploader] No incomplete DATALOG folders found");
        LOG_DEBUG("[FileUploader] Either all folders are uploaded or DATALOG is empty");
    } else {
        LOGF("[FileUploader] Processing %d folders (newest first)", (int)folders.size());
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
    
    // Scan for .edf files (also log ALL files for diagnostics)
    int totalFiles = 0;
    String allFileNames = "";
    File file = folder.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String fileName = String(file.name());
            
            // Extract just the file name (remove path prefix if present)
            int lastSlash = fileName.lastIndexOf('/');
            if (lastSlash >= 0) {
                fileName = fileName.substring(lastSlash + 1);
            }
            
            // Log all files for diagnostics
            if (totalFiles > 0) allFileNames += ", ";
            allFileNames += fileName + "(" + String(file.size()) + ")";
            totalFiles++;
            
            // Check if it's an .edf file
            if (fileName.endsWith(".edf") || fileName.endsWith(".EDF")) {
                files.push_back(fileName);
            }
        }
        file.close();
        file = folder.openNextFile();
    }
    folder.close();
    
    LOGF("[FileUploader] %s: %d total files, %d .edf: [%s]", 
         folderPath.c_str(), totalFiles, (int)files.size(), allFileNames.c_str());
    
    return files;
}

// Scan root and SETTINGS files that need tracking
// When forceAll is true, include all existing files regardless of checksum state.
// This is needed for cloud imports where SleepHQ requires companion files
// (STR.edf, Identification.*, SETTINGS/) alongside DATALOG data.
std::vector<String> FileUploader::scanRootAndSettingsFiles(fs::FS &sd, bool forceAll) {
    std::vector<String> files;
    
    if (forceAll) {
        LOG("[FileUploader] Cloud import active - including all root/SETTINGS files");
    }
    
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
            if (forceAll || stateManager->hasFileChanged(sd, file)) {
                files.push_back(file);
                LOG_DEBUGF("[FileUploader] Root file %s: %s", forceAll ? "included" : "changed", file.c_str());
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
                
                if (forceAll || stateManager->hasFileChanged(sd, settingsPath)) {
                    files.push_back(settingsPath);
                    LOG_DEBUGF("[FileUploader] SETTINGS file %s: %s", forceAll ? "included" : "changed", settingsPath.c_str());
                }
            }
            settingsFile.close();
            settingsFile = settingsDir.openNextFile();
        }
        settingsDir.close();
    } else {
        LOG_DEBUG("[FileUploader] /SETTINGS directory not found or not accessible");
    }
    
    LOGF("[FileUploader] Found %d root/SETTINGS files to upload%s", files.size(), forceAll ? " (forced for cloud import)" : "");
    
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
bool FileUploader::ensureCloudImport(SDCardManager* sdManager) {
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (cloudImportCreated) return true;
    if (cloudImportFailed) return false;  // Already failed this session, don't retry
    if (!sleephqUploader || !config->hasCloudEndpoint()) return true;  // No cloud = OK
    
    // Cloud init is 100% network I/O (OAuth + team/device discovery + import creation).
    // Release SD so CPAP can write while we spend ~15s on network calls.
    bool releasedSd = false;
    if (sdManager && sdManager->hasControl()) {
        sdManager->releaseControl();
        releasedSd = true;
        LOG_DEBUG("[FileUploader] SD released for CPAP during cloud init");
    }
    
    bool ok = true;
    if (!sleephqUploader->isConnected()) {
        LOG("[FileUploader] Connecting cloud uploader for import session...");
        if (!sleephqUploader->begin()) {
            LOG_ERROR("[FileUploader] Failed to initialize cloud uploader");
            LOG_WARN("[FileUploader] Cloud uploads will be skipped this session");
            cloudImportFailed = true;
            ok = false;
        }
    }
    if (ok && sleephqUploader->isConnected()) {
        if (!sleephqUploader->createImport()) {
            LOG_ERROR("[FileUploader] Failed to create cloud import");
            LOG_WARN("[FileUploader] Cloud uploads will be skipped this session");
            cloudImportFailed = true;
            ok = false;
        } else {
            cloudImportCreated = true;
        }
    }
    
    // Retake SD if we released it
    if (releasedSd && sdManager && !sdManager->hasControl()) {
        if (!sdManager->takeControl()) {
            LOG_ERROR("[FileUploader] Failed to retake SD after cloud init");
            return false;
        }
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
    cloudImportFailed = false;
    companionFilesUploaded = false;
    
    return true;
}

// End upload session and save state
void FileUploader::endUploadSession(fs::FS &sd, SDCardManager* sdManager) {
    LOG("[FileUploader] Ending upload session");
    
    // Process cloud import if active
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (sleephqUploader && config->hasCloudEndpoint()) {
        if (!sleephqUploader->getCurrentImportId().isEmpty()) {
            // SleepHQ requires companion files (STR.edf, Identification.*,
            // SETTINGS/) alongside DATALOG data. If companion files weren't
            // uploaded in Phase 2 (budget exhausted, SD failure, etc.),
            // force-upload them now — they're small (~80KB total) and without
            // them the import will fail with "files missing".
            if (cloudImportCreated && !companionFilesUploaded) {
                LOG("[FileUploader] Companion files missing - force-uploading for cloud import");
                // Ensure SD is mounted for file reads
                if (sdManager && !sdManager->hasControl()) {
                    if (!sdManager->takeControl()) {
                        delay(1000);
                        sdManager->takeControl();  // Best-effort retry
                    }
                }
                if (sdManager && sdManager->hasControl()) {
                    std::vector<String> companionFiles = scanRootAndSettingsFiles(sd, true);
                    for (const String& filePath : companionFiles) {
                        unsigned long bytes = 0;
                        if (sleephqUploader->upload(filePath, filePath, sd, bytes)) {
                            LOGF("[FileUploader] Companion file uploaded: %s (%lu bytes)", filePath.c_str(), bytes);
                            companionFilesUploaded = true;
                        } else {
                            LOG_WARNF("[FileUploader] Failed to upload companion file: %s", filePath.c_str());
                        }
                    }
                } else {
                    LOG_ERROR("[FileUploader] Cannot mount SD for companion files");
                }
            }
            
            // Only process import if companion files were uploaded.
            // Without them, SleepHQ will reject the import. Leave it
            // unprocessed so the next session can add files and process.
            if (!cloudImportCreated || companionFilesUploaded) {
                // Release SD during processImport() — it's a pure network call,
                // no SD access needed. Avoids holding SD for 5+ seconds.
                if (sdManager && sdManager->hasControl()) {
                    sdManager->releaseControl();
                }
                
                if (!sleephqUploader->processImport()) {
                    LOG_WARN("[FileUploader] Failed to process cloud import");
                }
            } else {
                LOG_WARN("[FileUploader] Skipping processImport - companion files missing, import would fail");
                LOG("[FileUploader] Import left open for next session to complete");
            }
            
            // Retake SD for state save below
            if (sdManager && !sdManager->hasControl()) {
                sdManager->takeControl();
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
    
https://sleephq.com/api/swagger.json    // Log file count for diagnostics (helps debug overnight detection issues)
    LOGF("[FileUploader] Folder %s: %d .edf files on SD card%s", 
         folderName.c_str(), (int)files.size(), isRecentRescan ? " (recent re-scan)" : "");
    
    // Upload each file
    int uploadedCount = 0;
    int skippedUnchanged = 0;
    
    // ─── Batch upload optimization (cloud-only) ─────────────────────────
    // When only cloud is configured, batch small files into a single 48KB
    // buffer during one SD hold, then release SD and upload all from RAM.
    // Eliminates per-file SD mount/unmount overhead (~600ms each).
    const size_t BATCH_BUF_SIZE = 49152;  // 48KB
    bool cloudOnlyBatch = false;
    uint8_t* batchBuf = nullptr;
    size_t batchOffset = 0;
    
    struct BatchEntry {
        String localPath;
        String fileName;
        size_t offset;
        size_t size;
    };
    std::vector<BatchEntry> pendingBatch;
    
#ifdef ENABLE_SLEEPHQ_UPLOAD
    cloudOnlyBatch = sleephqUploader && config->hasCloudEndpoint() && !cloudImportFailed;
#endif
#ifdef ENABLE_SMB_UPLOAD
    if (smbUploader && config->hasSmbEndpoint()) cloudOnlyBatch = false;
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
    if (webdavUploader && config->hasWebdavEndpoint()) cloudOnlyBatch = false;
#endif
    
    // Note: batchBuf allocation is deferred until the first file actually
    // needs batching. This avoids stealing 48KB from heap before TLS/OAuth
    // has a chance to run — which would cause "heap too low" failures.
    
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
        
        // Get file size (metadata only — no content read)
        File file = sd.open(localPath);
        if (!file) {
            LOG_ERRORF("[FileUploader] Cannot open file for reading: %s", localPath.c_str());
            LOG_ERROR("[FileUploader] File may be corrupted or SD card has read errors");
            LOG_WARN("[FileUploader] Skipping this file and continuing with next file");
            continue;  // Skip this file but continue with others
        }
        
        unsigned long fileSize = file.size();
        file.close();
        
        // Sanity check file size
        if (fileSize == 0) {
            LOG_WARNF("[FileUploader] File is empty: %s", localPath.c_str());
            // Mark empty file as processed to avoid re-scanning
            stateManager->markFileUploadedWithSize(localPath, 0);
            continue;  // Skip empty files
        }
        
        // For recent folder re-scans, use size-based change detection.
        // DATALOG .edf files are append-only — if size hasn't changed,
        // the file hasn't changed. No content read needed.
        if (isRecentRescan) {
            if (!stateManager->hasFileSizeChanged(localPath, fileSize)) {
                skippedUnchanged++;
                continue;  // File unchanged since last upload
            }
            LOG_DEBUGF("[FileUploader] File size changed in recent folder: %s (%lu bytes)", fileName.c_str(), fileSize);
        }
        
        // Check if we have budget for this file
        if (!budgetManager->canUploadFile(fileSize)) {
            LOG("[FileUploader] Insufficient time budget for remaining files");
            
#ifdef ENABLE_SLEEPHQ_UPLOAD
            // Flush any pending batch before exiting
            if (!pendingBatch.empty()) {
                LOGF("[FileUploader] Flushing batch before budget exit: %d files", (int)pendingBatch.size());
                if (sdManager->hasControl()) sdManager->releaseControl();
                if (ensureCloudImport(sdManager)) {
                    for (size_t bi = 0; bi < pendingBatch.size(); bi++) {
                        BatchEntry& entry = pendingBatch[bi];
                        unsigned long cBytes = 0;
                        if (!sleephqUploader->uploadFromBuffer(
                                batchBuf + entry.offset, entry.size,
                                entry.fileName, entry.localPath, cBytes)) {
                            break;
                        }
                        uploadedCount++;
                    }
                }
                // Free batch buffer BEFORE remounting SD (same reason as mid-loop flush)
                if (batchBuf) { free(batchBuf); batchBuf = nullptr; }
                if (!sdManager->hasControl()) sdManager->takeControl();
                for (auto& entry : pendingBatch) {
                    stateManager->markFileUploadedWithSize(entry.localPath, entry.size);
                }
                pendingBatch.clear();
                batchOffset = 0;
            }
#endif
            if (batchBuf) { free(batchBuf); batchBuf = nullptr; }
            
            LOGF("[FileUploader] Uploaded %d of %d files in DATALOG/%s before budget exhaustion", uploadedCount, (int)files.size(), folderName.c_str());
            LOG("[FileUploader] This is normal - upload will resume in next session");
            
            // Increment retry count for this folder (partial upload)
            stateManager->incrementCurrentRetryCount();
            if (!stateManager->save(sd)) {
                LOG("[FileUploader] WARNING: Failed to save state after partial upload");
            }
            return false;  // Session interrupted due to budget
        }
        
#ifdef ENABLE_SLEEPHQ_UPLOAD
        // ─── Batch path: accumulate small cloud-only files in RAM ───
        if (cloudOnlyBatch && fileSize + batchOffset <= BATCH_BUF_SIZE) {
            if (!ensureCloudImport(sdManager)) {
                LOG_WARN("[FileUploader] Cloud import not available, disabling batch");
                cloudOnlyBatch = false;
            } else {
                // Lazy allocation: defer 48KB buffer until AFTER cloud/TLS init
                // succeeds, so TLS handshake has enough heap for its buffers.
                if (!batchBuf) {
                    batchBuf = (uint8_t*)malloc(BATCH_BUF_SIZE);
                    if (!batchBuf) {
                        LOG_WARN("[FileUploader] Batch buffer alloc failed, using per-file mode");
                        cloudOnlyBatch = false;
                    } else {
                        LOG_DEBUG("[FileUploader] Batch buffer allocated (48KB)");
                    }
                }
            }
            if (cloudOnlyBatch) {
                File f = sd.open(localPath, FILE_READ);
                if (!f) {
                    LOG_ERRORF("[FileUploader] Cannot read for batch: %s", localPath.c_str());
                    continue;
                }
                size_t bytesRead = f.read(batchBuf + batchOffset, fileSize);
                f.close();
                if (bytesRead != fileSize) {
                    LOG_ERRORF("[FileUploader] Short read batching %s", fileName.c_str());
                    continue;
                }
                
                LOGF("[FileUploader] Batched: %s (%lu bytes, buf %u/%u)",
                     fileName.c_str(), fileSize, (unsigned)(batchOffset + fileSize), BATCH_BUF_SIZE);
                pendingBatch.push_back({localPath, fileName, batchOffset, (size_t)fileSize});
                batchOffset += fileSize;
                continue;  // Accumulate more files before uploading
            }
        }
        
        // ─── Flush pending batch before handling this file individually ──
        if (!pendingBatch.empty()) {
            LOGF("[FileUploader] Flushing batch: %d files, %u bytes",
                 (int)pendingBatch.size(), (unsigned)batchOffset);
            if (sdManager->hasControl()) sdManager->releaseControl();
            
            bool batchOk = true;
            int batchUp = 0;
            for (size_t bi = 0; bi < pendingBatch.size(); bi++) {
                BatchEntry& entry = pendingBatch[bi];
                unsigned long cBytes = 0;
                unsigned long bStart = millis();
                if (!sleephqUploader->uploadFromBuffer(
                        batchBuf + entry.offset, entry.size,
                        entry.fileName, entry.localPath, cBytes)) {
                    LOG_ERRORF("[FileUploader] Batch upload failed: %s", entry.localPath.c_str());
                    batchOk = false;
                    break;
                }
                unsigned long bTime = millis() - bStart;
                if (cBytes >= 5120) budgetManager->recordUpload(cBytes, bTime);
                batchUp++;
            }
            
            // Free batch buffer BEFORE remounting SD — the 49KB buffer + 32KB TLS
            // leaves insufficient heap for SD_MMC FAT mount (caused 0x101 errors).
            // DATALOG state marking uses only entry.localPath/size, not buffer data.
            if (batchBuf) { free(batchBuf); batchBuf = nullptr; }
            
            if (!sdManager->hasControl()) {
                if (!sdManager->takeControl()) {
                    LOG_ERROR("[FileUploader] Failed to retake SD after batch");
                    return false;
                }
            }
            for (int bi = 0; bi < batchUp; bi++) {
                BatchEntry& entry = pendingBatch[bi];
                stateManager->markFileUploadedWithSize(entry.localPath, entry.size);
                uploadedCount++;
                LOGF("[FileUploader] Uploaded: %s (%u bytes)", entry.fileName.c_str(), entry.size);
            }
            if (batchUp > 0) stateManager->save(sd);
            pendingBatch.clear();
            batchOffset = 0;
            
            if (!batchOk) {
                stateManager->incrementCurrentRetryCount();
                stateManager->save(sd);
                return false;
            }
            if (!ensureSdAndReleaseBetweenFiles(sdManager)) {
                stateManager->incrementCurrentRetryCount();
                stateManager->save(sd);
                return false;
            }
        }
#endif
        
        // Lazily create cloud import on first actual upload (avoids empty imports)
        if (!ensureCloudImport(sdManager)) {
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
        if (webdavUploader && config->hasWebdavEndpoint()) {
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
        if (sleephqUploader && config->hasCloudEndpoint() && !cloudImportFailed) {
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
            if (!sleephqUploader->upload(localPath, remotePath, sd, cloudBytes, sdManager)) {
                LOG_ERRORF("[FileUploader] Cloud upload failed for: %s", localPath.c_str());
                uploadSuccess = false;
            } else if (bytesTransferred == 0) {
                bytesTransferred = cloudBytes;
            }
        }
#endif
        
        // Retake SD if a backend released it during network I/O
        if (!sdManager->hasControl()) {
            if (!sdManager->takeControl()) {
                LOG_ERROR("[FileUploader] Failed to retake SD after backend network I/O");
                return false;
            }
        }
        
        if (!anyBackendConfigured) {
            LOG_ERROR("[FileUploader] No uploader available for configured endpoint type");
            stateManager->incrementCurrentRetryCount();
            stateManager->save(sd);
            return false;
        }
        
        if (!uploadSuccess) {
            LOG_ERRORF("[FileUploader] Failed to upload file: %s", localPath.c_str());
            LOG_WARNF("[FileUploader] Uploaded %d files in DATALOG/%s before failure", uploadedCount, folderName.c_str());
            
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
        
        // Store file size for size-based change detection on re-scan.
        // DATALOG files are append-only — size comparison is sufficient,
        // no need to re-read the file for a hash (saves a full SD read).
        stateManager->markFileUploadedWithSize(localPath, fileSize);
        
        uploadedCount++;
        LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), bytesTransferred);
        LOG_DEBUGF("[FileUploader] Budget remaining: %lu ms", budgetManager->getRemainingBudgetMs());
        
        // Give CPAP a guaranteed SD access window between every file upload
        if (!ensureSdAndReleaseBetweenFiles(sdManager)) {
            LOG_ERROR("[FileUploader] Failed to retake SD after inter-file CPAP window");
            stateManager->incrementCurrentRetryCount();
            stateManager->save(sd);
            return false;
        }
    }
    
#ifdef ENABLE_SLEEPHQ_UPLOAD
    // Flush any remaining batch entries
    if (!pendingBatch.empty()) {
        LOGF("[FileUploader] Flushing final batch: %d files, %u bytes",
             (int)pendingBatch.size(), (unsigned)batchOffset);
        if (sdManager->hasControl()) sdManager->releaseControl();
        
        if (!ensureCloudImport(sdManager)) {
            LOG_WARN("[FileUploader] Cloud import not available");
        }
        
        bool batchOk = true;
        int batchUp = 0;
        for (size_t bi = 0; bi < pendingBatch.size(); bi++) {
            BatchEntry& entry = pendingBatch[bi];
            unsigned long cBytes = 0;
            unsigned long bStart = millis();
            if (!sleephqUploader->uploadFromBuffer(
                    batchBuf + entry.offset, entry.size,
                    entry.fileName, entry.localPath, cBytes)) {
                LOG_ERRORF("[FileUploader] Batch upload failed: %s", entry.localPath.c_str());
                batchOk = false;
                break;
            }
            unsigned long bTime = millis() - bStart;
            if (cBytes >= 5120) budgetManager->recordUpload(cBytes, bTime);
            batchUp++;
        }
        
        // Free batch buffer BEFORE remounting SD (same reason as mid-loop flush)
        if (batchBuf) { free(batchBuf); batchBuf = nullptr; }
        
        if (!sdManager->hasControl()) {
            if (!sdManager->takeControl()) {
                LOG_ERROR("[FileUploader] Failed to retake SD after final batch");
                return false;
            }
        }
        for (int bi = 0; bi < batchUp; bi++) {
            BatchEntry& entry = pendingBatch[bi];
            stateManager->markFileUploadedWithSize(entry.localPath, entry.size);
            uploadedCount++;
            LOGF("[FileUploader] Uploaded: %s (%u bytes)", entry.fileName.c_str(), entry.size);
        }
        if (batchUp > 0) stateManager->save(sd);
        pendingBatch.clear();
        batchOffset = 0;
        
        if (!batchOk) {
            stateManager->incrementCurrentRetryCount();
            stateManager->save(sd);
            return false;
        }
    }
#endif
    
    if (batchBuf) { free(batchBuf); batchBuf = nullptr; }
    
    // All files processed successfully
    if (isRecentRescan) {
        LOGF("[FileUploader] Re-scan of %s complete: %d uploaded, %d unchanged", folderName.c_str(), uploadedCount, skippedUnchanged);
    } else {
        LOGF("[FileUploader] Successfully uploaded all %d files in DATALOG/%s", uploadedCount, folderName.c_str());
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
// When forceUpload is true, skip the hasFileChanged check (used for cloud imports
// where SleepHQ needs companion files even if unchanged locally).
bool FileUploader::uploadSingleFile(SDCardManager* sdManager, const String& filePath, bool forceUpload) {
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
    // Skip this check when forceUpload is true (cloud imports need companion files)
    if (!forceUpload && !stateManager->hasFileChanged(sd, filePath)) {
        LOG_DEBUG("[FileUploader] File unchanged, skipping upload");
        return true;  // Not an error, just no need to upload
    }
    
    // Lazily create cloud import on first actual upload (avoids empty imports)
    if (!ensureCloudImport(sdManager)) {
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
    if (webdavUploader && config->hasWebdavEndpoint()) {
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
    if (sleephqUploader && config->hasCloudEndpoint() && !cloudImportFailed) {
        anyBackendConfigured = true;
        if (!sleephqUploader->isConnected()) {
            LOG_DEBUG("[FileUploader] Cloud not connected, attempting to connect...");
            if (!sleephqUploader->begin()) {
                LOG_ERROR("[FileUploader] Failed to connect to cloud service");
                return false;
            }
        }
        unsigned long cloudBytes = 0;
        if (!sleephqUploader->upload(filePath, filePath, sd, cloudBytes, sdManager)) {
            LOG_ERRORF("[FileUploader] Cloud upload failed for: %s", filePath.c_str());
            uploadSuccess = false;
        } else if (bytesTransferred == 0) {
            bytesTransferred = cloudBytes;
        }
    }
#endif
    
    // Retake SD if a backend released it during network I/O
    if (!sdManager->hasControl()) {
        if (!sdManager->takeControl()) {
            LOG_ERROR("[FileUploader] Failed to retake SD after backend network I/O");
            return false;
        }
    }
    
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