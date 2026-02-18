#include "FileUploader.h"
#include "Logger.h"
#include "WebStatus.h"
#include <SD_MMC.h>

#ifdef ENABLE_TEST_WEBSERVER
#include "TestWebServer.h"
#endif

// Constructor
FileUploader::FileUploader(Config* cfg, WiFiManager* wifi) 
    : config(cfg),
      stateManager(nullptr),
      scheduleManager(nullptr),
      wifiManager(wifi),
#ifdef ENABLE_TEST_WEBSERVER
      webServer(nullptr),
#endif
      cloudImportCreated(false),
      cloudImportFailed(false)
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
    
    // Initialize ScheduleManager with new FSM-aware overload
    scheduleManager = new ScheduleManager();
    if (!scheduleManager->begin(
            config->getUploadMode(),
            config->getUploadStartHour(),
            config->getUploadEndHour(),
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
        
        // Pre-allocate SMB buffer NOW (before Cloud TLS init fragments heap)
        // Adaptive size based on current heap state
        uint32_t maxAlloc = ESP.getMaxAllocHeap();
        size_t smbBufferSize;
        if (maxAlloc > 80000) {
            smbBufferSize = 8192;  // 8KB for pristine heap
        } else if (maxAlloc > 50000) {
            smbBufferSize = 4096;  // 4KB for moderate heap
        } else if (maxAlloc > 30000) {
            smbBufferSize = 2048;  // 2KB for fragmented heap
        } else {
            smbBufferSize = 1024;  // 1KB minimum
        }
        
        LOGF("[FileUploader] Heap state: free=%u, max_alloc=%u, allocating SMB buffer=%u",
             ESP.getFreeHeap(), maxAlloc, smbBufferSize);
        
        if (!smbUploader->allocateBuffer(smbBufferSize)) {
            LOG_ERROR("[FileUploader] Failed to allocate SMB buffer - SMB uploads may fail");
            // Don't fail init - let upload attempts handle the error
        }
        
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


// ============================================================================
// New FSM-driven exclusive access upload
// ============================================================================

UploadResult FileUploader::uploadWithExclusiveAccess(SDCardManager* sdManager, int maxMinutes, DataFilter filter) {
    fs::FS &sd = sdManager->getFS();
    unsigned long sessionStart = millis();
    unsigned long maxMs = (unsigned long)maxMinutes * 60UL * 1000UL;
    
    LOGF("[FileUploader] Exclusive access upload: maxMinutes=%d, filter=%d", maxMinutes, (int)filter);
    
    // Check WiFi connection first
    if (!wifiManager || !wifiManager->isConnected()) {
        LOG_ERROR("[FileUploader] WiFi not connected - cannot upload");
        return UploadResult::ERROR;
    }
    
    // Initialize cloud import session (lazy — created on first actual upload)
    cloudImportCreated = false;
    cloudImportFailed = false;
    
    bool anyUploaded = false;
    bool timerExpired = false;
    
    // Lambda to check if X-minute timer expired
    auto isTimerExpired = [&]() -> bool {
        return (millis() - sessionStart) >= maxMs;
    };
    
    // ── Single scan: partition into fresh and old folders ──
    std::vector<String> freshFolders;
    std::vector<String> oldFolders;
    
    bool needFresh = (filter == DataFilter::FRESH_ONLY || filter == DataFilter::ALL_DATA);
    bool needOld = (filter == DataFilter::OLD_ONLY || filter == DataFilter::ALL_DATA);
    
    if (needFresh || needOld) {
        std::vector<String> allFolders = scanDatalogFolders(sd);
        for (const String& folderName : allFolders) {
            if (isRecentFolder(folderName)) {
                freshFolders.push_back(folderName);
            } else {
                oldFolders.push_back(folderName);
            }
        }
        LOGF("[FileUploader] Scan: %d fresh, %d old folders", freshFolders.size(), oldFolders.size());
    }
    
    // ── Phase 1: Fresh DATALOG folders (newest first) ──
    int consecutiveFailures = 0;
    if (!timerExpired && needFresh) {
        LOG("[FileUploader] Phase 1: Fresh DATALOG folders");
        
        for (const String& folderName : freshFolders) {
            if (isTimerExpired()) {
                LOG("[FileUploader] X-minute timer expired during fresh DATALOG phase");
                timerExpired = true;
                // Finalize any partial import before timing out
                if (cloudImportCreated) finalizeCloudImport(sdManager, sd);
                break;
            }
            
            bool folderOk = uploadDatalogFolder(sdManager, folderName);
            
            // Finalize this folder's import (mandatory files + processImport + TLS reset)
            if (cloudImportCreated) {
                finalizeCloudImport(sdManager, sd);
                anyUploaded = true;
            }
            
            if (!folderOk) {
                if (!cloudImportCreated) {
                    consecutiveFailures++;
                } else {
                    consecutiveFailures = 0;
                }
                LOGF("[FileUploader] Folder %s had errors, trying next folder", folderName.c_str());
                cloudImportFailed = false;
#ifdef ENABLE_SLEEPHQ_UPLOAD
                if (sleephqUploader && !sleephqUploader->isTlsAlive()) {
                    sleephqUploader->resetConnection();
                }
#endif
                if (consecutiveFailures >= 2) {
                    LOG("[FileUploader] Heap fragmented beyond recovery in this session, stopping early");
                    break;
                }
            } else {
                consecutiveFailures = 0;
            }
            
#ifdef ENABLE_TEST_WEBSERVER
            if (webServer) webServer->handleClient();
#endif
        }
    }
    
    // ── Phase 2: Old DATALOG folders (newest first, only if in window) ──
    if (!timerExpired && needOld) {
        if (scheduleManager && scheduleManager->canUploadOldData()) {
            LOG("[FileUploader] Phase 2: Old DATALOG folders");
            
            for (const String& folderName : oldFolders) {
                if (isTimerExpired()) {
                    LOG("[FileUploader] X-minute timer expired during old DATALOG phase");
                    timerExpired = true;
                    // Finalize any partial import before timing out
                    if (cloudImportCreated) finalizeCloudImport(sdManager, sd);
                    break;
                }
                
                bool folderOk = uploadDatalogFolder(sdManager, folderName);
                
                // Finalize this folder's import (mandatory files + processImport + TLS reset)
                if (cloudImportCreated) {
                    finalizeCloudImport(sdManager, sd);
                    anyUploaded = true;
                }
                
                if (!folderOk) {
                    if (!cloudImportCreated) {
                        consecutiveFailures++;
                    } else {
                        consecutiveFailures = 0;
                    }
                    LOGF("[FileUploader] Folder %s had errors, trying next folder", folderName.c_str());
                    cloudImportFailed = false;
#ifdef ENABLE_SLEEPHQ_UPLOAD
                    if (sleephqUploader && !sleephqUploader->isTlsAlive()) {
                        sleephqUploader->resetConnection();
                    }
#endif
                    if (consecutiveFailures >= 2) {
                        LOG("[FileUploader] Heap fragmented beyond recovery in this session, stopping early");
                        break;
                    }
                } else {
                    consecutiveFailures = 0;
                }
                
#ifdef ENABLE_TEST_WEBSERVER
                if (webServer) webServer->handleClient();
#endif
            }
        } else {
            LOG_DEBUG("[FileUploader] Skipping old DATALOG - not in upload window");
        }
    }
    
    // Phase 3 (Standalone Settings Check) removed to enforce strict dependency on DATALOG activity.
    // Settings/Root files are now only uploaded via finalizeCloudImport() if a DATALOG folder 
    // was successfully processed and triggered an import.
    
    // Save upload state
    if (!stateManager->save(sd)) {
        LOG_ERROR("[FileUploader] Failed to save upload state");
    }

    // Clear upload session status
    g_uploadSessionStatus.uploadActive = false;
    g_uploadSessionStatus.filesUploaded = 0;
    g_uploadSessionStatus.filesTotal = 0;
    g_uploadSessionStatus.currentFolder[0] = '\0';

    // Determine result
    unsigned long elapsed = millis() - sessionStart;
    LOGF("[FileUploader] Exclusive access session ended: %lu seconds elapsed", elapsed / 1000);
    
    if (timerExpired) {
        // Check if there are still incomplete folders
        bool hasIncomplete = (stateManager->getIncompleteFoldersCount() > 0);
        if (hasIncomplete) {
            LOG("[FileUploader] Timer expired with incomplete folders remaining (TIMEOUT)");
            return UploadResult::TIMEOUT;
        }
        // Timer expired but everything was uploaded
        LOG("[FileUploader] Timer expired but all files uploaded (COMPLETE)");
    }
    
    // Mark day completed if all folders done (for scheduled mode)
    if (stateManager->getIncompleteFoldersCount() == 0) {
        time_t now;
        time(&now);
        stateManager->setLastUploadTimestamp((unsigned long)now);
        if (scheduleManager) {
            scheduleManager->markDayCompleted();
        }
        LOG("[FileUploader] All folders completed - upload session marked as done");
        return UploadResult::COMPLETE;
    }
    
    return UploadResult::TIMEOUT;
}


// Scan DATALOG folders and sort by date (newest first)
std::vector<String> FileUploader::scanDatalogFolders(fs::FS &sd, bool includeCompleted) {
    std::vector<String> folders;
    int eligibleFolderCount = 0;
    
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

            // Count all eligible DATALOG folders (completed, incomplete, and empty-pending)
            // so progress can report remaining data folders across cooldown cycles.
            eligibleFolderCount++;
            
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

    if (stateManager) {
        stateManager->setTotalFoldersCount(eligibleFolderCount);
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

// Scan SETTINGS files that need tracking (conditionally uploaded)
std::vector<String> FileUploader::scanSettingsFiles(fs::FS &sd) {
    std::vector<String> files;
    
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
    
    LOG_DEBUGF("[FileUploader] Found %d changed SETTINGS files", files.size());
    
    return files;
}

// Check if a DATALOG folder name (YYYYMMDD) is within the recent window
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
    if (cloudImportFailed) return false;  // Already failed this session, don't retry
    if (!sleephqUploader || !config->hasCloudEndpoint()) return true;  // No cloud = OK
    
    if (!sleephqUploader->isConnected()) {
        LOG("[FileUploader] Connecting cloud uploader for import session...");
        if (!sleephqUploader->begin()) {
            LOG_ERROR("[FileUploader] Failed to initialize cloud uploader");
            LOG_WARN("[FileUploader] Cloud uploads will be skipped this session");
            cloudImportFailed = true;
            return false;
        }
    }
    if (sleephqUploader->isConnected()) {
        if (!sleephqUploader->createImport()) {
            LOG_ERROR("[FileUploader] Failed to create cloud import");
            LOG_WARN("[FileUploader] Cloud uploads will be skipped this session");
            cloudImportFailed = true;
            return false;
        }
        cloudImportCreated = true;
    }
    return cloudImportCreated;
#else
    return true;
#endif
}


// Finalize current cloud import: upload mandatory files, process, reset for next folder
void FileUploader::finalizeCloudImport(SDCardManager* sdManager, fs::FS &sd) {
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (!cloudImportCreated || !sleephqUploader || !config->hasCloudEndpoint()) return;
    
    LOG("[FileUploader] Finalizing cloud import with mandatory files...");
    
    // Upload mandatory root files for ResMed/SleepHQ imports (force=true, each import needs them)
    // Upload only supported root artifacts; ignore unrelated root files.
    const char* rootPaths[] = {
        "/Identification.json", "/Identification.crc", "/Identification.tgt",
        "/STR.edf"
    };
    for (const char* path : rootPaths) {
        if (sd.exists(path)) {
            uploadSingleFile(sdManager, String(path), true);
        }
    }
    
    // Upload settings files (force=true for each import)
    std::vector<String> settingsFiles = scanSettingsFiles(sd);
    for (const String& filePath : settingsFiles) {
        uploadSingleFile(sdManager, filePath, true);
    }
    
    // Process the import (uses raw TLS if connection alive, falls back to HTTPClient)
    if (!sleephqUploader->getCurrentImportId().isEmpty()) {
        if (!sleephqUploader->processImport()) {
            LOG_WARN("[FileUploader] Failed to process cloud import for this folder");
        }
    }
    
    // Reset import flags for next folder's import cycle
    cloudImportCreated = false;
    cloudImportFailed = false;
    
    // If connection died, free TLS memory (~40KB) so next folder can establish a new one
    if (!sleephqUploader->isTlsAlive()) {
        sleephqUploader->resetConnection();
        LOG("[FileUploader] Connection lost, TLS memory freed for next folder");
    } else {
        LOG("[FileUploader] Import cycle complete, connection kept alive for next folder");
    }
#endif
}


// Upload all files in a DATALOG folder
bool FileUploader::uploadDatalogFolder(SDCardManager* sdManager, const String& folderName) {
    fs::FS &sd = sdManager->getFS();
    
    LOGF("[FileUploader] Uploading DATALOG folder: %s", folderName.c_str());
    
    // Build folder path
    String folderPath = "/DATALOG/" + folderName;
    
    // Verify folder exists before scanning
    File folderCheck = sd.open(folderPath);
    if (!folderCheck) {
        LOG_ERRORF("[FileUploader] Cannot access folder: %s", folderPath.c_str());
        LOG_ERROR("[FileUploader] SD card may be in use by CPAP machine");
        return false;
    }
    
    if (!folderCheck.isDirectory()) {
        LOG_ERRORF("[FileUploader] Path is not a directory: %s", folderPath.c_str());
        folderCheck.close();
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
            return false;  // Treat as error
        }
        verifyFolder.close();
        
        // Folder is accessible but truly empty - handle with pending state
        LOG_WARN("[FileUploader] No .edf files found in folder (folder is empty)");
        
        // Check if NTP time is valid before tracking pending folders
        unsigned long currentTime = time(NULL);
        if (currentTime < 1000000000) {  // Invalid NTP time (before year 2001)
            LOG_WARN("[FileUploader] NTP time not available - cannot track empty folder timing");
            return false;  // Will retry when NTP is available
        }
        
        // Check if folder is already in pending state
        if (stateManager->isPendingFolder(folderName)) {
            // Check if 7-day timeout has elapsed
            if (stateManager->shouldPromotePendingToCompleted(folderName, currentTime)) {
                // Promote to completed after 7 days of being empty
                stateManager->promotePendingToCompleted(folderName);
                stateManager->save(sd);
                return true;
            } else {
                // Still within 7-day window, skip for now
                LOG_DEBUGF("[FileUploader] Pending folder still within 7-day window: %s", folderName.c_str());
                return true;
            }
        } else {
            // First time seeing this empty folder - mark as pending
            stateManager->markFolderPending(folderName, currentTime);
            LOG_DEBUGF("[FileUploader] Marked empty folder as pending: %s", folderName.c_str());
            stateManager->save(sd);
            return true;
        }
    }
    
    // Check if this is a recently completed folder being re-scanned
    bool isRecent = isRecentFolder(folderName);
    bool isRecentRescan = stateManager->isFolderCompleted(folderName) && isRecent;
    
    // Update web status: session started for this folder
    g_uploadSessionStatus.uploadActive = true;
    strncpy((char*)g_uploadSessionStatus.currentFolder, folderName.c_str(), sizeof(g_uploadSessionStatus.currentFolder) - 1);
    ((char*)g_uploadSessionStatus.currentFolder)[sizeof(g_uploadSessionStatus.currentFolder) - 1] = '\0';
    g_uploadSessionStatus.filesTotal    = (int)files.size();
    g_uploadSessionStatus.filesUploaded = 0;

    // Upload each file
    int uploadedCount = 0;
    int skippedUnchanged = 0;
    // Determine which backend groups are active. SMB/WebDAV run in Phase 1
    // (heap fresh) and SleepHQ in Phase 2 (after SMB disconnect). This ensures
    // ensureCloudImport()'s TLS/OAuth handshake — which drops maxAlloc from
    // ~90 KB to ~39 KB — never runs before libsmb2, preventing signing buffer
    // allocation failures and subsequent heap corruption.
    const bool hasSmbLike = false
#ifdef ENABLE_SMB_UPLOAD
        || (smbUploader && config->hasSmbEndpoint())
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
        || (webdavUploader && config->hasWebdavEndpoint())
#endif
        ;
    const bool hasCloud = false
#ifdef ENABLE_SLEEPHQ_UPLOAD
        || (sleephqUploader && config->hasCloudEndpoint() && !cloudImportFailed)
#endif
        ;
    if (!hasSmbLike && !hasCloud) {
        LOG_ERROR("[FileUploader] No uploader available for configured endpoint type");
        return false;
    }

    // -------------------------------------------------------
    // Phase 1: SMB / WebDAV  (must run while heap is fresh)
    // -------------------------------------------------------
    if (hasSmbLike) {
        for (const String& fileName : files) {
            String localPath = folderPath + "/" + fileName;
            if (isRecentRescan) {
                if (!stateManager->hasFileChanged(sd, localPath)) { skippedUnchanged++; continue; }
                LOG_DEBUGF("[FileUploader] File changed in recent folder: %s", fileName.c_str());
            }
            File file = sd.open(localPath);
            if (!file) {
                LOG_ERRORF("[FileUploader] Cannot open file for reading: %s", localPath.c_str());
                LOG_ERROR("[FileUploader] File may be corrupted or SD card has read errors");
                LOG_WARN("[FileUploader] Skipping this file and continuing with next file");
                continue;
            }
            unsigned long fileSize = file.size();
            if (fileSize == 0) {
                LOG_WARNF("[FileUploader] File is empty: %s", localPath.c_str());
                file.close();
                stateManager->markFileUploaded(localPath, "empty_file", 0);
                continue;
            }
            file.close();
            String remotePath = folderPath + "/" + fileName;
            unsigned long bytesTransferred = 0;
            LOGF("[FileUploader] Uploading file: %s (%lu bytes)", fileName.c_str(), fileSize);
            bool fileSuccess = true;
#ifdef ENABLE_SMB_UPLOAD
            if (smbUploader && config->hasSmbEndpoint()) {
                if (!smbUploader->isConnected()) {
                    LOG_DEBUG("[FileUploader] SMB not connected, attempting to connect...");
                    if (!smbUploader->begin()) {
                        LOG_ERROR("[FileUploader] Failed to connect to SMB share");
                        return false;
                    }
                }
                unsigned long smbBytes = 0;
                if (!smbUploader->upload(localPath, remotePath, sd, smbBytes)) {
                    LOG_ERRORF("[FileUploader] SMB upload failed for: %s", localPath.c_str());
                    fileSuccess = false;
                } else { bytesTransferred = smbBytes; }
            }
#endif
#ifdef ENABLE_WEBDAV_UPLOAD
            if (webdavUploader && config->hasWebdavEndpoint()) {
                if (!webdavUploader->isConnected()) {
                    LOG_DEBUG("[FileUploader] WebDAV not connected, attempting to connect...");
                    if (!webdavUploader->begin()) {
                        LOG_ERROR("[FileUploader] Failed to connect to WebDAV server");
                        return false;
                    }
                }
                unsigned long davBytes = 0;
                if (!webdavUploader->upload(localPath, remotePath, sd, davBytes)) {
                    LOG_ERRORF("[FileUploader] WebDAV upload failed for: %s", localPath.c_str());
                    fileSuccess = false;
                } else if (bytesTransferred == 0) { bytesTransferred = davBytes; }
            }
#endif
            if (!fileSuccess) {
                LOG_ERRORF("[FileUploader] Failed to upload file: %s", localPath.c_str());
                LOGF("[FileUploader] Successfully uploaded %d files before failure", uploadedCount);
                stateManager->save(sd);
                return false;
            }
            if (!hasCloud && isRecent) {
                stateManager->markFileUploaded(localPath, "", fileSize);
            }
            uploadedCount++;
            g_uploadSessionStatus.filesUploaded = uploadedCount;
            LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), bytesTransferred);
#ifdef ENABLE_TEST_WEBSERVER
            if (webServer) webServer->handleClient();
#endif
        }
        // Disconnect SMB before cloud phase — frees libsmb2 context and TCP socket
        // so ensureCloudImport()'s TLS/OAuth fragmentation is contained to Phase 2.
#ifdef ENABLE_SMB_UPLOAD
        if (smbUploader && smbUploader->isConnected()) {
            LOG_DEBUG("[FileUploader] SMB phase complete — disconnecting before cloud phase");
            smbUploader->end();
        }
#endif
    }
    // -------------------------------------------------------
    // Phase 2: SleepHQ cloud uploads
    // ensureCloudImport() (TLS/OAuth) is intentionally deferred
    // to here so heap fragmentation only affects this phase.
    // ensureCloudImport() is also lazy: called only when there is
    // actually a file to upload, avoiding TLS init for all-unchanged folders.
    // -------------------------------------------------------
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (hasCloud) {
        bool cloudImportAttempted = false;
        for (const String& fileName : files) {
            String localPath = folderPath + "/" + fileName;
            if (isRecentRescan) {
                if (!stateManager->hasFileChanged(sd, localPath)) {
                    if (!hasSmbLike) skippedUnchanged++;
                    continue;
                }
                if (!hasSmbLike) LOG_DEBUGF("[FileUploader] File changed in recent folder: %s", fileName.c_str());
            }
            File file = sd.open(localPath);
            if (!file) {
                if (!hasSmbLike) {
                    LOG_ERRORF("[FileUploader] Cannot open file for reading: %s", localPath.c_str());
                    LOG_WARN("[FileUploader] Skipping this file and continuing with next file");
                }
                continue;
            }
            unsigned long fileSize = file.size();
            file.close();
            if (fileSize == 0) {
                if (!hasSmbLike) stateManager->markFileUploaded(localPath, "empty_file", 0);
                continue;
            }
            // Lazily ensure cloud import — only on first file that actually needs uploading
            if (!cloudImportAttempted) {
                if (!ensureCloudImport()) {
                    LOG_WARN("[FileUploader] Cloud import not available, skipping cloud uploads this session");
                    break;
                }
                cloudImportAttempted = true;
            }
            String remotePath = folderPath + "/" + fileName;
            if (!hasSmbLike) LOGF("[FileUploader] Uploading file: %s (%lu bytes)", fileName.c_str(), fileSize);
            if (!sleephqUploader->isConnected()) {
                LOG_DEBUG("[FileUploader] Cloud not connected, attempting to connect...");
                if (!sleephqUploader->begin()) {
                    LOG_ERROR("[FileUploader] Failed to connect to cloud service");
                    return false;
                }
            }
            unsigned long cloudBytes = 0;
            String cloudChecksum = "";
            if (!sleephqUploader->upload(localPath, remotePath, sd, cloudBytes, cloudChecksum)) {
                LOG_ERRORF("[FileUploader] Cloud upload failed for: %s", localPath.c_str());
                if (!hasSmbLike) LOGF("[FileUploader] Successfully uploaded %d files before failure", uploadedCount);
                stateManager->save(sd);
                return false;
            }
            if (isRecent) stateManager->markFileUploaded(localPath, "", fileSize);
            if (!hasSmbLike) {
                uploadedCount++;
                g_uploadSessionStatus.filesUploaded = uploadedCount;
                LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), cloudBytes);
            }
#ifdef ENABLE_TEST_WEBSERVER
            if (webServer) webServer->handleClient();
#endif
        }
    }
