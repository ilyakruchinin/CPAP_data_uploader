#include <Arduino.h>
#include <esp_system.h>
#include "Config.h"
#include "SDCardManager.h"
#include "WiFiManager.h"
#include "FileUploader.h"
#include "Logger.h"
#include "pins_config.h"
#include "version.h"

#include "TrafficMonitor.h"
#include "UploadFSM.h"

#ifdef ENABLE_OTA_UPDATES
#include "OTAManager.h"
#endif

#ifdef ENABLE_TEST_WEBSERVER
#include "TestWebServer.h"
#include "CPAPMonitor.h"
#endif

// ============================================================================
// Global Objects
// ============================================================================
Config config;
SDCardManager sdManager;
WiFiManager wifiManager;
FileUploader* uploader = nullptr;
TrafficMonitor trafficMonitor;

#ifdef ENABLE_OTA_UPDATES
OTAManager otaManager;
#endif

#ifdef ENABLE_TEST_WEBSERVER
TestWebServer* testWebServer = nullptr;
CPAPMonitor* cpapMonitor = nullptr;
#endif

// ============================================================================
// Upload FSM State
// ============================================================================
UploadState currentState = UploadState::IDLE;
unsigned long stateEnteredAt = 0;
unsigned long cooldownStartedAt = 0;
bool uploadCycleHadTimeout = false;

// Monitoring mode flags
bool monitoringRequested = false;
bool stopMonitoringRequested = false;

// IDLE state periodic check
unsigned long lastIdleCheck = 0;
const unsigned long IDLE_CHECK_INTERVAL_MS = 60000;  // 60 seconds

// FreeRTOS upload task (runs upload on separate core for web server responsiveness)
volatile bool uploadTaskRunning = false;
volatile bool uploadTaskComplete = false;
volatile UploadResult uploadTaskResult = UploadResult::ERROR;
TaskHandle_t uploadTaskHandle = nullptr;

struct UploadTaskParams {
    FileUploader* uploader;
    SDCardManager* sdManager;
    int maxMinutes;
    DataFilter filter;
};

// ============================================================================
// Global State (legacy + shared)
// ============================================================================
unsigned long lastNtpSyncAttempt = 0;
const unsigned long NTP_RETRY_INTERVAL_MS = 5 * 60 * 1000;  // 5 minutes

unsigned long lastWifiReconnectAttempt = 0;
unsigned long lastSdCardRetry = 0;

// SD card logging periodic dump timing
unsigned long lastLogDumpTime = 0;
const unsigned long LOG_DUMP_INTERVAL_MS = 10 * 1000;  // 10 seconds

#ifdef ENABLE_TEST_WEBSERVER
// External trigger flags (defined in TestWebServer.cpp)
extern volatile bool g_triggerUploadFlag;
extern volatile bool g_resetStateFlag;

// Monitoring trigger flags (defined in TestWebServer.cpp)
extern volatile bool g_monitorActivityFlag;
extern volatile bool g_stopMonitorFlag;
#endif

// ============================================================================
// FSM Helper
// ============================================================================
void transitionTo(UploadState newState) {
    LOGF("[FSM] %s -> %s", getStateName(currentState), getStateName(newState));
    currentState = newState;
    stateEnteredAt = millis();
}

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * Convert ESP32 reset reason to human-readable string
 * Useful for diagnosing power issues, crashes, and unexpected resets
 */
const char* getResetReasonString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_UNKNOWN:
            return "Unknown";
        case ESP_RST_POWERON:
            return "Power-on reset (normal startup)";
        case ESP_RST_EXT:
            return "External reset via EN pin";
        case ESP_RST_SW:
            return "Software reset via esp_restart()";
        case ESP_RST_PANIC:
            return "Software panic/exception";
        case ESP_RST_INT_WDT:
            return "Interrupt watchdog timeout";
        case ESP_RST_TASK_WDT:
            return "Task watchdog timeout";
        case ESP_RST_WDT:
            return "Other watchdog timeout";
        case ESP_RST_DEEPSLEEP:
            return "Wake from deep sleep";
        case ESP_RST_BROWNOUT:
            return "Brown-out reset (low voltage)";
        case ESP_RST_SDIO:
            return "SDIO reset";
        default:
            return "Unrecognized reset reason";
    }
}

