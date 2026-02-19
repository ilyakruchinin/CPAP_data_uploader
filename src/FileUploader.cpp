#include "FileUploader.h"
#include "Logger.h"
#include "WebStatus.h"
#include <SD_MMC.h>
#include <functional>

#ifdef ENABLE_TEST_WEBSERVER
#include "TestWebServer.h"
#endif

// Constructor
FileUploader::FileUploader(Config* cfg, WiFiManager* wifi) 
    : config(cfg),
      smbStateManager(nullptr),
      cloudStateManager(nullptr),
      scheduleManager(nullptr),
      wifiManager(wifi),
#ifdef ENABLE_TEST_WEBSERVER
      webServer(nullptr),
#endif
      cloudImportCreated(false),
      cloudImportFailed(false),
      cloudDatalogFilesUploaded(0)
#ifdef ENABLE_SMB_UPLOAD
      , smbUploader(nullptr)
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
      , sleephqUploader(nullptr)
#endif
{
}

// Destructor
FileUploader::~FileUploader() {
    if (smbStateManager)   delete smbStateManager;
    if (cloudStateManager) delete cloudStateManager;
    if (scheduleManager)   delete scheduleManager;
#ifdef ENABLE_SMB_UPLOAD
    if (smbUploader) delete smbUploader;
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (sleephqUploader) delete sleephqUploader;
#endif
}