#endif

    // All files processed successfully
    if (isRecentRescan) {
        LOGF("[FileUploader] Re-scan complete: %d uploaded, %d unchanged in folder", uploadedCount, skippedUnchanged);
    } else {
        LOGF("[FileUploader] Successfully uploaded all %d files in folder", uploadedCount);
    }
    
    // Disconnect SMB after folder completes to free libsmb2 internal buffers
    // (per-folder, not per-file, to avoid socket exhaustion)
#ifdef ENABLE_SMB_UPLOAD
    if (smbUploader && smbUploader->isConnected()) {
        LOG_DEBUG("[FileUploader] Disconnecting SMB after folder to free internal buffers");
        smbUploader->end();
    }
#endif
    
    // Mark folder as completed
    stateManager->markFolderCompleted(folderName);
    
    // Save state
    stateManager->save(sd);
    
    return true;
}

// Upload a single file (for root and SETTINGS files)
bool FileUploader::uploadSingleFile(SDCardManager* sdManager, const String& filePath, bool force) {
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
    
    // Check if file has changed (checksum comparison)
    // If 'force' is true, skip this check (used for mandatory root files)
    if (!force) {
        if (!stateManager->hasFileChanged(sd, filePath)) {
            LOG_DEBUG("[FileUploader] File unchanged, skipping upload");
            return true;  // Not an error, just no need to upload
        }
    } else {
        LOG_DEBUG("[FileUploader] Forcing upload (mandatory file)");
    }
    
    // Upload the file — SMB/WebDAV first (heap fresh), SleepHQ second (TLS alloc after)
    unsigned long bytesTransferred = 0;
    unsigned long uploadStartTime = millis();
    bool uploadSuccess = true;
    bool anyBackendConfigured = false;

    // Phase 1: SMB / WebDAV
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

    // Phase 2: SleepHQ (TLS/OAuth after SMB is done)
    // ensureCloudImport() is called lazily here — only if this file needs uploading
#ifdef ENABLE_SLEEPHQ_UPLOAD
    String fileChecksum = "";
    if (sleephqUploader && config->hasCloudEndpoint() && !cloudImportFailed) {
        anyBackendConfigured = true;
        if (!ensureCloudImport()) {
            LOG_WARN("[FileUploader] Cloud import not available, skipping cloud upload for this file");
        } else {
            if (!sleephqUploader->isConnected()) {
                LOG_DEBUG("[FileUploader] Cloud not connected, attempting to connect...");
                if (!sleephqUploader->begin()) {
                    LOG_ERROR("[FileUploader] Failed to connect to cloud service");
                    return false;
                }
            }
            unsigned long cloudBytes = 0;
            String cloudChecksum = "";
            if (!sleephqUploader->upload(filePath, filePath, sd, cloudBytes, cloudChecksum)) {
                LOG_ERRORF("[FileUploader] Cloud upload failed for: %s", filePath.c_str());
                uploadSuccess = false;
            } else if (bytesTransferred == 0) {
                bytesTransferred = cloudBytes;
            }
            if (!cloudChecksum.isEmpty()) {
                fileChecksum = cloudChecksum;
            }
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
    
    // Calculate and store checksum + size so hasFileChanged() won't flag this file next session
    String checksum = fileChecksum;
    if (checksum.isEmpty()) {
        checksum = stateManager->calculateChecksum(sd, filePath);
    }
    
    if (!checksum.isEmpty()) {
        stateManager->markFileUploaded(filePath, checksum, fileSize);
    }
    
    LOGF("[FileUploader] Successfully uploaded: %s (%lu bytes)", filePath.c_str(), bytesTransferred);
    
#ifdef ENABLE_TEST_WEBSERVER
    if (webServer) webServer->handleClient();
#endif
    
    return true;
}