// ============================================================================
// Setup Function
// ============================================================================
void setup() {
    // Initialize serial port
    Serial.begin(115200);
    
    // CRITICAL: Immediately release SD card control to CPAP machine
    // This must happen before any delays to prevent CPAP machine errors
    // Initialize control pins
    pinMode(CS_SENSE, INPUT_PULLUP);
    pinMode(SD_SWITCH_PIN, OUTPUT);
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
    
    delay(1000);
    LOG("\n\n=== CPAP Data Auto-Uploader ===");
    LOGF("Firmware Version: %s", FIRMWARE_VERSION);
    LOGF("Build Info: %s", BUILD_INFO);
    LOGF("Build Time: %s", FIRMWARE_BUILD_TIME);
    
    // Log reset reason for power/stability diagnostics
    esp_reset_reason_t resetReason = esp_reset_reason();
    LOG_INFOF("Reset reason: %s", getResetReasonString(resetReason));
    
    // Check for power-related issues
    if (resetReason == ESP_RST_BROWNOUT) {
        LOG_ERROR("WARNING: System reset due to brown-out (insufficient power supply), this could be caused by:");
        LOG_ERROR(" - the CPAP was disconnected from the power supply");
        LOG_ERROR(" - the card was removed");
        LOG_ERROR(" - the CPAP machine cannot provide enough power");
    } else if (resetReason == ESP_RST_PANIC) {
        LOG_WARN("System reset due to software panic - check for stability issues");
    } else if (resetReason == ESP_RST_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_INT_WDT) {
        LOG_WARN("System reset due to watchdog timeout - possible hang or power issue");
    }

    // Initialize SD card control
    if (!sdManager.begin()) {
        LOG("Failed to initialize SD card manager");
        return;
    }
    
    // Initialize TrafficMonitor (PCNT-based bus activity detection on CS_SENSE pin)
    trafficMonitor.begin(CS_SENSE);

    // Boot delay - wait for CPAP machine to finish booting and release SD card
    // This delay is applied before first SD card access attempt
    // Note: Config not loaded yet, so we'll apply a default delay here
    // and use configured delay for subsequent operations
    const int DEFAULT_BOOT_DELAY_SECONDS = 30;
    LOGF("Waiting %d seconds for CPAP machine to complete boot sequence...", DEFAULT_BOOT_DELAY_SECONDS);
    delay(DEFAULT_BOOT_DELAY_SECONDS * 1000);
    LOG("Boot delay complete, attempting SD card access...");

    // Take control of SD card
    LOG("Waiting to access SD card...");
    while (!sdManager.takeControl()) {
        delay(1000);
    }

    // Read config file from SD card
    LOG("Loading configuration...");
    if (!config.loadFromSD(sdManager.getFS())) {
        LOG_ERROR("Failed to load configuration - cannot continue");
        LOG_ERROR("Please check config.json file on SD card");
        
        // Dump logs to SD card for configuration failures
        Logger::getInstance().dumpLogsToSDCard("config_load_failed");
        
        sdManager.releaseControl();
        return;
    }

    LOG("Configuration loaded successfully");
    LOG_DEBUGF("WiFi SSID: %s", config.getWifiSSID().c_str());
    LOG_DEBUGF("Endpoint: %s", config.getEndpoint().c_str());

    // Configure SD card logging if enabled (debugging only; can block CPAP SD access)
    if (config.getLogToSdCard()) {
        LOG_WARN("Enabling SD card logging - DEBUGGING ONLY - may block CPAP SD access; use scheduled mode outside therapy times");
        LOG_WARN("Logs will be dumped every 10 seconds");
        Logger::getInstance().enableSdCardLogging(true, &sdManager.getFS());
    }

    // Release SD card back to CPAP machine
    sdManager.releaseControl();

    // Apply power management settings from config
    LOG("Applying power management settings...");
    
    // Set CPU frequency
    int targetCpuMhz = config.getCpuSpeedMhz();
    setCpuFrequencyMhz(targetCpuMhz);
    LOGF("CPU frequency set to %dMHz", getCpuFrequencyMhz());

    // Setup WiFi event handlers for logging
    wifiManager.setupEventHandlers();

    // Initialize WiFi in station mode
    if (!wifiManager.connectStation(config.getWifiSSID(), config.getWifiPassword())) {
        LOG("Failed to connect to WiFi");
        // Note: WiFiManager already dumps logs to SD card on connection failures
        return;
    }
    
    // Apply WiFi power settings after connection is established
    wifiManager.applyPowerSettings(config.getWifiTxPower(), config.getWifiPowerSaving());
    LOG("WiFi power management settings applied");

    // Initialize uploader
    uploader = new FileUploader(&config, &wifiManager);
    
    // Take control of SD card to initialize uploader components
    LOG("Initializing uploader...");
    if (sdManager.takeControl()) {
        if (!uploader->begin(sdManager.getFS())) {
            LOG_ERROR("Failed to initialize uploader");
            sdManager.releaseControl();
            return;
        }
        sdManager.releaseControl();
        LOG("Uploader initialized successfully");
    } else {
        LOG_ERROR("Failed to take SD card control for uploader initialization");
        return;
    }
    
#ifdef ENABLE_OTA_UPDATES
    // Initialize OTA manager
    LOG("Initializing OTA manager...");
    if (!otaManager.begin()) {
        LOG_ERROR("Failed to initialize OTA manager");
        return;
    }
    otaManager.setCurrentVersion(VERSION_STRING);
    LOG("OTA manager initialized successfully");
    LOGF("OTA Version: %s", VERSION_STRING);
#endif
    
    // Synchronize time with NTP server
    LOG("Synchronizing time with NTP server...");
    ScheduleManager* sm = uploader->getScheduleManager();
    if (sm && sm->isTimeSynced()) {
        LOG("Time synchronized successfully");
        LOGF("System time: %s", sm->getCurrentLocalTime().c_str());
    } else {
        LOG_DEBUG("Time sync not yet available, will retry every 5 minutes");
        lastNtpSyncAttempt = millis();
    }

#ifdef ENABLE_TEST_WEBSERVER
    // Initialize CPAP monitor
#ifdef ENABLE_CPAP_MONITOR
    LOG("Initializing CPAP SD card usage monitor...");
    cpapMonitor = new CPAPMonitor();
    cpapMonitor->begin();
    LOG("CPAP monitor started - tracking SD card usage every 10 minutes");
#else
    LOG("CPAP monitor disabled (CS_SENSE hardware issue)");
    cpapMonitor = new CPAPMonitor();  // Use stub implementation
#endif
    
    // Initialize test web server for on-demand testing
    LOG("Initializing test web server...");
    
    // Create test web server with references to uploader's internal components
    testWebServer = new TestWebServer(&config, 
                                      uploader->getStateManager(),
                                      uploader->getScheduleManager(),
                                      &wifiManager,
                                      cpapMonitor);
    
    if (testWebServer->begin()) {
        LOG("Test web server started successfully");
        LOGF("Access web interface at: http://%s", wifiManager.getIPAddress().c_str());
        
#ifdef ENABLE_OTA_UPDATES
        // Set OTA manager reference in web server
        testWebServer->setOTAManager(&otaManager);
        LOG_DEBUG("OTA manager linked to web server");
#endif
        
        // Set TrafficMonitor reference in web server for SD Activity Monitor
        testWebServer->setTrafficMonitor(&trafficMonitor);
        LOG_DEBUG("TrafficMonitor linked to web server");
        
        // Set web server reference in uploader for responsive handling during uploads
        if (uploader) {
            uploader->setWebServer(testWebServer);
            LOG_DEBUG("Web server linked to uploader for responsive handling");
        }
    } else {
        LOG_ERROR("Failed to start test web server");
    }
#endif

    // Set initial FSM state based on upload mode
    if (uploader && uploader->getScheduleManager() && uploader->getScheduleManager()->isSmartMode()) {
        LOG("[FSM] Smart mode — starting in LISTENING (continuous loop)");
        transitionTo(UploadState::LISTENING);
    } else {
        LOG("[FSM] Scheduled mode — starting in IDLE");
        // IDLE is the correct initial state for scheduled mode
    }

    LOG("Setup complete!");
}