// Initialize all components and load upload state
bool FileUploader::begin(fs::FS &sd) {
    LOG("[FileUploader] Initializing components...");

    String endpointType = config->getEndpointType();
    LOGF("[FileUploader] Endpoint type: %s", endpointType.c_str());

    bool anyBackendCreated = false;

    // ── SMB uploader + state ─────────────────────────────────────────────────
#ifdef ENABLE_SMB_UPLOAD
    if (config->hasSmbEndpoint()) {
        smbUploader = new SMBUploader(
            config->getEndpoint(),
            config->getEndpointUser(),
            config->getEndpointPassword()
        );
        LOG("[FileUploader] SMBUploader created (will connect during upload)");

        uint32_t maxAlloc = ESP.getMaxAllocHeap();
        size_t smbBufferSize;
        if      (maxAlloc > 80000) smbBufferSize = 8192;
        else if (maxAlloc > 50000) smbBufferSize = 4096;
        else if (maxAlloc > 30000) smbBufferSize = 2048;
        else                       smbBufferSize = 1024;

        LOGF("[FileUploader] Heap state: free=%u, max_alloc=%u, allocating SMB buffer=%u",
             ESP.getFreeHeap(), maxAlloc, smbBufferSize);
        if (!smbUploader->allocateBuffer(smbBufferSize)) {
            LOG_ERROR("[FileUploader] Failed to allocate SMB buffer - SMB uploads may fail");
        }

        smbStateManager = new UploadStateManager();
        smbStateManager->setPaths("/.upload_state.v2.smb", "/.upload_state.v2.smb.log");
        if (!smbStateManager->begin(sd)) {
            LOG("[FileUploader] WARNING: SMB state load failed, starting fresh");
        }
        anyBackendCreated = true;
    }
#endif

    // ── Cloud uploader + state ───────────────────────────────────────────────
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (config->hasCloudEndpoint()) {
        sleephqUploader = new SleepHQUploader(config);
        LOG("[FileUploader] SleepHQUploader created (will connect during upload)");

        cloudStateManager = new UploadStateManager();
        cloudStateManager->setPaths("/.upload_state.v2.cloud", "/.upload_state.v2.cloud.log");
        if (!cloudStateManager->begin(sd)) {
            LOG("[FileUploader] WARNING: Cloud state load failed, starting fresh");
        }
        anyBackendCreated = true;
    }
#endif

    if (!anyBackendCreated) {
        LOGF("[FileUploader] ERROR: No uploader created for endpoint type: %s", endpointType.c_str());
        return false;
    }

    LOGF("[FileUploader] Backends active this run: SMB=%s CLOUD=%s",
         smbStateManager   ? "YES" : "NO",
         cloudStateManager ? "YES" : "NO");

    // ── Schedule manager ─────────────────────────────────────────────────────
    scheduleManager = new ScheduleManager();
    if (!scheduleManager->begin(
            config->getUploadMode(),
            config->getUploadStartHour(),
            config->getUploadEndHour(),
            config->getGmtOffsetHours())) {
        LOG("[FileUploader] ERROR: Failed to initialize ScheduleManager");
        return false;
    }
    scheduleManager->setLastUploadTimestamp(primaryStateManager()->getLastUploadTimestamp());

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

    if (!wifiManager || !wifiManager->isConnected()) {
        LOG_ERROR("[FileUploader] WiFi not connected - cannot upload");
        return UploadResult::ERROR;
    }

    cloudImportCreated = false;
    cloudImportFailed  = false;

    bool timerExpired = false;
    auto isTimerExpired = [&]() -> bool {
        return (millis() - sessionStart) >= maxMs;
    };

    bool needFresh = (filter == DataFilter::FRESH_ONLY || filter == DataFilter::ALL_DATA);
    bool needOld   = (filter == DataFilter::OLD_ONLY   || filter == DataFilter::ALL_DATA);

    // ═══════════════════════════════════════════════════════════════════════
    // SMB PASS — heap is fresh (max_alloc ~73 KB), no TLS in scope yet.
    // All DATALOG folders + mandatory root files go here before cloud starts.
    // ═══════════════════════════════════════════════════════════════════════
#ifdef ENABLE_SMB_UPLOAD
    if (smbUploader && smbStateManager) {
        LOG("[FileUploader] === SMB Pass ===");

        // Pre-flight scan — SD-only, no network.
        // 1. DATALOG folders with changed files (size check per file).
        std::vector<String> smbFreshFolders, smbOldFolders;
        if (needFresh || needOld) {
            std::vector<String> all = scanDatalogFolders(sd, smbStateManager);
            for (const String& f : all) {
                if (isRecentFolder(f)) smbFreshFolders.push_back(f);
                else                   smbOldFolders.push_back(f);
            }
            LOGF("[FileUploader] SMB scan: %d fresh, %d old folders",
                 (int)smbFreshFolders.size(), (int)smbOldFolders.size());
        }

        // 2. Mandatory root files + SETTINGS: any size or checksum change?
        bool smbMandatoryChanged = false;
        {
            static const char* rootPaths[] = {
                "/Identification.json", "/Identification.crc",
                "/Identification.tgt",  "/STR.edf"
            };
            for (const char* p : rootPaths) {
                if (sd.exists(p) && smbStateManager->hasFileChanged(sd, String(p))) {
                    smbMandatoryChanged = true;
                    break;
                }
            }
            if (!smbMandatoryChanged) {
                for (const String& fp : scanSettingsFiles(sd)) {
                    if (smbStateManager->hasFileChanged(sd, fp)) {
                        smbMandatoryChanged = true;
                        break;
                    }
                }
            }
        }

        bool smbHasWork = !smbFreshFolders.empty() ||
                          (!smbOldFolders.empty() &&
                           scheduleManager && scheduleManager->canUploadOldData()) ||
                          smbMandatoryChanged;

        if (!smbHasWork) {
            LOG("[FileUploader] SMB: nothing to upload — skipping");
        } else {
        // Mandatory root files first (Identification.*, STR.edf, SETTINGS/)
        if (!isTimerExpired()) {
            uploadMandatoryFilesSmb(sdManager, sd);
        }

        if (!timerExpired && needFresh) {
            LOG("[FileUploader] Phase 1: Fresh DATALOG folders");
            for (const String& folder : smbFreshFolders) {
                if (isTimerExpired()) { timerExpired = true; break; }
                bool uploadSuccess = uploadDatalogFolderSmb(sdManager, folder);
                if (!uploadSuccess) {
                    LOG_WARNF("[FileUploader] SMB upload failed for folder: %s - marking recent scan failed", folder.c_str());
                    if (isRecentFolder(folder)) {
                        smbStateManager->markFolderRecentScanFailed(folder);
                    }
                }
#ifdef ENABLE_TEST_WEBSERVER
                if (webServer) webServer->handleClient();
#endif
            }
        }

        if (!timerExpired && needOld && scheduleManager && scheduleManager->canUploadOldData()) {
            LOG("[FileUploader] Phase 2: Old DATALOG folders");
            for (const String& folder : smbOldFolders) {
                if (isTimerExpired()) { timerExpired = true; break; }
                bool uploadSuccess = uploadDatalogFolderSmb(sdManager, folder);
                if (!uploadSuccess) {
                    LOG_WARNF("[FileUploader] SMB upload failed for folder: %s - marking recent scan failed", folder.c_str());
                    if (isRecentFolder(folder)) {
                        smbStateManager->markFolderRecentScanFailed(folder);
                    }
                }
#ifdef ENABLE_TEST_WEBSERVER
                if (webServer) webServer->handleClient();
#endif
            }
        }

        if (smbUploader->isConnected()) smbUploader->end();
        } // smbHasWork
        smbStateManager->save(sd);

        // Destroy the SMB uploader entirely to reclaim its 8 KB upload buffer
        // before the cloud TLS pass.  After the SMB pass the uploader is no
        // longer needed; freeing the buffer here can raise max_alloc by ~8 KB
        // (from ~47 KB → ~55 KB), which is often the margin that lets both
        // the OAuth and the import-creation TLS sessions succeed.
        LOGF("[FileUploader] SMB pass done — heap before SMB teardown: fh=%u ma=%u",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        delete smbUploader;
        smbUploader = nullptr;
        LOGF("[FileUploader] SMB uploader freed — heap after teardown:  fh=%u ma=%u",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

        // Clear SMB status bar
        g_smbSessionStatus.uploadActive  = false;
        g_smbSessionStatus.filesUploaded  = 0;
        g_smbSessionStatus.filesTotal     = 0;
        g_smbSessionStatus.currentFolder[0] = '\0';
    }
#endif

    // ═══════════════════════════════════════════════════════════════════════
    // Cloud PASS — single TLS session: begin() does OAuth + team-discovery +
    // createImport() while heap is fresh (post-SMB-teardown). All DATALOG
    // folders are uploaded into that one import; mandatory root/SETTINGS
    // files and processImport() are called once at the very end.
    // ═══════════════════════════════════════════════════════════════════════
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (!timerExpired && sleephqUploader && cloudStateManager) {
        LOG("[FileUploader] === Cloud Pass ===");
        cloudDatalogFilesUploaded = 0;

        // Pre-flight scan — SD-only, no network. Determines whether any cloud
        // work exists before spending heap on OAuth + team-discovery + createImport.
        std::vector<String> cloudFreshFolders, cloudOldFolders;
        if (needFresh || needOld) {
            std::vector<String> all = scanDatalogFolders(sd, cloudStateManager);
            for (const String& f : all) {
                if (isRecentFolder(f)) cloudFreshFolders.push_back(f);
                else                   cloudOldFolders.push_back(f);
            }
            LOGF("[FileUploader] Cloud scan: %d fresh, %d old folders",
                 (int)cloudFreshFolders.size(), (int)cloudOldFolders.size());
        }

        bool cloudHasWork = !cloudFreshFolders.empty() ||
                            (!cloudOldFolders.empty() &&
                             scheduleManager && scheduleManager->canUploadOldData());

        if (!cloudHasWork) {
            LOG("[FileUploader] Cloud: nothing to upload — skipping auth + import");
        } else {
        // Connect + create import NOW, while max_alloc is at its peak after
        // SMB teardown. begin() reuses the same TLS session for all three steps
        // (OAuth, team-discovery, createImport) — no second handshake required.
        if (!sleephqUploader->isConnected()) {
            LOG("[FileUploader] Initializing cloud session (OAuth + import)...");
            LOGF("[FileUploader] Heap before cloud begin: fh=%u ma=%u",
                 (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            if (!sleephqUploader->begin()) {
                LOG_ERROR("[FileUploader] Cloud init failed — skipping cloud pass");
                cloudImportFailed = true;
            } else {
                cloudImportCreated = true;
                LOGF("[FileUploader] Cloud session ready — heap: fh=%u ma=%u",
                     (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            }
        } else {
            // Already connected from a previous attempt; ensure import exists
            if (!cloudImportCreated && !cloudImportFailed) {
                if (!sleephqUploader->createImport()) {
                    cloudImportFailed = true;
                } else {
                    cloudImportCreated = true;
                }
            }
        }

        if (!cloudImportFailed) {

            auto runCloudFolder = [&](const String& folder) -> bool {
                if (isTimerExpired()) { timerExpired = true; return false; }
                bool uploadSuccess = uploadDatalogFolderCloud(sdManager, folder);
                if (!uploadSuccess) {
                    LOG_WARNF("[FileUploader] Cloud upload failed for folder: %s - marking recent scan failed", folder.c_str());
                    if (isRecentFolder(folder)) {
                        cloudStateManager->markFolderRecentScanFailed(folder);
                    }
                }
#ifdef ENABLE_TEST_WEBSERVER
                if (webServer) webServer->handleClient();
#endif
                return true;
            };

            if (!timerExpired && needFresh) {
                LOG("[FileUploader] Phase 1: Fresh DATALOG folders (cloud)");
                for (const String& folder : cloudFreshFolders) {
                    if (!runCloudFolder(folder)) break;
                }
            }
            if (!timerExpired && needOld && scheduleManager && scheduleManager->canUploadOldData()) {
                LOG("[FileUploader] Phase 2: Old DATALOG folders (cloud)");
                for (const String& folder : cloudOldFolders) {
                    if (!runCloudFolder(folder)) break;
                }
            }

            // Finalize once: upload mandatory root/SETTINGS files then processImport.
            // Skip if no DATALOG files were actually uploaded — an import with only
            // mandatory/SETTINGS files violates requirements and wastes an import slot.
            if (cloudImportCreated && cloudDatalogFilesUploaded > 0) {
                LOGF("[FileUploader] Finalizing import: %d DATALOG files uploaded", cloudDatalogFilesUploaded);
                finalizeCloudImport(sdManager, sd);
            } else if (cloudImportCreated && cloudDatalogFilesUploaded == 0) {
                LOG("[FileUploader] No new DATALOG files — skipping import finalize");
            }
        }
        } // cloudHasWork

        cloudStateManager->save(sd);

        // Clear cloud status bar
        g_cloudSessionStatus.uploadActive   = false;
        g_cloudSessionStatus.filesUploaded  = 0;
        g_cloudSessionStatus.filesTotal     = 0;
        g_cloudSessionStatus.currentFolder[0] = '\0';
    }
#endif

    // ── Determine result ─────────────────────────────────────────────────────
    unsigned long elapsed = millis() - sessionStart;
    LOGF("[FileUploader] Exclusive access session ended: %lu seconds elapsed", elapsed / 1000);

    if (timerExpired) {
        bool hasIncomplete = hasIncompleteFolders();
        if (hasIncomplete) {
            LOG("[FileUploader] Timer expired with incomplete folders remaining (TIMEOUT)");
            return UploadResult::TIMEOUT;
        }
        LOG("[FileUploader] Timer expired but all files uploaded (COMPLETE)");
    }

    // Mark day completed if all folders done (for scheduled mode)
    if (!hasIncompleteFolders()) {
        time_t now;
        time(&now);
        primaryStateManager()->setLastUploadTimestamp((unsigned long)now);
        if (scheduleManager) {
            scheduleManager->markDayCompleted();
        }
        LOG("[FileUploader] All folders completed - upload session marked as done");
        return UploadResult::COMPLETE;
    }
    
    return UploadResult::TIMEOUT;
}


// Scan DATALOG folders and sort by date (newest first)
std::vector<String> FileUploader::scanDatalogFolders(fs::FS &sd, UploadStateManager* sm,
                                                      bool includeCompleted) {
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
            if (sm->isFolderCompleted(folderName)) {
                if (includeCompleted) {
                    // For delta/deep scans, include completed folders
                    folders.push_back(folderName);
                    LOG_INFOF("[FileUploader] Found completed DATALOG folder: %s", folderName.c_str());
                } else if (isRecentFolder(folderName) && sm->shouldRescanRecentFolder(folderName)) {
                    // Recent completed folders: re-scan if recent scan failed
                    // This ensures failed uploads get retried while avoiding unnecessary scans
                    folders.push_back(folderName);
                    LOG_DEBUGF("[FileUploader] Recent folder needs rescan (failed recent scan): %s", folderName.c_str());
                } else {
                    LOG_DEBUGF("[FileUploader] Skipping completed folder: %s", folderName.c_str());
                }
            } else if (sm->isPendingFolder(folderName)) {
                // Check if pending folder now has files (was empty but now has content)
                String folderPath = "/DATALOG/" + folderName;
                std::vector<String> folderFiles = scanFolderFiles(sd, folderPath);
                
                if (!folderFiles.empty()) {
                    // Folder now has files - remove from pending state immediately and process normally
                    LOG_DEBUGF("[FileUploader] Pending folder now has files, removing from pending: %s", folderName.c_str());
                    sm->removeFolderFromPending(folderName);
                    folders.push_back(folderName);
                } else {
                    // Still empty - check if pending folder has timed out
                    unsigned long currentTime = time(NULL);
                    if (currentTime >= 1000000000 && sm->shouldPromotePendingToCompleted(folderName, currentTime)) {
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

    if (sm) sm->setTotalFoldersCount(eligibleFolderCount);
    
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

// Scan all SETTINGS files (change-checking is left to the upload method)
std::vector<String> FileUploader::scanSettingsFiles(fs::FS &sd) {
    std::vector<String> files;
    File settingsDir = sd.open("/SETTINGS");
    if (settingsDir && settingsDir.isDirectory()) {
        File settingsFile = settingsDir.openNextFile();
        while (settingsFile) {
            if (!settingsFile.isDirectory()) {
                String name = String(settingsFile.name());
                int lastSlash = name.lastIndexOf('/');
                if (lastSlash >= 0) name = name.substring(lastSlash + 1);
                files.push_back("/SETTINGS/" + name);
            }
            settingsFile.close();
            settingsFile = settingsDir.openNextFile();
        }
        settingsDir.close();
    }
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

    const char* rootPaths[] = {
        "/Identification.json", "/Identification.crc", "/Identification.tgt", "/STR.edf"
    };
    for (const char* path : rootPaths) {
        if (sd.exists(path)) uploadSingleFileCloud(sdManager, String(path), true);
    }
    std::vector<String> settingsFiles = scanSettingsFiles(sd);
    for (const String& filePath : settingsFiles) {
        uploadSingleFileCloud(sdManager, filePath, true);
    }

    if (!sleephqUploader->getCurrentImportId().isEmpty()) {
        if (!sleephqUploader->processImport()) {
            LOG_WARN("[FileUploader] Failed to process cloud import for this folder");
        }
    }

    cloudImportCreated = false;
    cloudImportFailed  = false;

    if (!sleephqUploader->isTlsAlive()) {
        sleephqUploader->resetConnection();
        LOG("[FileUploader] Connection lost, TLS memory freed for next folder");
    } else {
        LOG("[FileUploader] Import cycle complete, connection kept alive for next folder");
    }
#endif
}


// ============================================================================
// Shared helper: handle empty folder state (pending/promote). Uses provided sm.
// Returns true if caller should return true (no files but handled).
// Returns false if caller should return false (error).
// Sets filesOut to the file list on success.
// ============================================================================
static bool handleFolderScan(fs::FS &sd, const String& folderName, const String& folderPath,
                              UploadStateManager* sm,
                              std::vector<String>& filesOut,
                              std::function<std::vector<String>(fs::FS&, const String&)> scanFn) {
    File folderCheck = sd.open(folderPath);
    if (!folderCheck) {
        LOG_ERRORF("[FileUploader] Cannot access folder: %s", folderPath.c_str());
        return false;
    }
    if (!folderCheck.isDirectory()) {
        LOG_ERRORF("[FileUploader] Path is not a directory: %s", folderPath.c_str());
        folderCheck.close();
        return false;
    }
    folderCheck.close();

    filesOut = scanFn(sd, folderPath);

    if (sm->isPendingFolder(folderName) && !filesOut.empty()) {
        sm->removeFolderFromPending(folderName);
    }

    if (filesOut.empty()) {
        File vf = sd.open(folderPath);
        if (!vf) return false;
        vf.close();
        LOG_WARN("[FileUploader] No .edf files found in folder (folder is empty)");
        unsigned long currentTime = time(NULL);
        if (currentTime < 1000000000) { return false; }
        if (sm->isPendingFolder(folderName)) {
            if (sm->shouldPromotePendingToCompleted(folderName, currentTime)) {
                sm->promotePendingToCompleted(folderName);
                sm->save(sd);
            }
        } else {
            sm->markFolderPending(folderName, currentTime);
            sm->save(sd);
        }
        return true;  // "done" for this folder (empty)
    }
    return true;  // filesOut populated
}

// ============================================================================
// SMB PASS: upload all DATALOG files for one folder
// ============================================================================
bool FileUploader::uploadDatalogFolderSmb(SDCardManager* sdManager, const String& folderName) {
#ifndef ENABLE_SMB_UPLOAD
    return true;
#else
    if (!smbUploader || !smbStateManager) return false;
    fs::FS &sd = sdManager->getFS();

    LOGF("[FileUploader] [SMB] Uploading DATALOG folder: %s", folderName.c_str());
    String folderPath = "/DATALOG/" + folderName;

    std::vector<String> files;
    if (!handleFolderScan(sd, folderName, folderPath, smbStateManager, files,
            [this](fs::FS& sd2, const String& fp) { return scanFolderFiles(sd2, fp); })) {
        return false;
    }
    if (files.empty()) return true;  // empty folder handled

    bool isRecent     = isRecentFolder(folderName);
    bool isRescan     = smbStateManager->isFolderCompleted(folderName) && isRecent;

    g_smbSessionStatus.uploadActive = true;
    strncpy((char*)g_smbSessionStatus.currentFolder, folderName.c_str(),
            sizeof(g_smbSessionStatus.currentFolder) - 1);
    ((char*)g_smbSessionStatus.currentFolder)[sizeof(g_smbSessionStatus.currentFolder) - 1] = '\0';
    g_smbSessionStatus.filesTotal    = (int)files.size();
    g_smbSessionStatus.filesUploaded = 0;

    int uploadedCount   = 0;
    int skippedUnchanged = 0;

    for (const String& fileName : files) {
        String localPath  = folderPath + "/" + fileName;
        if (isRescan) {
            if (!smbStateManager->hasFileChanged(sd, localPath)) { skippedUnchanged++; continue; }
            LOG_DEBUGF("[FileUploader] [SMB] File changed: %s", fileName.c_str());
        }
        File f = sd.open(localPath);
        if (!f) { LOG_ERRORF("[FileUploader] [SMB] Cannot open: %s", localPath.c_str()); continue; }
        unsigned long fileSize = f.size();
        if (fileSize == 0) {
            f.close();
            smbStateManager->markFileUploaded(localPath, "empty_file", 0);
            continue;
        }
        f.close();

        LOGF("[FileUploader] Uploading file: %s (%lu bytes)", fileName.c_str(), fileSize);

        if (!smbUploader->isConnected()) {
            if (!smbUploader->begin()) {
                LOG_ERROR("[FileUploader] [SMB] Failed to connect");
                smbStateManager->save(sd);
                return false;
            }
        }
        unsigned long smbBytes = 0;
        if (!smbUploader->upload(localPath, localPath, sd, smbBytes)) {
            LOG_ERRORF("[FileUploader] [SMB] Upload failed: %s", localPath.c_str());
            LOGF("[FileUploader] Successfully uploaded %d files before failure", uploadedCount);
            smbStateManager->save(sd);
            return false;
        }
        if (isRecent) smbStateManager->markFileUploaded(localPath, "", fileSize);
        uploadedCount++;
        g_smbSessionStatus.filesUploaded = uploadedCount;
        LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), smbBytes);
#ifdef ENABLE_TEST_WEBSERVER
        if (webServer) webServer->handleClient();
#endif
    }

    if (isRescan) {
        LOGF("[FileUploader] [SMB] Re-scan complete: %d uploaded, %d unchanged", uploadedCount, skippedUnchanged);
    } else {
        LOGF("[FileUploader] [SMB] Folder complete: %d files", uploadedCount);
    }

    // Per-folder disconnect (not per-file — avoids socket exhaustion)
    if (smbUploader->isConnected()) smbUploader->end();

    // Calculate actual upload success
    bool uploadSuccess = (uploadedCount == (int)files.size());
    smbStateManager->markFolderUploadProgress(folderName, files.size(), uploadedCount, uploadSuccess);
    
    if (uploadSuccess) {
        smbStateManager->markFolderCompletedWithScan(folderName, true);  // Recent scan passed
        LOGF("[FileUploader] [SMB] Folder upload successful: %s (%d/%d files)", folderName.c_str(), uploadedCount, (int)files.size());
    } else {
        smbStateManager->markFolderRecentScanFailed(folderName);  // Recent scan failed
        LOG_WARNF("[FileUploader] [SMB] Folder upload incomplete: %s (%d/%d files)", folderName.c_str(), uploadedCount, (int)files.size());
    }
    
    smbStateManager->save(sd);
    return uploadSuccess;
#endif
}

// ── SMB: upload a single root/SETTINGS file ──────────────────────────────────
bool FileUploader::uploadSingleFileSmb(SDCardManager* sdManager, const String& filePath, bool force) {
#ifndef ENABLE_SMB_UPLOAD
    return true;
#else
    if (!smbUploader || !smbStateManager) return false;
    fs::FS &sd = sdManager->getFS();

    if (!sd.exists(filePath)) return true;  // file absent — not an error

    File f = sd.open(filePath);
    if (!f) { LOG_ERRORF("[FileUploader] [SMB] Cannot open: %s", filePath.c_str()); return false; }
    unsigned long fileSize = f.size();
    f.close();

    if (fileSize == 0) return true;

    if (!force && !smbStateManager->hasFileChanged(sd, filePath)) {
        LOG_DEBUGF("[FileUploader] [SMB] Unchanged, skipping: %s", filePath.c_str());
        return true;
    }

    LOGF("[FileUploader] Uploading single file: %s", filePath.c_str());

    if (!smbUploader->isConnected() && !smbUploader->begin()) {
        LOG_ERROR("[FileUploader] [SMB] Connection failed");
        return false;
    }
    unsigned long smbBytes = 0;
    if (!smbUploader->upload(filePath, filePath, sd, smbBytes)) {
        LOG_ERRORF("[FileUploader] [SMB] Upload failed: %s", filePath.c_str());
        return false;
    }
    String checksum = smbStateManager->calculateChecksum(sd, filePath);
    if (!checksum.isEmpty()) smbStateManager->markFileUploaded(filePath, checksum, fileSize);

    LOGF("[FileUploader] Successfully uploaded: %s (%lu bytes)", filePath.c_str(), smbBytes);
    return true;
#endif
}

// ── SMB: upload all mandatory root + SETTINGS files ──────────────────────────
bool FileUploader::uploadMandatoryFilesSmb(SDCardManager* sdManager, fs::FS &sd) {
#ifndef ENABLE_SMB_UPLOAD
    return true;
#else
    LOG("[FileUploader] [SMB] Uploading mandatory root files...");
    const char* rootPaths[] = {
        "/Identification.json", "/Identification.crc", "/Identification.tgt", "/STR.edf"
    };
    for (const char* path : rootPaths) {
        if (sd.exists(path)) uploadSingleFileSmb(sdManager, String(path), false);
    }
    std::vector<String> settingsFiles = scanSettingsFiles(sd);
    for (const String& fp : settingsFiles) {
        uploadSingleFileSmb(sdManager, fp, false);
    }
    if (smbStateManager) smbStateManager->save(sd);
    return true;
#endif
}

// ============================================================================
// Cloud PASS: upload all DATALOG files for one folder (SleepHQ)
// ============================================================================
bool FileUploader::uploadDatalogFolderCloud(SDCardManager* sdManager, const String& folderName) {
#ifndef ENABLE_SLEEPHQ_UPLOAD
    return true;
#else
    if (!sleephqUploader || !cloudStateManager) return false;
    fs::FS &sd = sdManager->getFS();

    LOGF("[FileUploader] [Cloud] Uploading DATALOG folder: %s", folderName.c_str());
    String folderPath = "/DATALOG/" + folderName;

    std::vector<String> files;
    if (!handleFolderScan(sd, folderName, folderPath, cloudStateManager, files,
            [this](fs::FS& sd2, const String& fp) { return scanFolderFiles(sd2, fp); })) {
        return false;
    }
    if (files.empty()) return true;

    bool isRecent = isRecentFolder(folderName);
    bool isRescan = cloudStateManager->isFolderCompleted(folderName) && isRecent;

    g_cloudSessionStatus.uploadActive = true;
    strncpy((char*)g_cloudSessionStatus.currentFolder, folderName.c_str(),
            sizeof(g_cloudSessionStatus.currentFolder) - 1);
    ((char*)g_cloudSessionStatus.currentFolder)[sizeof(g_cloudSessionStatus.currentFolder) - 1] = '\0';
    g_cloudSessionStatus.filesTotal    = (int)files.size();
    g_cloudSessionStatus.filesUploaded = 0;

    int uploadedCount    = 0;
    int skippedUnchanged = 0;

    // Import was created eagerly in begin() before this folder loop starts
    if (cloudImportFailed || sleephqUploader->getCurrentImportId().isEmpty()) {
        LOG_WARN("[FileUploader] [Cloud] No active import — skipping folder");
        return true;
    }

    for (const String& fileName : files) {
        String localPath = folderPath + "/" + fileName;
        if (isRescan) {
            if (!cloudStateManager->hasFileChanged(sd, localPath)) { skippedUnchanged++; continue; }
            LOG_DEBUGF("[FileUploader] [Cloud] File changed: %s", fileName.c_str());
        }
        File f = sd.open(localPath);
        if (!f) { LOG_ERRORF("[FileUploader] [Cloud] Cannot open: %s", localPath.c_str()); continue; }
        unsigned long fileSize = f.size();
        f.close();
        if (fileSize == 0) {
            cloudStateManager->markFileUploaded(localPath, "empty_file", 0);
            continue;
        }

        LOGF("[FileUploader] Uploading file: %s (%lu bytes)", fileName.c_str(), fileSize);

        if (!sleephqUploader->isConnected() && !sleephqUploader->begin()) {
            LOG_ERROR("[FileUploader] [Cloud] Connection failed");
            cloudStateManager->save(sd);
            return false;
        }
        unsigned long cloudBytes = 0;
        String cloudChecksum = "";
        if (!sleephqUploader->upload(localPath, localPath, sd, cloudBytes, cloudChecksum)) {
            LOG_ERRORF("[FileUploader] [Cloud] Upload failed: %s", localPath.c_str());
            LOGF("[FileUploader] Successfully uploaded %d files before failure", uploadedCount);
            cloudStateManager->save(sd);
            return false;
        }
        if (isRecent) cloudStateManager->markFileUploaded(localPath, "", fileSize);
        uploadedCount++;
        cloudDatalogFilesUploaded++;
        g_cloudSessionStatus.filesUploaded = uploadedCount;
        LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), cloudBytes);
#ifdef ENABLE_TEST_WEBSERVER
        if (webServer) webServer->handleClient();
#endif
    }

    if (isRescan) {
        LOGF("[FileUploader] [Cloud] Re-scan complete: %d uploaded, %d unchanged", uploadedCount, skippedUnchanged);
    } else {
        LOGF("[FileUploader] [Cloud] Folder complete: %d files", uploadedCount);
    }

    // Calculate actual upload success
    bool uploadSuccess = (uploadedCount == (int)files.size());
    cloudStateManager->markFolderUploadProgress(folderName, files.size(), uploadedCount, uploadSuccess);
    
    if (uploadSuccess) {
        cloudStateManager->markFolderCompletedWithScan(folderName, true);  // Recent scan passed
        LOGF("[FileUploader] [Cloud] Folder upload successful: %s (%d/%d files)", folderName.c_str(), uploadedCount, (int)files.size());
    } else {
        cloudStateManager->markFolderRecentScanFailed(folderName);  // Recent scan failed
        LOG_WARNF("[FileUploader] [Cloud] Folder upload incomplete: %s (%d/%d files)", folderName.c_str(), uploadedCount, (int)files.size());
    }
    
    cloudStateManager->save(sd);
    return uploadSuccess;
#endif
}

// ── Cloud: upload a single root/SETTINGS file ────────────────────────────────
bool FileUploader::uploadSingleFileCloud(SDCardManager* sdManager, const String& filePath, bool force) {
#ifndef ENABLE_SLEEPHQ_UPLOAD
    return true;
#else
    if (!sleephqUploader || !cloudStateManager) return false;
    fs::FS &sd = sdManager->getFS();

    if (!sd.exists(filePath)) return true;

    File f = sd.open(filePath);
    if (!f) return false;
    unsigned long fileSize = f.size();
    f.close();
    if (fileSize == 0) return true;

    if (!force && !cloudStateManager->hasFileChanged(sd, filePath)) {
        LOG_DEBUGF("[FileUploader] [Cloud] Unchanged, skipping: %s", filePath.c_str());
        return true;
    }

    LOGF("[FileUploader] Uploading single file: %s", filePath.c_str());

    if (!sleephqUploader->isConnected() && !sleephqUploader->begin()) {
        LOG_ERROR("[FileUploader] [Cloud] Connection failed");
        return false;
    }
    unsigned long cloudBytes = 0;
    String cloudChecksum = "";
    if (!sleephqUploader->upload(filePath, filePath, sd, cloudBytes, cloudChecksum)) {
        LOG_ERRORF("[FileUploader] [Cloud] Upload failed: %s", filePath.c_str());
        return false;
    }
    String checksum = cloudChecksum.isEmpty()
        ? cloudStateManager->calculateChecksum(sd, filePath)
        : cloudChecksum;
    if (!checksum.isEmpty()) cloudStateManager->markFileUploaded(filePath, checksum, fileSize);

    LOGF("[FileUploader] Successfully uploaded: %s (%lu bytes)", filePath.c_str(), cloudBytes);
    return true;
#endif
}

