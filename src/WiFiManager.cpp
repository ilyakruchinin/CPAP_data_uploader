#include "WiFiManager.h"
#include "Logger.h"
#include "Config.h"  // For power management enums
#include "SDCardManager.h"
#include <WiFi.h>

WiFiManager::WiFiManager() : connected(false) {}

bool WiFiManager::connectStation(const String& ssid, const String& password) {
    // Validate SSID before attempting connection
    if (ssid.isEmpty()) {
        LOG_ERROR("Cannot connect to WiFi: SSID is empty");
        Logger::getInstance().dumpLogsToSDCard("wifi_config_error");
        return false;
    }
    
    if (ssid.length() > 32) {
        LOG_ERROR("Cannot connect to WiFi: SSID exceeds 32 character limit");
        LOGF("SSID length: %d characters", ssid.length());
        Logger::getInstance().dumpLogsToSDCard("wifi_config_error");
        return false;
    }
    
    if (password.isEmpty()) {
        LOG_WARN("WiFi password is empty - attempting open network connection");
    }
    
    LOGF("Connecting to WiFi: %s", ssid.c_str());
    LOGF("SSID length: %d characters", ssid.length());

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        LOG_DEBUG(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        LOG("\nWiFi connected");
        LOGF("IP address: %s", WiFi.localIP().toString().c_str());
        connected = true;
        return true;
    } else {
        LOG("\nWiFi connection failed");
        LOGF("WiFi status: %d", WiFi.status());
        
        // Log detailed failure reason
        switch (WiFi.status()) {
            case WL_NO_SSID_AVAIL:
                LOG_ERROR("WiFi failure: SSID not found");
                break;
            case WL_CONNECT_FAILED:
                LOG_ERROR("WiFi failure: Connection failed (wrong password?)");
                break;
            case WL_CONNECTION_LOST:
                LOG_ERROR("WiFi failure: Connection lost");
                break;
            case WL_DISCONNECTED:
                LOG_ERROR("WiFi failure: Disconnected");
                break;
            default:
                LOGF("WiFi failure: Unknown status %d", WiFi.status());
                break;
        }
        
        // Dump logs to SD card for critical connection failures
        Logger::getInstance().dumpLogsToSDCard("wifi_connection_failed");
        connected = false;
        return false;
    }
}

bool WiFiManager::isConnected() const { 
    return connected && WiFi.status() == WL_CONNECTED; 
}

void WiFiManager::disconnect() {
    WiFi.disconnect();
    connected = false;
}

String WiFiManager::getIPAddress() const {
    if (connected && WiFi.status() == WL_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "Not connected";
}

int WiFiManager::getSignalStrength() const {
    if (connected && WiFi.status() == WL_CONNECTED) {
        return WiFi.RSSI();  // Returns signal strength in dBm
    }
    return 0;
}

String WiFiManager::getSignalQuality() const {
    if (!connected || WiFi.status() != WL_CONNECTED) {
        return "Not connected";
    }
    
    int rssi = WiFi.RSSI();
    
    // Classify signal strength based on RSSI value
    // RSSI ranges: Excellent > -50, Good > -60, Fair > -70, Weak > -80, Very Weak <= -80
    if (rssi > -50) {
        return "Excellent";
    } else if (rssi > -60) {
        return "Good";
    } else if (rssi > -70) {
        return "Fair";
    } else if (rssi > -80) {
        return "Weak";
    } else {
        return "Very Weak";
    }
}
// Power management methods
void WiFiManager::setHighPerformanceMode() {
    if (connected && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(false);  // Disable all power saving
        LOG_DEBUG("WiFi set to high performance mode (no power saving)");
    }
}

void WiFiManager::setPowerSaveMode() {
    if (connected && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(WIFI_PS_MIN_MODEM);  // Enable minimum modem sleep
        LOG_DEBUG("WiFi set to power save mode (WIFI_PS_MIN_MODEM)");
    }
}

void WiFiManager::setMaxPowerSave() {
    if (connected && WiFi.status() == WL_CONNECTED) {
        WiFi.setSleep(WIFI_PS_MAX_MODEM);  // Enable maximum modem sleep
        LOG_DEBUG("WiFi set to maximum power save mode (WIFI_PS_MAX_MODEM)");
    }
}
void WiFiManager::applyPowerSettings(WifiTxPower txPower, WifiPowerSaving powerSaving) {
    if (!connected || WiFi.status() != WL_CONNECTED) {
        LOG_WARN("Cannot apply power settings - WiFi not connected");
        return;
    }
    
    // Apply TX power setting
    wifi_power_t espTxPower;
    switch (txPower) {
        case WifiTxPower::POWER_HIGH:
            espTxPower = WIFI_POWER_19_5dBm;  // ~20dBm (maximum)
            LOG_DEBUG("WiFi TX power set to HIGH (19.5dBm)");
            break;
        case WifiTxPower::POWER_MID:
            espTxPower = WIFI_POWER_11dBm;    // 11dBm (medium)
            LOG_DEBUG("WiFi TX power set to MID (11dBm)");
            break;
        case WifiTxPower::POWER_LOW:
            espTxPower = WIFI_POWER_5dBm;     // 5dBm (low)
            LOG_DEBUG("WiFi TX power set to LOW (5dBm)");
            break;
        default:
            espTxPower = WIFI_POWER_19_5dBm;
            LOG_WARN("Unknown TX power setting, using HIGH");
            break;
    }
    WiFi.setTxPower(espTxPower);
    
    // Apply power saving setting
    switch (powerSaving) {
        case WifiPowerSaving::SAVE_NONE:
            WiFi.setSleep(false);
            LOG_DEBUG("WiFi power saving disabled (high performance)");
            break;
        case WifiPowerSaving::SAVE_MID:
            WiFi.setSleep(WIFI_PS_MIN_MODEM);
            LOG_DEBUG("WiFi power saving set to MID (WIFI_PS_MIN_MODEM)");
            break;
        case WifiPowerSaving::SAVE_MAX:
            WiFi.setSleep(WIFI_PS_MAX_MODEM);
            LOG_DEBUG("WiFi power saving set to MAX (WIFI_PS_MAX_MODEM)");
            break;
        default:
            WiFi.setSleep(false);
            LOG_WARN("Unknown power saving setting, disabling power save");
            break;
    }
}