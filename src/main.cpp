#include <Arduino.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_bt.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <esp_freertos_hooks.h>
#include <Preferences.h>
#include <LittleFS.h>

#include "Config.h"
#include "SDCardManager.h"
#include "WiFiManager.h"
#include "FileUploader.h"
#include "Logger.h"
#include "pins_config.h"
#include "version.h"

#include "TrafficMonitor.h"
#include "UlpMonitor.h"
#include "UploadFSM.h"
#include <ESPmDNS.h>

// True when esp_restart() was the reset cause (ESP_RST_SW).
// Any programmatic restart means the CPAP machine was already idle and
// voltages are stable, so cold-boot delays (stabilization, Smart Wait,
// NTP settle) can be skipped.  Set once in setup() from esp_reset_reason().
bool g_heapRecoveryBoot = false;

// ── POWER: Brownout-recovery degraded boot ──
// When the previous reset was ESP_RST_BROWNOUT, boot in a reduced-but-reachable
// mode: no mDNS, no SSE, lowest TX power, MAX power save. Automatically clears
// after one successful upload cycle or on next clean boot.
bool g_brownoutRecoveryBoot = false;

// ── POWER: Timed mDNS ──
// mDNS runs for 60 seconds after boot/reconnect then stops to eliminate
// continuous multicast group membership and associated radio wakes.
unsigned long g_mdnsStartTime = 0;
const unsigned long MDNS_ACTIVE_DURATION_MS = 60000;  // 60 seconds
bool g_mdnsTimedOut = false;

#ifdef ENABLE_OTA_UPDATES
#include "OTAManager.h"
#endif

#ifdef ENABLE_WEBSERVER
#include "CpapWebServer.h"
#endif

// ============================================================================
// Global Objects
// ============================================================================
Config config;
SDCardManager sdManager;
WiFiManager wifiManager;
FileUploader* uploader = nullptr;
TrafficMonitor trafficMonitor;
UlpMonitor ulpMonitor;

#ifdef ENABLE_OTA_UPDATES
OTAManager otaManager;
#endif

#ifdef ENABLE_WEBSERVER
CpapWebServer* webServer = nullptr;
#endif

// ============================================================================
// Upload FSM State
// ============================================================================
UploadState currentState = UploadState::IDLE;
unsigned long stateEnteredAt = 0;
unsigned long cooldownStartedAt = 0;
bool uploadCycleHadTimeout = false;
bool g_nothingToUpload = false;  // Set when pre-flight finds no work — skip reboot, go to cooldown

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

// Software watchdog: upload task updates this heartbeat; main loop kills task if stale
volatile unsigned long g_uploadHeartbeat = 0;
const unsigned long UPLOAD_WATCHDOG_TIMEOUT_MS = 120000;  // 2 minutes

// Power management lock — held in active states to prevent auto light-sleep.
// Released in IDLE and COOLDOWN so the CPU can enter light-sleep between DTIM intervals.
esp_pm_lock_handle_t g_pmLock = nullptr;

// ── CPU load measurement via FreeRTOS idle hooks ──
// Idle hooks increment counters every time the idle task runs on each core.
// The diagnostics endpoint samples these counters to compute load %.
volatile uint32_t g_idleCount0 = 0, g_idleCount1 = 0;
uint32_t g_cpuLoad0 = 0, g_cpuLoad1 = 0;  // 0-100 percent, updated every 2s
static bool _idleHook0() { g_idleCount0++; return true; }
static bool _idleHook1() { g_idleCount1++; return true; }

// ── Reboot reason helper ──
// Stores a human-readable reason in NVS before esp_restart() so the next
// boot can log it clearly.  The reason is read and cleared early in setup().
static void setRebootReason(const char* reason) {
    Preferences p;
    p.begin("cpap_flags", false);
    p.putString("reboot_why", reason);
    p.end();
}

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

// Persistent log flush timing
unsigned long lastLogFlushTime = 0;
const unsigned long LOG_FLUSH_INTERVAL_MS = 10 * 1000;  // 10 seconds

// Runtime debug mode: set from config DEBUG=true after config load.
// Gates [res fh= ma= fd=] heap suffix on all log lines and verbose pre-flight output.
bool g_debugMode = false;

#ifdef ENABLE_WEBSERVER
// External trigger flags (defined in WebServer.cpp)
extern volatile bool g_triggerUploadFlag;
extern volatile bool g_resetStateFlag;

// Monitoring trigger flags (defined in WebServer.cpp)
extern volatile bool g_monitorActivityFlag;
extern volatile bool g_stopMonitorFlag;
extern volatile bool g_abortUploadFlag;
#endif

