#include "FileUploader.h"
#include "Logger.h"
#include "WebStatus.h"
#include <SD_MMC.h>
#include <LittleFS.h>
#include <functional>
#include <time.h>

#ifdef ENABLE_WEBSERVER
#include "CpapWebServer.h"
#endif

// Cooperative abort flag — set by web server when config lock is requested during upload
extern volatile bool g_abortUploadFlag;

// Constructor
FileUploader::FileUploader(Config* cfg, WiFiManager* wifiManager) 
    : config(cfg),
      smbStateManager(nullptr),
      cloudStateManager(nullptr),
      scheduleManager(nullptr),
      wifiManager(wifiManager),
      currentPhase(UploadBackend::NONE),
#ifdef ENABLE_WEBSERVER
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
bool FileUploader::begin() {
    LOG("[FileUploader] Initializing components...");

    fs::FS &stateFs = LittleFS;

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

        smbStateManager = new UploadStateManager();
        smbStateManager->setPaths("/.upload_state.v2.smb", "/.upload_state.v2.smb.log");
        if (!smbStateManager->begin(stateFs)) {
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
        if (!cloudStateManager->begin(stateFs)) {
            LOG("[FileUploader] WARNING: Cloud state load failed, starting fresh");
        }
        anyBackendCreated = true;
    }
#endif

    if (!anyBackendCreated) {
        LOGF("[FileUploader] ERROR: No uploader created for endpoint type: %s", endpointType.c_str());
        return false;
    }

    // Populate GUI backend status
    const char* mode = hasBothBackends() ? "DUAL" :
                       hasCloudBackend() ? "CLOUD" :
                       hasSmbBackend()   ? "SMB"   : "NONE";
    strncpy(g_activeBackendStatus.name, mode, sizeof(g_activeBackendStatus.name) - 1);
    g_activeBackendStatus.valid = anyBackendCreated;
    // Inactive backend display — not used in dual mode
    strncpy(g_inactiveBackendStatus.name, "NONE", sizeof(g_inactiveBackendStatus.name) - 1);
    g_inactiveBackendStatus.valid = false;

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
    UploadStateManager* sm = primaryStateManager();
    if (sm) scheduleManager->setLastUploadTimestamp(sm->getLastUploadTimestamp());

    LOG("[FileUploader] Initialization complete");
    return true;
}

// ============================================================================
// Phased dual-backend upload orchestrator
// ============================================================================
//
// Runs both backends sequentially in a single session:
//   Phase 1: CLOUD (TLS pre-warmed by caller before SD mount — uses cleanest
//            contiguous heap; falls back to on-demand connect if pre-warm failed)
//   Phase 2: SMB   (TLS torn down, clean sockets, more heap for libsmb2)
//
// TLS pre-warm happens in uploadTaskFunction() BEFORE SD_MMC.begin() so mbedTLS
// gets first pick of the unfragmented heap.  If the cloud pre-flight finds no
// work, a safety resetConnection() before Phase 2 ensures the lingering TLS
// socket doesn't conflict with libsmb2's TCP socket (errno:9).
//
// This eliminates backend cycling, prevents TLS/SMB socket conflicts, and
// ensures both backends make progress every session without reboots.
// ============================================================================

