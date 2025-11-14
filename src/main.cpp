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
unsigned long nextUploadCheckTime = 0;
bool uploadSessionActive = false;

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
    // Note: shouldUpload() will trigger NTP sync internally
    // We just check if it worked
    if (!uploader->shouldUpload()) {
        Serial.println("WARNING: Initial NTP synchronization may have failed");
        Serial.println("Will retry NTP sync every 5 minutes until successful");
        lastNtpSyncAttempt = millis();
    } else {
        Serial.println("Time synchronized successfully");
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
    if (uploader) {
        unsigned long currentTime = millis();
        if (currentTime - lastNtpSyncAttempt >= NTP_RETRY_INTERVAL_MS) {
            Serial.println("Attempting NTP synchronization...");
            if (uploader->shouldUpload()) {
                Serial.println("NTP synchronization successful");
            } else {
                Serial.println("WARNING: NTP synchronization failed, will retry in 5 minutes");
            }
            lastNtpSyncAttempt = currentTime;
        }
    }

    // Check if we're in an active upload session
    if (uploadSessionActive) {
        // Wait for the appropriate time before next session
        if (millis() < nextUploadCheckTime) {
            delay(1000);  // Check every second during wait period
            return;
        }
        uploadSessionActive = false;
    }

    // Check if it's time to upload (scheduled window)
    if (!uploader || !uploader->shouldUpload()) {
        // Not upload time yet, wait before checking again
        delay(60000);  // Check every minute when not in upload window
        return;
    }

    Serial.println("Upload time detected, attempting to start upload session...");

    // Try to take control of SD card for upload session
    if (!sdManager.takeControl()) {
        Serial.println("CPAP machine is using SD card, will retry...");
        delay(5000);  // Wait 5 seconds before retrying
        return;
    }

    Serial.println("SD card control acquired, starting upload session...");

    // Perform upload session
    bool uploadSuccess = uploader->uploadNewFiles(sdManager.getFS());

    // Release SD card back to CPAP machine
    sdManager.releaseControl();
    Serial.println("SD card control released");

    if (uploadSuccess) {
        Serial.println("Upload session completed successfully");
    } else {
        Serial.println("Upload session completed with errors or budget exhaustion");
    }

    // Calculate wait time (2x session duration) before next upload attempt
    unsigned long sessionDuration = config.getSessionDurationSeconds() * 1000;
    unsigned long waitTime = sessionDuration * 2;
    
    Serial.print("Waiting ");
    Serial.print(waitTime / 1000);
    Serial.println(" seconds before next upload session...");
    
    nextUploadCheckTime = millis() + waitTime;
    uploadSessionActive = true;
}
