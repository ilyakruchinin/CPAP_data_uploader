#include <Arduino.h>
#include "Config.h"
#include "SDCardManager.h"
#include "WiFiManager.h"
#include "FileUploader.h"

// ============================================================================
// Global Objects
// ============================================================================
Config config;
SDCardManager sdManager;
WiFiManager wifiManager;
FileUploader* uploader = nullptr;

// ============================================================================
// Global State
// ============================================================================
unsigned long lastNtpSyncAttempt = 0;
const unsigned long NTP_RETRY_INTERVAL_MS = 5 * 60 * 1000;  // 5 minutes
unsigned long nextUploadRetryTime = 0;
bool budgetExhaustedRetry = false;  // True if waiting due to budget exhaustion

// ============================================================================
// Setup Function
// ============================================================================
void setup() {
    // Initialize serial port
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n\n=== CPAP Data Auto-Uploader ===");

    // Initialize SD card control
    if (!sdManager.begin()) {
        Serial.println("Failed to initialize SD card manager");
        return;
    }

    // Take control of SD card
    Serial.println("Waiting to access SD card...");
    while (!sdManager.takeControl()) {
        delay(1000);
    }

    // Read config file from SD card
    Serial.println("Loading configuration...");
    if (!config.loadFromSD(sdManager.getFS())) {
        Serial.println("Failed to load configuration");
        sdManager.releaseControl();
        return;
    }

    Serial.println("Configuration loaded successfully");
    Serial.print("WiFi SSID: ");
    Serial.println(config.getWifiSSID());
    Serial.print("Endpoint: ");
    Serial.println(config.getEndpoint());

    // Release SD card back to CPAP machine
    sdManager.releaseControl();

    // Initialize WiFi in station mode
    if (!wifiManager.connectStation(config.getWifiSSID(), config.getWifiPassword())) {
        Serial.println("Failed to connect to WiFi");
        return;
    }

    // Initialize uploader
    uploader = new FileUploader(&config, &wifiManager);
    
    // Take control of SD card to initialize uploader components
    Serial.println("Initializing uploader...");
    if (sdManager.takeControl()) {
        if (!uploader->begin(sdManager.getFS())) {
            Serial.println("ERROR: Failed to initialize uploader");
            sdManager.releaseControl();
            return;
        }
        sdManager.releaseControl();
        Serial.println("Uploader initialized successfully");
    } else {
        Serial.println("ERROR: Failed to take SD card control for uploader initialization");
        return;
    }
    
    // Synchronize time with NTP server
    Serial.println("Synchronizing time with NTP server...");
    // The ScheduleManager handles NTP sync internally during begin()
    // We can check if time is synced by calling shouldUpload()
    // (it will return false if time is not synced)
    
    // Trigger initial time sync check
    if (uploader->shouldUpload()) {
        Serial.println("Time synchronized successfully");
        Serial.println("Currently in upload window - will begin upload shortly");
    } else {
        Serial.println("Time sync status unknown or not in upload window");
        Serial.println("Will retry NTP sync every 5 minutes if needed");
        lastNtpSyncAttempt = millis();
    }

    Serial.println("Setup complete!");
}

// ============================================================================
// Loop Function
// ============================================================================
void loop() {
    // Check WiFi connection
    if (!wifiManager.isConnected()) {
        Serial.println("WARNING: WiFi disconnected, attempting to reconnect...");
        if (!wifiManager.connectStation(config.getWifiSSID(), config.getWifiPassword())) {
            Serial.println("ERROR: Failed to reconnect to WiFi");
            Serial.println("Will retry in 30 seconds...");
            delay(30000);
            return;
        }
        Serial.println("WiFi reconnected successfully");
        
        // Reset NTP sync attempt timer to trigger immediate sync after reconnection
        lastNtpSyncAttempt = 0;
    }

    // Handle NTP sync retry if time is not synchronized
    // This runs periodically to ensure time stays synchronized
    if (uploader) {
        unsigned long currentTime = millis();
        if (currentTime - lastNtpSyncAttempt >= NTP_RETRY_INTERVAL_MS) {
            Serial.println("Periodic NTP synchronization check...");
            // Note: shouldUpload() checks time sync internally
            // We just want to trigger the check periodically
            lastNtpSyncAttempt = currentTime;
        }
    }

    // Check if we're waiting due to budget exhaustion
    if (budgetExhaustedRetry) {
        // Wait for the appropriate time before retrying
        if (millis() < nextUploadRetryTime) {
            delay(1000);  // Check every second during wait period
            return;
        }
        // Wait period over, clear the flag and continue
        budgetExhaustedRetry = false;
        Serial.println("Budget exhaustion wait period complete, resuming upload...");
    }

    // Check if it's time to upload (scheduled window - once per day)
    if (!uploader || !uploader->shouldUpload()) {
        // Not upload time yet, wait before checking again
        delay(60000);  // Check every minute when not in upload window
        return;
    }

    Serial.println("=== Upload Window Active ===");
    Serial.println("Attempting to start upload session...");

    // Try to take control of SD card for upload session
    if (!sdManager.takeControl()) {
        Serial.println("CPAP machine is using SD card, will retry in 5 seconds...");
        delay(5000);  // Wait 5 seconds before retrying
        return;
    }

    Serial.println("SD card control acquired, starting upload session...");

    // Perform upload session
    // Note: uploadNewFiles() handles:
    // - Time budget enforcement
    // - File prioritization (DATALOG newest first, then root/SETTINGS)
    // - State persistence
    // - Retry count management
    bool uploadSuccess = uploader->uploadNewFiles(sdManager.getFS());

    // Release SD card back to CPAP machine
    sdManager.releaseControl();
    Serial.println("SD card control released");

    // Determine if we need to wait before retrying
    // The upload can fail for two reasons:
    // 1. Budget exhausted (partial upload) - need to wait 2x session duration
    // 2. All files uploaded or scheduled upload complete - wait until next day
    
    if (uploadSuccess) {
        Serial.println("=== Upload Session Completed Successfully ===");
        Serial.println("All pending files have been uploaded");
        Serial.println("Next upload will occur at scheduled time tomorrow");
        // The ScheduleManager has already marked upload as completed
        // No need to set retry timer - will wait for next scheduled time
    } else {
        Serial.println("=== Upload Session Incomplete ===");
        Serial.println("Session ended due to time budget exhaustion or errors");
        
        // Calculate wait time (2x session duration) before retry
        unsigned long sessionDuration = config.getSessionDurationSeconds() * 1000;
        unsigned long waitTime = sessionDuration * 2;
        
        Serial.print("Waiting ");
        Serial.print(waitTime / 1000);
        Serial.println(" seconds before retry...");
        Serial.println("This allows CPAP machine priority access to SD card");
        
        nextUploadRetryTime = millis() + waitTime;
        budgetExhaustedRetry = true;
        
        // Note: We stay in the same upload window (same day)
        // The ScheduleManager will NOT mark upload as completed
        // So shouldUpload() will continue to return true after wait period
    }
}