// ============================================================================
// FSM State Handlers
// ============================================================================

void handleIdle() {
    // IDLE is only used in scheduled mode.
    // Smart mode never enters IDLE — it uses the continuous loop:
    // LISTENING → ACQUIRING → UPLOADING → RELEASING → COOLDOWN → LISTENING
    
    unsigned long now = millis();
    if (now - lastIdleCheck < IDLE_CHECK_INTERVAL_MS) return;
    lastIdleCheck = now;
    
    if (!uploader || !uploader->getScheduleManager()) return;
    
    ScheduleManager* sm = uploader->getScheduleManager();
    
    // In scheduled mode: transition to LISTENING when the upload window opens,
    // even if all known files are marked complete. This ensures new DATALOG
    // folders written by the CPAP since the last upload are discovered during
    // the scan phase of the upload cycle.
    if (sm->isInUploadWindow() && !sm->isDayCompleted()) {
        LOG("[FSM] Upload window open — transitioning to LISTENING");
        transitionTo(UploadState::LISTENING);
    }
}

void handleListening() {
    // TrafficMonitor.update() is called in main loop before FSM dispatch
    uint32_t inactivityMs = (uint32_t)config.getInactivitySeconds() * 1000UL;
    
    if (trafficMonitor.isIdleFor(inactivityMs)) {
        LOGF("[FSM] %ds of bus silence confirmed", config.getInactivitySeconds());
        transitionTo(UploadState::ACQUIRING);
        return;
    }
    
    // In scheduled mode, check if the upload window has closed while we were listening
    // Smart mode never exits LISTENING to IDLE — it stays in the continuous loop
    ScheduleManager* sm = uploader->getScheduleManager();
    if (!sm->isSmartMode()) {
        if (!sm->isInUploadWindow() || sm->isDayCompleted()) {
            LOG("[FSM] Scheduled mode — window closed or day completed while listening");
            transitionTo(UploadState::IDLE);
        }
    }
}