// ============================================================================
// FSM Helper
// ============================================================================
void transitionTo(UploadState newState) {
    LOGF("[FSM] %s -> %s", getStateName(currentState), getStateName(newState));
    
    // ── POWER: Manage PM lock for auto light-sleep ──
    // Hold the lock in active states (CPU must stay awake for PCNT, SD I/O, network).
    // Release in IDLE and COOLDOWN so auto light-sleep can engage between DTIM intervals.
    if (g_pmLock) {
        bool newStateIsLowPower = (newState == UploadState::IDLE || newState == UploadState::COOLDOWN);
        bool oldStateIsLowPower = (currentState == UploadState::IDLE || currentState == UploadState::COOLDOWN);
        
        if (newStateIsLowPower && !oldStateIsLowPower) {
            esp_pm_lock_release(g_pmLock);
            LOG_DEBUG("[POWER] PM lock released — light-sleep enabled");
        } else if (!newStateIsLowPower && oldStateIsLowPower) {
            esp_pm_lock_acquire(g_pmLock);
            LOG_DEBUG("[POWER] PM lock acquired — light-sleep inhibited");
        }
    }
    
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
    // ── POWER: Immediate CPU throttle ──
    // Reduce from 240 MHz to 80 MHz before anything else runs.
    // Saves ~30-40 mA during the entire 20+ second boot sequence.
    // 80 MHz is the minimum for WiFi and sufficient for all boot I/O.
    setCpuFrequencyMhz(80);
    
    // ── POWER: Release Bluetooth memory ──
    // Firmware is WiFi-only. Release BT controller memory (~28 KB DRAM).
    // Note: CONFIG_BT_ENABLED=n in sdkconfig does NOT strip BT from the Arduino
    // framework's precompiled libraries. This runtime call is our only effective
    // mechanism — it frees the BTDM controller's reserved DRAM regions.
    uint32_t btHeapBefore = ESP.getFreeHeap();
    uint32_t btMaxAllocBefore = ESP.getMaxAllocHeap();
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    uint32_t btHeapAfter = ESP.getFreeHeap();
    uint32_t btMaxAllocAfter = ESP.getMaxAllocHeap();
    
    // Initialize serial port
    Serial.begin(115200);
    
    // CRITICAL: Immediately release SD card control to CPAP machine
    // This must happen before any delays to prevent CPAP machine errors
    // Initialize control pins
    pinMode(CS_SENSE, INPUT);
    pinMode(SD_SWITCH_PIN, OUTPUT);
    digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
    
    delay(1000);
    LOG("\n\n=== CPAP Data Auto-Uploader ===");
    LOGF("Firmware Version: %s", FIRMWARE_VERSION);
    LOGF("BT memory release: free %u->%u (+%u), max_alloc %u->%u",
         btHeapBefore, btHeapAfter, btHeapAfter - btHeapBefore,
         btMaxAllocBefore, btMaxAllocAfter);
    LOGF("Build Info: %s", BUILD_INFO);
    LOGF("Build Time: %s", FIRMWARE_BUILD_TIME);
    
    // Log reset reason for power/stability diagnostics
    esp_reset_reason_t resetReason = esp_reset_reason();
    LOG_INFOF("Reset reason: %s", getResetReasonString(resetReason));
    
    // Register CPU idle hooks for load measurement (before any blocking waits)
    esp_register_freertos_idle_hook_for_cpu(_idleHook0, 0);
    esp_register_freertos_idle_hook_for_cpu(_idleHook1, 1);

    // Check for power-related issues
    if (resetReason == ESP_RST_BROWNOUT) {
        LOG_ERROR("WARNING: System reset due to brown-out (insufficient power supply), this could be caused by:");
        LOG_ERROR(" - the CPAP was disconnected from the power supply");
        LOG_ERROR(" - the card was removed");
        LOG_ERROR(" - the CPAP machine cannot provide enough power");
        // ── POWER: Activate brownout-recovery degraded mode ──
        // Boot in reduced-but-reachable mode: no mDNS, no SSE, lowest TX power,
        // MAX power save. This reduces current draw on hardware that just proved
        // it cannot sustain normal operation.
        g_brownoutRecoveryBoot = true;
        LOG_WARN("[POWER] Brownout-recovery mode ACTIVE — degraded boot to reduce power draw");
    } else if (resetReason == ESP_RST_PANIC) {
        LOG_WARN("System reset due to software panic - check for stability issues");
    } else if (resetReason == ESP_RST_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_INT_WDT) {
        LOG_WARN("System reset due to watchdog timeout - possible hang or power issue");
    }

    // Initialize LittleFS for state and internal logs
    if (!LittleFS.begin(true)) {
        LOG_ERROR("Failed to mount LittleFS - state and logs cannot be saved!");
    } else {
        LOG("LittleFS mounted successfully");
    }

    // Initialize SD card control
    if (!sdManager.begin()) {
        LOG("Failed to initialize SD card manager");
        return;
    }
    
    // Initialize TrafficMonitor (PCNT-based bus activity detection on CS_SENSE pin)
    trafficMonitor.begin(CS_SENSE);
    
    // Initialize ULP coprocessor monitor for ultra-low-power CS_SENSE detection.
    // ULP runs at 10 Hz sampling GPIO 33 via RTC_GPIO8 while main CPU can sleep.
    // Activity is checked in the main loop; ULP is stopped before SD card operations.
    if (ulpMonitor.begin()) {
        LOG_DEBUG("ULP coprocessor CS_SENSE monitor initialized");
    } else {
        LOG_WARN("ULP monitor initialization failed — using PCNT only");
    }

    // Determine boot type: software reset (ESP_RST_SW) = soft-reboot / FastBoot.
    // Cold boots (power-on, brownout, watchdog) use distinct reason codes.
    g_heapRecoveryBoot = (esp_reset_reason() == ESP_RST_SW);
    bool fastBoot = g_heapRecoveryBoot;

    // Smart Wait constants — same values for both cold and soft-reboot.
    // 5 s of continuous SD bus silence required before taking control.
    // The previous 45s hostile takeover timeout has been removed to prevent filesystem corruption.
    const unsigned long SMART_WAIT_REQUIRED_MS = 5000;

    auto runSmartWait = [&]() {
        LOG("Checking for CPAP SD card activity (Smart Wait)...");
        while (true) {
            trafficMonitor.update();
            delay(10);
            if (trafficMonitor.isIdleFor(SMART_WAIT_REQUIRED_MS)) {
                LOGF("Smart Wait: %lums of bus silence — CPAP is idle", SMART_WAIT_REQUIRED_MS);
                break;
            }
        }
    };

    if (fastBoot) {
        // Soft-reboot: voltages already stable, skip 15 s electrical stabilization.
        // Smart Wait still runs — CPAP may have been mid-access when the reboot
        // was triggered and we must wait for it to finish before touching the SD card.
        LOG("[FastBoot] Software reset — skipping 15s electrical stabilization");
        runSmartWait();
    } else {
        // Cold boot: wait for power-rail stabilization and CPAP boot sequence to settle,
        // then wait for SD bus silence before attempting to take SD card control.
        // 8 seconds is sufficient for voltage rails and CPAP initialization.
        LOG("Waiting 8s for electrical stabilization...");
        delay(8000);
        runSmartWait();
    }
    
    LOG("Boot delay complete, attempting SD card access...");

    // Take control of SD card
    LOG("Waiting to access SD card...");
    while (!sdManager.takeControl()) {
        delay(1000);
    }

    // Check NVS flags from previous boot
    {
        Preferences resetPrefs;
        resetPrefs.begin("cpap_flags", false);
        
        // Display and clear stored reboot reason (set by setRebootReason() before esp_restart)
        String rebootWhy = resetPrefs.getString("reboot_why", "");
        if (rebootWhy.length() > 0) {
            LOGF("[BOOT] Reboot reason: %s", rebootWhy.c_str());
            resetPrefs.remove("reboot_why");
        }

        // Check if software watchdog killed the upload task last boot
        bool watchdogKill = resetPrefs.getBool("watchdog_kill", false);
        if (watchdogKill) {
            LOG_WARN("=== Previous boot: upload task was killed by software watchdog (hung >2 min) ===");
            resetPrefs.putBool("watchdog_kill", false);
        }
        
        bool resetPending = resetPrefs.getBool("reset_state", false);
        if (resetPending) {
            LOG("=== Completing deferred state reset (flag set before reboot) ===");
            resetPrefs.putBool("reset_state", false);
            resetPrefs.end();
            
            // Delete all known state/summary paths from internal LittleFS only.
            // Paths are relative to the LittleFS mount — do NOT include /littlefs/ prefix.
            static const char* STATE_FILES[] = {
                "/.upload_state.v2.smb",
                "/.upload_state.v2.smb.log",
                "/.upload_state.v2.cloud",
                "/.upload_state.v2.cloud.log",
                "/.backend_summary.smb",
                "/.backend_summary.cloud",
                "/.upload_state.v2",      // legacy: pre-split single-manager path
                "/.upload_state.v2.log",
            };
            bool removedAny = false;
            for (const char* path : STATE_FILES) {
                if (LittleFS.remove(path)) {
                    LOGF("Deleted state file: %s", path);
                    removedAny = true;
                }
            }
            if (!removedAny) {
                LOG_WARN("No state files found (may already be clean)");
            }
            LOG("State reset complete — continuing with fresh start");
        } else {
            resetPrefs.end();
        }
    }

    // Read config file from SD card
    LOG("Loading configuration...");
    if (!config.loadFromSD(sdManager.getFS())) {
        LOG_ERROR("Failed to load configuration - cannot continue");
        LOG_ERROR("Please check config.txt file on SD card");
        
        // ── EMERGENCY BOOT ERROR DUMP ──
        // Without config, we have no WiFi and no Web UI. We must dump the reason
        // directly to the SD card so the user can read it on their PC.
        LOG_ERROR("FATAL ERROR: System halted due to config failure. Please check config.txt.");
        Logger::getInstance().dumpToSD(sdManager.getFS(), "/uploader_error.txt", "Config load failure");
        
        sdManager.releaseControl();
        
        // Save logs to internal storage for configuration failures
        bool dumped = Logger::getInstance().dumpSavedLogs("config_load_failed");
        if (!dumped) {
            LOG_WARN("Failed to persist logs (config_load_failed)");
        }

        // Fail-safe: always force SD switch back to CPAP before aborting setup
        digitalWrite(SD_SWITCH_PIN, SD_SWITCH_CPAP_VALUE);
        
        return;
    }

    LOG("Configuration loaded successfully");
    g_debugMode = config.getDebugMode();
    if (g_debugMode) {
        LOG_WARN("DEBUG mode enabled — verbose pre-flight logs and heap stats active");
    }
    LOG_DEBUGF("WiFi SSID: %s", config.getWifiSSID().c_str());
    LOG_DEBUGF("Endpoint: %s", config.getEndpoint().c_str());

    // Check if a previous boot left an emergency error log on the SD card
    Logger::getInstance().checkPreviousBootError(sdManager.getFS());

    // Always enable persistent logging with multi-file rotation (syslog.0-3.txt)
    // First call also migrates legacy log files (syslog.A/B, last_reboot_log, etc.)
    Logger::getInstance().enableLogSaving(true, &LittleFS);
    // Flush immediately so boot logs (reset reason, Smart Wait, config load)
    // are captured to NAND before upload activity overwrites the 8KB buffer.
    Logger::getInstance().dumpSavedLogsPeriodic(nullptr);

    // Release SD card back to CPAP machine
    sdManager.releaseControl();

    // Apply power management settings from config
    LOG("Applying power management settings...");
    
    // Update CPU frequency from config (boot default is 80 MHz, set in early setup)
    int targetCpuMhz = config.getCpuSpeedMhz();
    if (targetCpuMhz != getCpuFrequencyMhz()) {
        setCpuFrequencyMhz(targetCpuMhz);
    }
    LOGF("CPU frequency: %dMHz", getCpuFrequencyMhz());

    // Setup WiFi event handlers for logging
    wifiManager.setupEventHandlers();

    // Apply TX power BEFORE WiFi.begin() to prevent full-power spikes during association
    wifiManager.applyTxPowerEarly(config.getWifiTxPower());

    // Initialize WiFi in station mode
    if (!wifiManager.connectStation(config.getWifiSSID(), config.getWifiPassword())) {
        LOG("Failed to connect to WiFi");
        // Note: WiFiManager already persists logs on connection failures
        
        // ── EMERGENCY BOOT ERROR DUMP ──
        // If we can't connect to WiFi on boot, the user can't access the Web UI to see why.
        // We re-take the SD card just to dump the log buffer.
        if (sdManager.takeControl()) {
            LOG_ERROR("FATAL ERROR: System halted due to WiFi connection failure.");
            Logger::getInstance().dumpToSD(sdManager.getFS(), "/uploader_error.txt", "WiFi connection failure");
            sdManager.releaseControl();
        }
        
        return;
    }
    
    // ── POWER: mDNS and WiFi power settings (brownout-aware) ──
    if (g_brownoutRecoveryBoot) {
        // Brownout-recovery: skip mDNS entirely, force lowest TX power + MAX power save
        LOG_WARN("[POWER] Brownout-recovery: skipping mDNS, forcing lowest TX power + MAX power save");
        wifiManager.applyPowerSettings(WifiTxPower::POWER_LOW, WifiPowerSaving::SAVE_MAX);
    } else {
        // Normal boot: start timed mDNS (60s then stop) and apply configured power settings
        wifiManager.startMDNS(config.getHostname());
        g_mdnsStartTime = millis();
        g_mdnsTimedOut = false;
        wifiManager.applyPowerSettings(config.getWifiTxPower(), config.getWifiPowerSaving());
    }
    LOG("WiFi power management settings applied");
    
    // ── POWER: Configure Dynamic Frequency Scaling (DFS) + Auto Light-Sleep ──
    // With CONFIG_PM_ENABLE=y in sdkconfig, the CPU can automatically scale
    // between min and max frequency when tasks are idle. The WiFi driver holds
    // a PM lock during active operations, ensuring full speed when needed.
    //
    // When CPU_SPEED_MHZ=80 (default), max==min==80 → DFS is effectively
    // disabled, eliminating PLL relock transients that stress the power supply.
    // Users on non-constrained hardware can set CPU_SPEED_MHZ=160 to re-enable DFS.
    //
    // Auto light-sleep allows the CPU to sleep between WiFi DTIM intervals,
    // reducing idle current from ~20 mA to ~2-3 mA. A PM lock held in active
    // FSM states prevents sleep during PCNT counting, SD I/O, and uploads.
    esp_pm_config_esp32_t pm_config = {
        .max_freq_mhz = targetCpuMhz,  // Respects CPU_SPEED_MHZ config (default 80)
        .min_freq_mhz = 80,            // Floor at 80 MHz (WiFi PHY minimum)
        .light_sleep_enable = true      // Auto light-sleep in IDLE/COOLDOWN states
    };
    esp_err_t pm_err = esp_pm_configure(&pm_config);
    if (pm_err == ESP_OK) {
        if (targetCpuMhz == 80) {
            LOG("Power management: CPU locked at 80MHz (no DFS), auto light-sleep enabled");
        } else {
            LOGF("Power management: DFS enabled (80-%dMHz), auto light-sleep enabled", targetCpuMhz);
        }
    } else {
        LOGF("PM configuration failed (err=%d), CPU stays at %dMHz", pm_err, getCpuFrequencyMhz());
    }
    
    // ── POWER: Configure GPIO wakeup for auto light-sleep ──
    // GPIO 33 (CS_SENSE) is an RTC GPIO that detects CPAP SD card bus activity.
    // When the CPU is in light-sleep, a CS_SENSE edge wakes it immediately.
    gpio_wakeup_enable((gpio_num_t)CS_SENSE, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    LOG_DEBUG("GPIO wakeup configured on CS_SENSE (GPIO 33)");
    
    // ── POWER: Create PM lock for active FSM states ──
    // Acquired in LISTENING/ACQUIRING/UPLOADING/RELEASING/MONITORING/COMPLETE
    // to prevent light-sleep while PCNT counting, SD I/O, or network I/O is active.
    // Released in IDLE and COOLDOWN to allow light-sleep.
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "fsm_active", &g_pmLock) == ESP_OK) {
        // Start with lock acquired — initial state (LISTENING or IDLE) will
        // release it via transitionTo() if entering a low-power state.
        esp_pm_lock_acquire(g_pmLock);
        LOG_DEBUG("PM lock created and acquired for active states");
    } else {
        LOG_WARN("Failed to create PM lock — light-sleep management unavailable");
        g_pmLock = nullptr;
    }

    // Initialize uploader (no SD card needed — state is on LittleFS)
    uploader = new FileUploader(&config, &wifiManager);
    LOG("Initializing uploader...");
    if (!uploader->begin()) {
        LOG_ERROR("Failed to initialize uploader");
        return;
    }
    g_heapRecoveryBoot = false;  // consumed — only skip delays on this one boot
    LOG("Uploader initialized successfully");
    
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