UploadResult FileUploader::runFullSession(SDCardManager* sdManager, int maxMinutes, DataFilter filter) {
    fs::FS &sd = sdManager->getFS();
    fs::FS &stateFs = LittleFS;
    unsigned long sessionStart = millis();
    unsigned long maxMs = (unsigned long)maxMinutes * 60UL * 1000UL;

    // When both backends are configured, auto-scale time budget so each
    // backend gets at least the configured minutes.
    bool dual = hasBothBackends();
    if (dual) {
        maxMs *= 2;
        LOGF("[FileUploader] Session start: DUAL mode, maxMinutes=%d×2=%d filter=%d",
             maxMinutes, maxMinutes * 2, (int)filter);
    } else {
        const char* mode = hasCloudBackend() ? "CLOUD" : hasSmbBackend() ? "SMB" : "NONE";
        LOGF("[FileUploader] Session start: %s mode, maxMinutes=%d filter=%d",
             mode, maxMinutes, (int)filter);
    }

    if (!wifiManager || !wifiManager->isConnected()) {
        LOG_ERROR("[FileUploader] WiFi not connected - cannot upload");
        return UploadResult::ERROR;
    }
    if (!hasSmbBackend() && !hasCloudBackend()) {
        LOG_ERROR("[FileUploader] No backend configured");
        return UploadResult::ERROR;
    }

    // ── Pre-flight: check every configured backend for pending work ──────────
    bool smbWork   = false;
    bool cloudWork = false;
    {
        // Compute once: can we process old (non-recent) folders this session?
        // When false (smart mode outside upload window), skip old folders entirely
        // to avoid unnecessary SD card I/O scanning hundreds of DATALOG folders.
        bool canUploadOld = !scheduleManager || scheduleManager->canUploadOldData();

        auto preflightFolderHasWork = [&](UploadStateManager* sm) -> bool {
            File root = sd.open("/DATALOG");
            if (!root || !root.isDirectory()) return false;
            File entry = root.openNextFile();
            while (entry) {
                if (entry.isDirectory()) {
                    String name = String(entry.name());
                    int sl = name.lastIndexOf('/');
                    if (sl >= 0) name = name.substring(sl + 1);

                    bool completed = sm->isFolderCompleted(name);
                    bool pending   = sm->isPendingFolder(name);
                    bool recent    = isRecentFolder(name);

                    // Fast path: skip old folders entirely when outside upload window.
                    // Only recent folders can produce work in this mode.  Pending-folder
                    // promotion (7-day timeout) is pure state management — still allowed.
                    if (!recent && !canUploadOld) {
                        // Still promote timed-out pending folders (no SD I/O needed)
                        if (!completed && pending) {
                            unsigned long currentTime = time(NULL);
                            if (currentTime >= 1000000000 &&
                                    sm->shouldPromotePendingToCompleted(name, currentTime)) {
                                sm->promotePendingToCompleted(name);
                                sm->save(stateFs);
                                LOGF("[FileUploader] Pre-flight: empty folder %s pending 7+ days — promoted to completed",
                                     name.c_str());
                            }
                        }
                        entry.close();
                        entry = root.openNextFile();
                        continue;
                    }

                    if (g_debugMode) {
                        LOGF("[FileUploader] Pre-flight scan: folder=%s completed=%d pending=%d recent=%d",
                             name.c_str(), completed, pending, recent);
                    }

                    if (!completed && !pending) {
                        String folderPath = "/DATALOG/" + name;
                        auto files = scanFolderFiles(sd, folderPath);
                        if (!files.empty()) {
                            LOGF("[FileUploader] Pre-flight: WORK — folder %s has %d file(s)",
                                 name.c_str(), (int)files.size());
                            entry.close(); root.close(); return true;
                        } else {
                            unsigned long currentTime = time(NULL);
                            if (currentTime >= 1000000000) {
                                sm->markFolderPending(name, currentTime);
                                sm->save(stateFs);
                                LOGF("[FileUploader] Pre-flight: empty folder %s — marked pending",
                                     name.c_str());
                            }
                        }
                    }
                    if (!completed && pending) {
                        String folderPath = "/DATALOG/" + name;
                        auto pendingFiles = scanFolderFiles(sd, folderPath);
                        if (!pendingFiles.empty()) {
                            LOGF("[FileUploader] Pre-flight: WORK — pending folder %s now has files",
                                     name.c_str());
                            entry.close(); root.close(); return true;
                        } else {
                            unsigned long currentTime = time(NULL);
                            if (currentTime >= 1000000000 &&
                                    sm->shouldPromotePendingToCompleted(name, currentTime)) {
                                sm->promotePendingToCompleted(name);
                                sm->save(stateFs);
                                LOGF("[FileUploader] Pre-flight: empty folder %s pending 7+ days — promoted to completed",
                                     name.c_str());
                            }
                        }
                    }
                    if (completed && recent) {
                        String folderPath = "/DATALOG/" + name;
                        auto files = scanFolderFiles(sd, folderPath);
                        for (const String& fp : files) {
                            String fullPath = folderPath + "/" + fp;
                            if (sm->hasFileChanged(sd, fullPath)) {
                                LOGF("[FileUploader] Pre-flight: WORK — file changed: %s",
                                     fullPath.c_str());
                                entry.close(); root.close(); return true;
                            }
                        }
                    }
                }
                entry.close();
                entry = root.openNextFile();
            }
            root.close();
            return false;
        };

        auto checkHasWork = [&](UploadStateManager* sm) -> bool {
            if (!sm) return false;
            return preflightFolderHasWork(sm);
        };

#ifdef ENABLE_SMB_UPLOAD
        if (config->hasSmbEndpoint())   smbWork   = checkHasWork(smbStateManager);
#endif
#ifdef ENABLE_SLEEPHQ_UPLOAD
        if (config->hasCloudEndpoint()) cloudWork = checkHasWork(cloudStateManager);
#endif
    }

    if (!smbWork && !cloudWork) {
        LOG("[FileUploader] Pre-flight: no work for any backend — skipping session");
        return UploadResult::NOTHING_TO_DO;
    }

    LOGF("[FileUploader] Pre-flight: smb_work=%d cloud_work=%d", smbWork, cloudWork);

    cloudImportCreated = false;
    cloudImportFailed  = false;

    bool timerExpired = false;
    auto isTimerExpired = [&]() -> bool {
        return (millis() - sessionStart) >= maxMs;
    };

    bool needFresh = (filter == DataFilter::FRESH_ONLY || filter == DataFilter::ALL_DATA);
    bool needOld   = (filter == DataFilter::OLD_ONLY   || filter == DataFilter::ALL_DATA);

    // Time budget: when both backends have work, split time evenly.
    // cloudDeadline marks when cloud phase must yield to SMB phase.
    unsigned long cloudDeadline = 0;
    if (cloudWork && smbWork) {
        cloudDeadline = sessionStart + (maxMs / 2);
    } else if (cloudWork) {
        cloudDeadline = sessionStart + maxMs;
    }
    auto isCloudTimeBudgetExpired = [&]() -> bool {
        return cloudDeadline > 0 && millis() >= cloudDeadline;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // PHASE 1: CLOUD (TLS connects on-demand in begin() — no pre-warm)
    // ═══════════════════════════════════════════════════════════════════════
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (cloudWork && sleephqUploader && cloudStateManager) {
        currentPhase = UploadBackend::CLOUD;
        strncpy(g_activeBackendStatus.name, "CLOUD", sizeof(g_activeBackendStatus.name) - 1);
        LOG("[FileUploader] === Phase 1: Cloud Session ===");
        cloudDatalogFilesUploaded = 0;

        std::vector<String> freshFolders, oldFolders;
        if (needFresh || needOld) {
            std::vector<String> all = scanDatalogFolders(sd, cloudStateManager);
            for (const String& f : all) {
                if (isRecentFolder(f)) freshFolders.push_back(f);
                else                   oldFolders.push_back(f);
            }
            LOGF("[FileUploader] Cloud scan: %d fresh, %d old folders",
                 (int)freshFolders.size(), (int)oldFolders.size());
        }

        bool cloudHasWork = !freshFolders.empty() ||
                       (!oldFolders.empty() && scheduleManager && scheduleManager->canUploadOldData());

        if (!cloudHasWork) {
            LOG("[FileUploader] Cloud: nothing to upload — skipping auth + import");
        } else {
            LOGF("[FileUploader] Heap before cloud begin: fh=%u ma=%u",
                 (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
            if (!sleephqUploader->isConnected()) {
                if (!sleephqUploader->begin()) {
                    LOG_ERROR("[FileUploader] Cloud init failed — skipping cloud phase");
                    cloudImportFailed = true;
                } else {
                    cloudImportCreated = true;
                    LOGF("[FileUploader] Cloud session ready — heap: fh=%u ma=%u",
                         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
                }
            } else {
                if (!cloudImportCreated && !cloudImportFailed) {
                    if (!sleephqUploader->createImport()) cloudImportFailed = true;
                    else                                  cloudImportCreated = true;
                }
            }

            if (!cloudImportFailed) {
                auto runCloudFolder = [&](const String& folder) -> bool {
                    if (isTimerExpired() || isCloudTimeBudgetExpired()) { timerExpired = true; return false; }
                    uploadDatalogFolderCloud(sdManager, folder);
#ifdef ENABLE_WEBSERVER
                    if (webServer) webServer->handleClient();
#endif
                    return true;
                };
                if (!timerExpired && needFresh) {
                    LOG("[FileUploader] Cloud: Fresh DATALOG folders");
                    for (const String& folder : freshFolders) {
                        if (!runCloudFolder(folder)) break;
                    }
                }
                if (!timerExpired && needOld && scheduleManager && scheduleManager->canUploadOldData()) {
                    LOG("[FileUploader] Cloud: Old DATALOG folders");
                    for (const String& folder : oldFolders) {
                        if (!runCloudFolder(folder)) break;
                    }
                }
                if (cloudImportCreated && cloudDatalogFilesUploaded > 0) {
                    LOGF("[FileUploader] Finalizing import: %d DATALOG files", cloudDatalogFilesUploaded);
                    finalizeCloudImport(sdManager, sd);
                } else if (cloudImportCreated && cloudDatalogFilesUploaded == 0) {
                    LOG("[FileUploader] No new DATALOG files — skipping import finalize");
                }
            }
        }
        cloudStateManager->save(stateFs);

        // Release TLS resources — frees ~40KB heap for SMB phase and clears
        // the lwIP socket table to prevent errno:9 on libsmb2 connects.
        sleephqUploader->resetConnection();
        LOGF("[FileUploader] Released TLS resources after cloud phase (fh=%u ma=%u)",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        delay(100);  // lwIP socket cleanup

        g_cloudSessionStatus.uploadActive     = false;
        g_cloudSessionStatus.filesUploaded    = 0;
        g_cloudSessionStatus.filesTotal       = 0;
        g_cloudSessionStatus.currentFolder[0] = '\0';

        // Reset timer flag for SMB phase — cloud budget expiry shouldn't block SMB
        timerExpired = false;
    }
#endif

    // ═══════════════════════════════════════════════════════════════════════
    // PHASE 2: SMB (TLS torn down, clean sockets, more heap for libsmb2)
    // ═══════════════════════════════════════════════════════════════════════
    // Safety: ensure TLS is released before SMB starts.  When TLS is
    // pre-warmed (before SD mount for clean heap) but cloud pre-flight
    // finds no work, the normal cloud-phase cleanup is skipped.  The
    // lingering lwIP socket conflicts with libsmb2's TCP socket (errno:9).
    // resetConnection() is safe to call when already disconnected.
#ifdef ENABLE_SLEEPHQ_UPLOAD
    if (smbWork && sleephqUploader) {
        // Always release TLS resources, even if the connection died silently
        // during pre-flight scanning.  When the server closes an idle
        // pre-warmed connection, isConnected() returns false but mbedTLS
        // internal buffers (~32 KB) may still be allocated — fragmenting the
        // heap and starving lwIP during the SMB phase.
        // resetConnection() is safe to call when already disconnected.
        LOG("[FileUploader] Releasing TLS resources before SMB phase");
        sleephqUploader->resetConnection();
        delay(100);  // lwIP socket cleanup
    }
#endif
#ifdef ENABLE_SMB_UPLOAD
    if (smbWork && smbUploader && smbStateManager) {
        currentPhase = UploadBackend::SMB;
        strncpy(g_activeBackendStatus.name, "SMB", sizeof(g_activeBackendStatus.name) - 1);
        LOG("[FileUploader] === Phase 2: SMB Session ===");

        // Allocate SMB buffer dynamically based on current heap state
        uint32_t currentMa = ESP.getMaxAllocHeap();
        size_t smbBufSize = (currentMa > 80000) ? 8192 :
                            (currentMa > 50000) ? 4096 :
                            (currentMa > 30000) ? 2048 : 1024;
        LOGF("[FileUploader] SMB phase heap: fh=%u ma=%u, buffer=%u",
             (unsigned)ESP.getFreeHeap(), (unsigned)currentMa, (unsigned)smbBufSize);
        if (!smbUploader->allocateBuffer(smbBufSize)) {
            LOG_ERROR("[FileUploader] Failed to allocate SMB buffer — skipping SMB phase");
            smbStateManager->save(stateFs);
        } else {
            std::vector<String> freshFolders, oldFolders;
            if (needFresh || needOld) {
                std::vector<String> all = scanDatalogFolders(sd, smbStateManager);
                for (const String& f : all) {
                    if (isRecentFolder(f)) freshFolders.push_back(f);
                    else                   oldFolders.push_back(f);
                }
                LOGF("[FileUploader] SMB scan: %d fresh, %d old folders",
                     (int)freshFolders.size(), (int)oldFolders.size());
            }

            bool mandatoryChanged = false;
            {
                static const char* rootPaths[] = {
                    "/Identification.json", "/Identification.crc",
                    "/Identification.tgt",  "/STR.edf"
                };
                for (const char* p : rootPaths) {
                    if (sd.exists(p) && smbStateManager->hasFileChanged(sd, String(p))) {
                        mandatoryChanged = true; break;
                    }
                }
                if (!mandatoryChanged) {
                    for (const String& fp : scanSettingsFiles(sd)) {
                        if (smbStateManager->hasFileChanged(sd, fp)) {
                            mandatoryChanged = true; break;
                        }
                    }
                }
            }

            bool smbHasWork = !freshFolders.empty() ||
                           (!oldFolders.empty() && scheduleManager && scheduleManager->canUploadOldData()) ||
                           mandatoryChanged;

            if (!smbHasWork) {
                LOG("[FileUploader] SMB: nothing to upload — skipping");
            } else {
                if (!isTimerExpired()) uploadMandatoryFilesSmb(sdManager, sd);

                if (!timerExpired && needFresh) {
                    LOG("[FileUploader] SMB: Fresh DATALOG folders");
                    for (const String& folder : freshFolders) {
                        if (isTimerExpired()) { timerExpired = true; break; }
                        uploadDatalogFolderSmb(sdManager, folder);
#ifdef ENABLE_WEBSERVER
                        if (webServer) webServer->handleClient();
#endif
                    }
                }
                if (!timerExpired && needOld && scheduleManager && scheduleManager->canUploadOldData()) {
                    LOG("[FileUploader] SMB: Old DATALOG folders");
                    for (const String& folder : oldFolders) {
                        if (isTimerExpired()) { timerExpired = true; break; }
                        uploadDatalogFolderSmb(sdManager, folder);
#ifdef ENABLE_WEBSERVER
                        if (webServer) webServer->handleClient();
#endif
                    }
                }
                if (smbUploader->isConnected()) smbUploader->end();
            }
            smbStateManager->save(stateFs);

            // Free SMB buffer to recover heap for next session
            smbUploader->freeBuffer();
        }

        g_smbSessionStatus.uploadActive     = false;
        g_smbSessionStatus.filesUploaded    = 0;
        g_smbSessionStatus.filesTotal       = 0;
        g_smbSessionStatus.currentFolder[0] = '\0';
    }
#endif

    currentPhase = UploadBackend::NONE;
    // Restore GUI status to show configured mode
    const char* mode = dual ? "DUAL" : hasCloudBackend() ? "CLOUD" : "SMB";
    strncpy(g_activeBackendStatus.name, mode, sizeof(g_activeBackendStatus.name) - 1);

    // ── Determine result ──────────────────────────────────────────────────────
    unsigned long elapsed = millis() - sessionStart;

    // Log stack high-water mark for tuning task stack size
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    LOGF("[FileUploader] Session ended: %lu seconds, stack HWM=%u bytes free",
         elapsed / 1000, (unsigned)(hwm * sizeof(StackType_t)));

    if (timerExpired && hasIncompleteFolders()) {
        LOG("[FileUploader] Timer expired with incomplete folders (TIMEOUT)");
        return UploadResult::TIMEOUT;
    }

    if (!hasIncompleteFolders()) {
        time_t endNow; time(&endNow);
        UploadStateManager* sm = primaryStateManager();
        if (sm) sm->setLastUploadTimestamp((unsigned long)endNow);
        if (scheduleManager) scheduleManager->markDayCompleted();
        LOG("[FileUploader] All folders complete — session done");
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
                } else if (isRecentFolder(folderName)) {
                    // Recent completed folders are always rescanned — CPAP may have added
                    // or extended files. Per-file size tracking (hasFileChanged) skips
                    // unchanged files so only new/modified data is re-uploaded.
                    folders.push_back(folderName);
                    LOG_DEBUGF("[FileUploader] Recent completed folder — rescanning: %s", folderName.c_str());
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

    if (sm) sm->setTotalFoldersCount(folders.size());
    
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
static bool handleFolderScan(fs::FS &sd, fs::FS &stateFs, const String& folderName, const String& folderPath,
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
                sm->save(stateFs);
            }
        } else {
            sm->markFolderPending(folderName, currentTime);
            sm->save(stateFs);
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
    fs::FS &stateFs = LittleFS;

    LOGF("[FileUploader] [SMB] Uploading DATALOG folder: %s", folderName.c_str());
    String folderPath = "/DATALOG/" + folderName;

    std::vector<String> files;
    if (!handleFolderScan(sd, stateFs, folderName, folderPath, smbStateManager, files,
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

    int uploadedCount    = 0;
    int skippedUnchanged = 0;
    int skippedEmpty     = 0;

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
            skippedEmpty++;
            continue;
        }
        f.close();

        LOGF("[FileUploader] Uploading file: %s (%lu bytes)", fileName.c_str(), fileSize);

        if (!smbUploader->isConnected()) {
            if (!smbUploader->begin()) {
                LOG_ERROR("[FileUploader] [SMB] Failed to connect");
                smbStateManager->save(stateFs);
                return false;
            }
        }
        unsigned long smbBytes = 0;
        if (!smbUploader->upload(localPath, localPath, sd, smbBytes)) {
            LOG_ERRORF("[FileUploader] [SMB] Upload failed: %s", localPath.c_str());
            LOGF("[FileUploader] Successfully uploaded %d files before failure", uploadedCount);
            smbStateManager->save(stateFs);
            return false;
        }
        if (isRecent) smbStateManager->markFileUploaded(localPath, "", fileSize);
        uploadedCount++;
        g_smbSessionStatus.filesUploaded = uploadedCount;
        if (g_debugMode) LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), smbBytes);
#ifdef ENABLE_WEBSERVER
        if (webServer) webServer->handleClient();
#endif
        if (g_abortUploadFlag) {
            LOG_WARN("[FileUploader] [SMB] Abort requested — stopping upload cleanly");
            smbStateManager->save(stateFs);
            return false;
        }
    }

    if (isRescan) {
        LOGF("[FileUploader] [SMB] Re-scan complete: %d uploaded, %d unchanged", uploadedCount, skippedUnchanged);
    } else {
        LOGF("[FileUploader] [SMB] Folder complete: %d files", uploadedCount);
    }

    // Per-folder disconnect (not per-file — avoids socket exhaustion)
    if (smbUploader->isConnected()) smbUploader->end();

    bool uploadSuccess = (uploadedCount == (int)files.size() - skippedUnchanged - skippedEmpty);
    LOGF("[FileUploader] [SMB] Folder %s: %d/%d files, %d unchanged, %d empty — success=%s",
         folderName.c_str(), uploadedCount, (int)files.size(), skippedUnchanged, skippedEmpty,
         uploadSuccess ? "yes" : "no");

    // Mark-complete strategy:
    //   Recent folders — always mark complete so the next scan uses the isRescan path
    //   (per-file size entries track which files need re-upload).
    //   Old folders — only mark complete when every file was uploaded (enables full retry
    //   of partially-uploaded old folders on the next session).
    if (isRecent) {
        smbStateManager->markFolderCompleted(folderName);
    } else if (uploadSuccess) {
        smbStateManager->markFolderCompleted(folderName);
    }

    smbStateManager->save(stateFs);
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
    fs::FS &stateFs = LittleFS;

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
    if (smbStateManager) smbStateManager->save(stateFs);
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
    fs::FS &stateFs = LittleFS;

    LOGF("[FileUploader] [Cloud] Uploading DATALOG folder: %s", folderName.c_str());
    String folderPath = "/DATALOG/" + folderName;

    std::vector<String> files;
    if (!handleFolderScan(sd, stateFs, folderName, folderPath, cloudStateManager, files,
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
    int skippedEmpty     = 0;

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
            skippedEmpty++;
            continue;
        }

        LOGF("[FileUploader] Uploading file: %s (%lu bytes)", fileName.c_str(), fileSize);

        if (!sleephqUploader->isConnected() && !sleephqUploader->begin()) {
            LOG_ERROR("[FileUploader] [Cloud] Connection failed");
            cloudStateManager->save(stateFs);
            return false;
        }
        unsigned long cloudBytes = 0;
        String cloudChecksum = "";
        if (!sleephqUploader->upload(localPath, localPath, sd, cloudBytes, cloudChecksum)) {
            LOG_ERRORF("[FileUploader] [Cloud] Upload failed: %s", localPath.c_str());
            LOGF("[FileUploader] Successfully uploaded %d files before failure", uploadedCount);
            cloudStateManager->save(stateFs);
            return false;
        }
        if (isRecent) cloudStateManager->markFileUploaded(localPath, "", fileSize);
        uploadedCount++;
        cloudDatalogFilesUploaded++;
        g_cloudSessionStatus.filesUploaded = uploadedCount;
        if (g_debugMode) LOGF("[FileUploader] Uploaded: %s (%lu bytes)", fileName.c_str(), cloudBytes);
#ifdef ENABLE_WEBSERVER
        if (webServer) webServer->handleClient();
#endif
        if (g_abortUploadFlag) {
            LOG_WARN("[FileUploader] [Cloud] Abort requested — stopping upload cleanly");
            cloudStateManager->save(stateFs);
            return false;
        }
    }

    if (isRescan) {
        LOGF("[FileUploader] [Cloud] Re-scan complete: %d uploaded, %d unchanged", uploadedCount, skippedUnchanged);
    } else {
        LOGF("[FileUploader] [Cloud] Folder complete: %d files", uploadedCount);
    }

    bool uploadSuccess = (uploadedCount == (int)files.size() - skippedUnchanged - skippedEmpty);
    LOGF("[FileUploader] [Cloud] Folder %s: %d/%d files, %d unchanged, %d empty — success=%s",
         folderName.c_str(), uploadedCount, (int)files.size(), skippedUnchanged, skippedEmpty,
         uploadSuccess ? "yes" : "no");

    // Mark-complete strategy: same as SMB — recent always, old only on full success.
    if (isRecent) {
        cloudStateManager->markFolderCompleted(folderName);
    } else if (uploadSuccess) {
        cloudStateManager->markFolderCompleted(folderName);
    }

    cloudStateManager->save(stateFs);
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