void handleAcquiring() {
    if (sdManager.takeControl()) {
        LOG("[FSM] SD card control acquired");
        transitionTo(UploadState::UPLOADING);
    } else {
        LOG_WARN("[FSM] Failed to acquire SD card, releasing to cooldown");
        transitionTo(UploadState::RELEASING);
    }
}

// FreeRTOS task function — runs on Core 0 so main loop (Core 1) stays responsive
void uploadTaskFunction(void* pvParameters) {
    UploadTaskParams* params = (UploadTaskParams*)pvParameters;
    
    UploadResult result = params->uploader->uploadWithExclusiveAccess(
        params->sdManager, params->maxMinutes, params->filter);
    
    uploadTaskResult = result;
    uploadTaskComplete = true;
    
    delete params;
    vTaskDelete(NULL);  // Self-delete
}

void handleUploading() {
    if (!uploader) {
        transitionTo(UploadState::RELEASING);
        return;
    }
    
    if (!uploadTaskRunning) {
        // ── First call: determine filter and spawn upload task ──
        ScheduleManager* sm = uploader->getScheduleManager();
        DataFilter filter;
        bool canFresh = sm->canUploadFreshData();
        bool canOld = sm->canUploadOldData();
        
        if (canFresh && canOld) {
            filter = DataFilter::ALL_DATA;
        } else if (canFresh) {
            filter = DataFilter::FRESH_ONLY;
        } else if (canOld) {
            filter = DataFilter::OLD_ONLY;
        } else {
            LOG_WARN("[FSM] No data category eligible, releasing");
            transitionTo(UploadState::RELEASING);
            return;
        }
        
        // Disable web server handling inside upload task — main loop handles it
        // This prevents concurrent handleClient() calls from two cores
#ifdef ENABLE_TEST_WEBSERVER
        uploader->setWebServer(nullptr);
#endif
        
        UploadTaskParams* params = new UploadTaskParams{
            uploader, &sdManager, config.getExclusiveAccessMinutes(), filter
        };
        
        uploadTaskComplete = false;
        uploadTaskRunning = true;
        
        // Pin to Core 0 (protocol core) — keeps Core 1 free for main loop + web server
        // Stack: 16KB — TLS buffers are on heap, task only needs stack for locals
        BaseType_t rc = xTaskCreatePinnedToCore(
            uploadTaskFunction,  // Task function
            "upload",            // Name
            16384,               // Stack size (16KB for task locals + SD I/O)
            params,              // Parameters
            1,                   // Priority (same as loop task)
            &uploadTaskHandle,   // Handle
            0                    // Pin to Core 0
        );
        
        if (rc != pdPASS) {
            LOG_ERROR("[FSM] Failed to create upload task — falling back to blocking upload");
            uploadTaskRunning = false;
            delete params;
#ifdef ENABLE_TEST_WEBSERVER
            uploader->setWebServer(testWebServer);
#endif
            // Fallback: run synchronously (old behavior)
            int maxMinutes = config.getExclusiveAccessMinutes();
            UploadResult result = uploader->uploadWithExclusiveAccess(&sdManager, maxMinutes, filter);
            switch (result) {
                case UploadResult::COMPLETE: transitionTo(UploadState::COMPLETE); break;
                case UploadResult::TIMEOUT:  uploadCycleHadTimeout = true; transitionTo(UploadState::RELEASING); break;
                case UploadResult::ERROR:    LOG_ERROR("[FSM] Upload error"); transitionTo(UploadState::RELEASING); break;
            }
        } else {
            LOG("[FSM] Upload task started on Core 0 (non-blocking)");
        }
    } else if (uploadTaskComplete) {
        // ── Task finished: read result and transition ──
        uploadTaskRunning = false;
        uploadTaskHandle = nullptr;
        
        // Restore web server handling in uploader
#ifdef ENABLE_TEST_WEBSERVER
        uploader->setWebServer(testWebServer);
#endif
        
        UploadResult result = (UploadResult)uploadTaskResult;
        
        switch (result) {
            case UploadResult::COMPLETE:
                transitionTo(UploadState::COMPLETE);
                break;
            case UploadResult::TIMEOUT:
                uploadCycleHadTimeout = true;
                transitionTo(UploadState::RELEASING);
                break;
            case UploadResult::ERROR:
                LOG_ERROR("[FSM] Upload error occurred");
                transitionTo(UploadState::RELEASING);
                break;
        }
    }
    // else: task still running — return immediately (non-blocking)
}