#ifdef ENABLE_WEBSERVER
    // Initialize web server
    LOG("Initializing web server...");
    
    // Create web server with references to uploader's internal components
    webServer = new CpapWebServer(&config, 
                                      uploader->getStateManager(),
                                      uploader->getScheduleManager(),
                                      &wifiManager);
    
    if (webServer->begin()) {
        LOG("Web server started successfully");
        LOGF("Access web interface at: http://%s", wifiManager.getIPAddress().c_str());
        
#ifdef ENABLE_OTA_UPDATES
        // Set OTA manager reference in web server
        webServer->setOTAManager(&otaManager);
        LOG_DEBUG("OTA manager linked to web server");
#endif
        
        // Set TrafficMonitor reference in web server for SD Activity Monitor
        webServer->setTrafficMonitor(&trafficMonitor);
        LOG_DEBUG("TrafficMonitor linked to web server");

        webServer->setSdManager(&sdManager);
        LOG_DEBUG("SDCardManager linked to web server for config editor");

        // Give web server access to the SMB state manager so updateStatusSnapshot()
        // can show folder counts from the active backend (SMB pass vs cloud pass).
        webServer->setSmbStateManager(uploader->getSmbStateManager());
        
        // Set web server reference in uploader for responsive handling during uploads
        if (uploader) {
            uploader->setWebServer(webServer);
            LOG_DEBUG("Web server linked to uploader for responsive handling");
        }

        // Build static config snapshot once — served from g_webConfigBuf with zero heap alloc.
        webServer->initConfigSnapshot();
        LOG_DEBUG("[WebStatus] Config snapshot built");
    } else {
        LOG_ERROR("Failed to start web server");
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

    // Smart mode logic
    if (config.isSmartMode()) {
        if (trafficMonitor.isIdleFor(inactivityMs)) {
            LOGF("[FSM] %ds of bus silence confirmed", config.getInactivitySeconds());
            
            // No network pre-connect here — backends connect on-demand when actual work is confirmed
            // (SMB connects lazily in FileUploader, Cloud connects after preflight)
            transitionTo(UploadState::ACQUIRING);
            return;
        }
    }
    
    // In scheduled mode, check if the upload window has closed while we were listening
    // Smart mode never exits LISTENING to IDLE — it stays in the continuous loop
    ScheduleManager* sm = uploader->getScheduleManager();
    if (!sm->isSmartMode()) {
        if (!sm->isInUploadWindow() || sm->isDayCompleted()) {
            LOG("[FSM] Scheduled mode — window closed or day completed while listening");
            transitionTo(UploadState::IDLE);
        } else if (trafficMonitor.isIdleFor(inactivityMs)) {
            LOGF("[FSM] Scheduled mode — %ds of bus silence confirmed", config.getInactivitySeconds());
            
            // No network pre-connect here — backends connect on-demand when actual work is confirmed
            transitionTo(UploadState::ACQUIRING);
        }
    }
}