void handleReleasing() {
    if (sdManager.hasControl()) {
        sdManager.releaseControl();
    }
    
    // If monitoring was requested during upload, go to MONITORING instead of COOLDOWN
    if (monitoringRequested) {
        monitoringRequested = false;
        trafficMonitor.resetStatistics();
        LOG("[FSM] Monitoring requested during upload — entering MONITORING after release");
        transitionTo(UploadState::MONITORING);
        return;
    }
    
    cooldownStartedAt = millis();
    transitionTo(UploadState::COOLDOWN);
}

void handleCooldown() {
    unsigned long cooldownMs = (unsigned long)config.getCooldownMinutes() * 60UL * 1000UL;
    
    if (millis() - cooldownStartedAt < cooldownMs) {
        return;  // Non-blocking wait
    }
    
    LOGF("[FSM] Cooldown complete (%d minutes)", config.getCooldownMinutes());
    uploadCycleHadTimeout = false;
    
    ScheduleManager* sm = uploader->getScheduleManager();
    
    if (sm->isSmartMode()) {
        // Smart mode: ALWAYS return to LISTENING (continuous loop)
        trafficMonitor.resetIdleTracking();
        LOG("[FSM] Smart mode — returning to LISTENING (continuous loop)");
        transitionTo(UploadState::LISTENING);
    } else {
        // Scheduled mode: return to LISTENING if still in window and day not done
        if (sm->isInUploadWindow() && !sm->isDayCompleted()) {
            trafficMonitor.resetIdleTracking();
            transitionTo(UploadState::LISTENING);
        } else {
            LOG("[FSM] Scheduled mode — window closed or day completed");
            transitionTo(UploadState::IDLE);
        }
    }
}

void handleComplete() {
    ScheduleManager* sm = uploader->getScheduleManager();
    
    if (sm->isSmartMode()) {
        // Smart mode: release → cooldown → listening (continuous loop)
        // Next cycle will scan SD card and discover any new data naturally
        LOG("[FSM] Smart mode complete — continuing loop via RELEASING → COOLDOWN → LISTENING");
        transitionTo(UploadState::RELEASING);
    } else {
        // Scheduled mode: done for today
        sm->markDayCompleted();
        LOG("[FSM] Scheduled mode — day marked as completed");
        transitionTo(UploadState::IDLE);
    }
}

void handleMonitoring() {
    // TrafficMonitor.update() runs as normal (called in main loop)
    // No upload activity, no SD card access
    // Web endpoint /api/sd-activity serves live PCNT sample data
    
    if (stopMonitoringRequested) {
        stopMonitoringRequested = false;
        LOG("[FSM] Monitoring stopped by user");
        ScheduleManager* sm = uploader ? uploader->getScheduleManager() : nullptr;
        if (sm && sm->isSmartMode()) {
            trafficMonitor.resetIdleTracking();
            transitionTo(UploadState::LISTENING);
        } else {
            transitionTo(UploadState::IDLE);
        }
    }
}

// ============================================================================
// Loop Function
// ============================================================================
void loop() {
    // ── Always-on tasks ──
    
    // Periodic SD card log dump (every 10 seconds when enabled)
    // Skip when upload task is running — SD card is in use on another core
    if (config.getLogToSdCard() && !uploadTaskRunning) {
        unsigned long currentTime = millis();
        if (currentTime - lastLogDumpTime >= LOG_DUMP_INTERVAL_MS) {
            if (Logger::getInstance().dumpLogsToSDCardPeriodic(&sdManager)) {
                LOG_DEBUG("Periodic log dump to SD card completed");
            }
            lastLogDumpTime = currentTime;
        }
    }
    
    // Update traffic monitor (non-blocking ~100ms sample)
    trafficMonitor.update();

#ifdef ENABLE_TEST_WEBSERVER
    // Update CPAP monitor
#ifdef ENABLE_CPAP_MONITOR
    if (cpapMonitor) {
        cpapMonitor->update();
    }
#endif
    
    // Handle web server requests
    if (testWebServer) {
        testWebServer->handleClient();
    }
    
    // ── Web trigger handlers (operate independently of FSM) ──
    
    // Check for state reset trigger (blocked while upload task is running)
    if (g_resetStateFlag && !uploadTaskRunning) {
        LOG("=== State Reset Triggered via Web Interface ===");
        g_resetStateFlag = false;
        
        if (sdManager.takeControl()) {
            LOG("Resetting upload state...");

            bool removedAnyState = false;
            if (sdManager.getFS().remove("/.upload_state.v2")) {
                LOG("Upload state snapshot deleted successfully");
                removedAnyState = true;
            }

            if (sdManager.getFS().remove("/.upload_state.v2.log")) {
                LOG("Upload state journal deleted successfully");
                removedAnyState = true;
            }

            if (!removedAnyState) {
                LOG_WARN("Failed to delete upload state files (may not exist)");
            }
            
            if (uploader) {
                delete uploader;
                uploader = new FileUploader(&config, &wifiManager);
                if (uploader->begin(sdManager.getFS())) {
                    LOG("Uploader reinitialized with fresh state");
                    
                    if (testWebServer) {
                        testWebServer->updateManagers(uploader->getStateManager(),
                                                     uploader->getScheduleManager());
                        uploader->setWebServer(testWebServer);
                    }
                } else {
                    LOG_ERROR("Failed to reinitialize uploader");
                }
            }
            
            sdManager.releaseControl();
            ScheduleManager* sm = uploader ? uploader->getScheduleManager() : nullptr;
            if (sm && sm->isSmartMode()) {
                trafficMonitor.resetIdleTracking();
                transitionTo(UploadState::LISTENING);
            } else {
                transitionTo(UploadState::IDLE);
            }
            LOG("State reset complete");
        } else {
            LOG_ERROR("Cannot reset state - SD card in use");
        }
    }
    
    // Check for upload trigger (force immediate upload — skip inactivity check)
    // Blocked while upload task is running — already uploading
    if (g_triggerUploadFlag && !uploadTaskRunning) {
        LOG("=== Upload Triggered via Web Interface ===");
        g_triggerUploadFlag = false;
        uploadCycleHadTimeout = false;
        transitionTo(UploadState::ACQUIRING);
    }
    
    // Check for monitoring triggers
    if (g_monitorActivityFlag) {
        g_monitorActivityFlag = false;
        monitoringRequested = true;
    }
    if (g_stopMonitorFlag) {
        g_stopMonitorFlag = false;
        stopMonitoringRequested = true;
    }
#endif
    
    // ── WiFi reconnection (non-blocking with 30 second retry interval) ──
    if (!wifiManager.isConnected()) {
        unsigned long currentTime = millis();
        if (currentTime - lastWifiReconnectAttempt >= 30000) {
            LOG_WARN("WiFi disconnected, attempting to reconnect...");
            
            if (!config.valid() || config.getWifiSSID().isEmpty()) {
                LOG_ERROR("Cannot reconnect to WiFi: Invalid configuration");
                lastWifiReconnectAttempt = currentTime;
                return;
            }
            
            if (!wifiManager.connectStation(config.getWifiSSID(), config.getWifiPassword())) {
                LOG_ERROR("Failed to reconnect to WiFi");
                lastWifiReconnectAttempt = currentTime;
                return;
            }
            LOG_DEBUG("WiFi reconnected successfully");
            lastNtpSyncAttempt = 0;
            lastWifiReconnectAttempt = 0;
        }
        return;  // Skip FSM while WiFi is down
    }

    // ── NTP sync retry ──
    if (uploader) {
        unsigned long currentTime = millis();
        if (currentTime - lastNtpSyncAttempt >= NTP_RETRY_INTERVAL_MS) {
            LOG_DEBUG("Periodic NTP synchronization check...");
            lastNtpSyncAttempt = currentTime;
        }
    }
    
    // ── Monitoring request handling (can interrupt most states) ──
    if (monitoringRequested) {
        if (currentState != UploadState::UPLOADING) {
            monitoringRequested = false;
            if (currentState == UploadState::ACQUIRING && sdManager.hasControl()) {
                sdManager.releaseControl();
            }
            trafficMonitor.resetStatistics();
            transitionTo(UploadState::MONITORING);
        }
        // If UPLOADING, leave flag set — handleReleasing() will redirect to MONITORING
        // after upload finishes current cycle + mandatory root/SETTINGS files
    }

    // ── FSM dispatch ──
    switch (currentState) {
        case UploadState::IDLE:       handleIdle();       break;
        case UploadState::LISTENING:  handleListening();  break;
        case UploadState::ACQUIRING:  handleAcquiring();  break;
        case UploadState::UPLOADING:  handleUploading();  break;
        case UploadState::RELEASING:  handleReleasing();  break;
        case UploadState::COOLDOWN:   handleCooldown();   break;
        case UploadState::COMPLETE:   handleComplete();   break;
        case UploadState::MONITORING: handleMonitoring(); break;
    }
}