void handleAcquiring() {
    // The upload task now owns the full lifecycle:
    //   SD mount → pre-flight scan → phased upload → SD release
    // Task stack is allocated at high max_alloc (~73KB).  TLS connects
    // on-demand only when pre-flight confirms cloud work (asymmetric
    // mbedTLS buffers fit at post-SD-mount heap levels).
    transitionTo(UploadState::UPLOADING);
}

// FreeRTOS task function — runs on Core 0 so main loop (Core 1) stays responsive
// Owns the full session lifecycle: SD mount → pre-flight → upload → SD release
void uploadTaskFunction(void* pvParameters) {
    UploadTaskParams* params = (UploadTaskParams*)pvParameters;
    
    g_uploadHeartbeat = millis();
    
    // NOTE: TLS pre-warm was removed here.  With custom asymmetric mbedTLS
    // (16 KB IN / 4 KB OUT), the largest single TLS allocation is ~16.7 KB
    // which fits comfortably at ma≈38900 after SD mount.  The cloud phase's
    // begin() handles on-demand TLS connection, saving ~11 s and ~28 KB of
    // heap when pre-flight finds no cloud work.

    // Stop ULP monitor before SD card operations — ULP and SDMMC share RTC GPIO
    extern UlpMonitor ulpMonitor;
    if (ulpMonitor.isRunning()) {
        ulpMonitor.stop();
    }

    // Mount SD card
    if (!params->sdManager->takeControl()) {
        LOG_ERROR("[Upload] Failed to acquire SD card control");
        uploadTaskResult = UploadResult::ERROR;
        uploadTaskComplete = true;
        delete params;
        vTaskDelete(NULL);
        return;
    }
    LOG("[FSM] SD card control acquired");

    // Phase 1+2: Run phased upload (CLOUD first, then SMB)
    UploadResult result = params->uploader->runFullSession(
        params->sdManager, params->maxMinutes, params->filter);
    
    // Phase 3: Release SD card
    if (params->sdManager->hasControl()) {
        params->sdManager->releaseControl();
    }

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

        LOGF("[FSM] Heap before upload task: fh=%u ma=%u",
             ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        g_abortUploadFlag = false;  // Clear any stale abort request

        // Disable web server handling inside upload task — main loop handles it
        // This prevents concurrent handleClient() calls from two cores
#ifdef ENABLE_WEBSERVER
        uploader->setWebServer(nullptr);
#endif

        UploadTaskParams* params = new UploadTaskParams{
            uploader, &sdManager, config.getExclusiveAccessMinutes(), filter
        };
        
        uploadTaskComplete = false;
        uploadTaskRunning = true;
        
        // Unsubscribe IDLE0 from task watchdog — upload task will monopolize Core 0
        // during TLS handshake (5-15s of CPU-intensive crypto), starving IDLE0.
        // Without this, IDLE0 can't feed the WDT and the system reboots.
        esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0));
        
        // Pin to Core 0 (protocol core) — keeps Core 1 free for main loop + web server
        // Stack: 12KB — TLS buffers are on heap, task only needs stack for locals.
        // Created BEFORE TLS pre-warm and SD mount so max_alloc is at its highest.
        BaseType_t rc = xTaskCreatePinnedToCore(
            uploadTaskFunction,  // Task function
            "upload",            // Name
            12288,               // Stack size (12KB — verified via HWM logging)
            params,              // Parameters
            1,                   // Priority (same as loop task)
            &uploadTaskHandle,   // Handle
            0                    // Pin to Core 0
        );
        
        if (rc != pdPASS) {
            LOG_ERRORF("[FSM] Failed to create upload task (rc=%ld, free=%u, max_alloc=%u) \u2014 releasing",
                       (long)rc,
                       ESP.getFreeHeap(),
                       ESP.getMaxAllocHeap());
            uploadTaskRunning = false;
            delete params;
            esp_task_wdt_add(xTaskGetIdleTaskHandleForCPU(0));
#ifdef ENABLE_WEBSERVER
            uploader->setWebServer(webServer);
#endif
            transitionTo(UploadState::RELEASING);
        } else {
            LOG("[FSM] Upload task started on Core 0 (non-blocking)");
        }
    } else if (uploadTaskComplete) {
        // ── Task finished: read result and transition ──
        uploadTaskRunning = false;
        uploadTaskHandle = nullptr;
        g_abortUploadFlag = false;  // Clear abort flag — task has stopped
        
        // Re-subscribe IDLE0 to task watchdog now that Core 0 is free
        esp_task_wdt_add(xTaskGetIdleTaskHandleForCPU(0));
        
        // Restore web server handling in uploader
#ifdef ENABLE_WEBSERVER
        uploader->setWebServer(webServer);
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
            case UploadResult::NOTHING_TO_DO:
                LOG("[FSM] Nothing to upload — releasing SD and entering cooldown (no reboot)");
                g_nothingToUpload = true;
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
    
    // Restart ULP monitor for low-power CS_SENSE detection during idle states
    if (!ulpMonitor.isRunning()) {
        ulpMonitor.begin();
    }
    
    // If monitoring was requested during upload, go to MONITORING instead of COOLDOWN
    if (monitoringRequested) {
        monitoringRequested = false;
        trafficMonitor.enableSampleBuffer();  // Allocate buffer for web UI
        trafficMonitor.resetStatistics();
        LOG("[FSM] Monitoring requested during upload — entering MONITORING after release");
        transitionTo(UploadState::MONITORING);
        return;
    }

    // If nothing was uploaded, skip the reboot and go straight to cooldown.
    // This prevents an endless reboot cycle when all backends are already synced.
    if (g_nothingToUpload) {
        g_nothingToUpload = false;
        LOGF("[FSM] Nothing to upload — entering cooldown without reboot (fh=%u ma=%u)",
             (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
        cooldownStartedAt = millis();
        transitionTo(UploadState::COOLDOWN);
        return;
    }

    // MINIMIZE_REBOOTS: skip elective reboot and reuse existing runtime.
    // The device enters COOLDOWN → LISTENING and picks up work in the next cycle.
    if (config.getMinimizeReboots()) {
        unsigned fh = (unsigned)ESP.getFreeHeap();
        unsigned ma = (unsigned)ESP.getMaxAllocHeap();
        LOGF("[FSM] MINIMIZE_REBOOTS: skipping elective reboot after upload (fh=%u ma=%u)", fh, ma);
        if (ma < 35000) {
            LOG_WARN("[FSM] Heap fragmented — contiguous block below 35KB. Consider rebooting if uploads fail.");
        }
        uploadCycleHadTimeout = false;
        cooldownStartedAt = millis();
        transitionTo(UploadState::COOLDOWN);
        return;
    }

    // Default: soft-reboot after a real upload session.
    // A clean reboot restores the full contiguous heap and keeps the FSM simple.
    // The fast-boot path (ESP_RST_SW) skips cold-boot delays.
    LOGF("[FSM] Upload session complete — soft-reboot to restore heap (fh=%u ma=%u)",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    setRebootReason("Heap recovery after upload session");
    Logger::getInstance().flushBeforeReboot();
    delay(200);
    esp_restart();
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
    // Clear brownout-recovery mode after a successful upload — the device
    // has proven it can sustain a full upload cycle at current power levels.
    if (g_brownoutRecoveryBoot) {
        g_brownoutRecoveryBoot = false;
        LOG_INFO("[POWER] Brownout-recovery mode cleared — successful upload cycle");
    }
    
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
        trafficMonitor.disableSampleBuffer();  // Free ~2.4KB buffer
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
    
    // Periodic persisted-log flush (every 10 seconds)
    // Uses multi-file rotation on LittleFS — independent of SD_MMC / upload task.
    // ── POWER: Skip during active uploads to avoid internal SPI flash writes
    // overlapping with SD reads, TLS encryption, and WiFi TX bursts.
    // flushBeforeReboot() ensures no logs are lost on post-upload reboot.
    if (!uploadTaskRunning) {
        unsigned long currentTime = millis();
        if (currentTime - lastLogFlushTime >= LOG_FLUSH_INTERVAL_MS) {
            Logger::getInstance().dumpSavedLogsPeriodic(nullptr);
            lastLogFlushTime = currentTime;
        }
    }
    
    // Update traffic monitor only in states that need activity detection
    // LISTENING: needs idle detection to trigger uploads
    // MONITORING: needs live data for web UI
    if (currentState == UploadState::LISTENING || currentState == UploadState::MONITORING) {
        trafficMonitor.update();
    }

#ifdef ENABLE_WEBSERVER
    // Handle web server requests
    if (webServer) {
        webServer->handleClient();
        // Push SSE log events to connected client (if any).
        // ── POWER: Suppress SSE during uploads to eliminate continuous WiFi TX
        // churn from log streaming while SD + TLS + WiFi are all active.
        if (!uploadTaskRunning && !g_brownoutRecoveryBoot) {
            pushSseLogs();
        }
        // Status snapshot is rebuilt on-demand in handleApiStatus() — no periodic
        // rebuild needed. The API handler calls updateStatusSnapshot() before
        // serving, so the data is always fresh when a client requests it.
    }
    
    // ── POWER: Timed mDNS — stop responder after 60 seconds ──
    // mDNS is only needed for initial .local discovery. After the browser
    // resolves the hostname and gets redirected to the IP, mDNS can stop
    // to eliminate multicast group membership and associated radio wakes.
    if (!g_mdnsTimedOut && g_mdnsStartTime > 0 &&
        (millis() - g_mdnsStartTime >= MDNS_ACTIVE_DURATION_MS)) {
        g_mdnsTimedOut = true;
        MDNS.end();
        LOG_DEBUG("[POWER] Timed mDNS stopped after 60 seconds");
    }
    
    // ── Software watchdog for upload task ──
    // If the upload task hasn't sent a heartbeat in UPLOAD_WATCHDOG_TIMEOUT_MS, it's hung.
    // Force-kill it and reboot — vTaskDelete mid-SD-I/O corrupts the SD bus,
    // making remount impossible. A clean reboot is the only reliable recovery.
    if (uploadTaskRunning && g_uploadHeartbeat > 0 &&
        (millis() - g_uploadHeartbeat > UPLOAD_WATCHDOG_TIMEOUT_MS)) {
        LOG_ERROR("[FSM] Upload task appears hung (no heartbeat for 2 minutes) — rebooting");
        
        // Set NVS flag so we can log the reason on next boot
        Preferences wdPrefs;
        wdPrefs.begin("cpap_flags", false);
        wdPrefs.putBool("watchdog_kill", true);
        wdPrefs.end();
        
        if (uploadTaskHandle) {
            vTaskDelete(uploadTaskHandle);
        }
        
        setRebootReason("Upload task hung (software watchdog, >2 min no heartbeat)");
        Logger::getInstance().flushBeforeReboot();
        delay(300);
        esp_restart();
    }
    
    // ── Web trigger handlers (operate independently of FSM) ──
    
    // Check for state reset trigger — takes effect IMMEDIATELY, even during upload.
    // Strategy: set NVS flag → kill upload task → reboot.
    // State files are deleted on next boot with a clean SD card mount.
    // This avoids SD card access after killing a task mid-I/O (which can hang).
    if (g_resetStateFlag) {
        LOG("=== State Reset Triggered via Web Interface ===");
        g_resetStateFlag = false;
        
        // Set NVS flag so state files are deleted on next clean boot
        Preferences resetPrefs;
        resetPrefs.begin("cpap_flags", false);
        resetPrefs.putBool("reset_state", true);
        resetPrefs.end();
        LOG("Reset flag saved to NVS");
        
        // Kill upload task if running (don't touch SD card after this!)
        if (uploadTaskRunning && uploadTaskHandle) {
            LOG_WARN("[FSM] Killing active upload task for state reset");
            vTaskDelete(uploadTaskHandle);
            uploadTaskRunning = false;
            uploadTaskHandle = nullptr;
        }
        
        // Immediate reboot — state files deleted on next clean boot
        LOG("Rebooting for clean state reset...");
        setRebootReason("State reset requested via Web UI");
        Logger::getInstance().flushBeforeReboot();
        delay(300);  // Brief pause for web response to send
        esp_restart();
    }
    
    // Soft reboot — next boot detects ESP_RST_SW and skips all delays automatically
    if (g_softRebootFlag) {
        LOG("=== Soft Reboot Triggered via Web Interface ===");
        g_softRebootFlag = false;
        setRebootReason("Soft reboot requested via Web UI");
        Logger::getInstance().flushBeforeReboot();
        delay(300);
        esp_restart();
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
    // GUARD: Do NOT attempt reconnection while upload task is running on Core 0.
    // The upload task manages its own WiFi recovery via tryCoordinatedWifiCycle().
    // Concurrent reconnection from both cores corrupts the lwIP state machine.
    if (!wifiManager.isConnected() && !uploadTaskRunning) {
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
            
            // Restart timed mDNS after reconnection (unless brownout-recovery mode)
            if (!g_brownoutRecoveryBoot) {
                wifiManager.startMDNS(config.getHostname());
                g_mdnsStartTime = millis();
                g_mdnsTimedOut = false;
            }
            
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
            stopMonitoringRequested = false;  // discard any stale stop from before monitoring began
            if (currentState == UploadState::ACQUIRING && sdManager.hasControl()) {
                sdManager.releaseControl();
            }
            trafficMonitor.enableSampleBuffer();  // Allocate buffer for web UI
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
    
    // ── POWER: Yield CPU so DFS can scale down ──
    // Without explicit yields the loop task never blocks, keeping CPU at max
    // frequency. State-appropriate delays allow the FreeRTOS IDLE task to run,
    // triggering automatic frequency scaling (DFS) when no work is pending.
    switch (currentState) {
        case UploadState::IDLE:
        case UploadState::COOLDOWN:
            vTaskDelay(pdMS_TO_TICKS(100));  // Low-frequency states: 100ms yield
            break;
        case UploadState::LISTENING:
            vTaskDelay(pdMS_TO_TICKS(50));   // TrafficMonitor samples; 50ms is sufficient
            break;
        case UploadState::MONITORING:
        case UploadState::UPLOADING:
            vTaskDelay(pdMS_TO_TICKS(10));   // Responsive states: 10ms yield
            break;
        default:
            vTaskDelay(pdMS_TO_TICKS(10));   // Transient states: brief yield
            break;
    }
}
