#include "WiFiManager.h"
#include "Logger.h"
#include "Config.h"  // For power management enums
#include "SDCardManager.h"
#include <WiFi.h>
#include <ESPmDNS.h>

WiFiManager::WiFiManager() : connected(false), mdnsStarted(false) {}

void WiFiManager::setupEventHandlers() {
    WiFi.onEvent(onWiFiEvent);
    LOG_DEBUG("WiFi event handlers registered");
}

void WiFiManager::onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_READY:
            LOG_DEBUG("WiFi Event: WiFi interface ready");
            break;
            
        case ARDUINO_EVENT_WIFI_SCAN_DONE:
            LOG_DEBUG("WiFi Event: Scan completed");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_START:
            LOG_INFO("WiFi Event: Station mode started");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_STOP:
            LOG_INFO("WiFi Event: Station mode stopped");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            LOG_INFO("WiFi Event: Connected to AP");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
            uint8_t reason = info.wifi_sta_disconnected.reason;
            LOG_WARNF("WiFi Event: Disconnected from AP (reason: %d)", reason);
            
            // Log human-readable disconnect reasons
            switch (reason) {
                case WIFI_REASON_UNSPECIFIED:
                    LOG_WARN("Disconnect reason: Unspecified");
                    break;
                case WIFI_REASON_AUTH_EXPIRE:
                    LOG_WARN("Disconnect reason: Authentication expired");
                    break;
                case WIFI_REASON_AUTH_LEAVE:
                    LOG_WARN("Disconnect reason: Deauthenticated (left network)");
                    break;
                case WIFI_REASON_ASSOC_EXPIRE:
                    LOG_WARN("Disconnect reason: Association expired");
                    break;
                case WIFI_REASON_ASSOC_TOOMANY:
                    LOG_WARN("Disconnect reason: Too many associations");
                    break;
                case WIFI_REASON_NOT_AUTHED:
                    LOG_WARN("Disconnect reason: Not authenticated");
                    break;
                case WIFI_REASON_NOT_ASSOCED:
                    LOG_WARN("Disconnect reason: Not associated");
                    break;
                case WIFI_REASON_ASSOC_LEAVE:
                    LOG_WARN("Disconnect reason: Disassociated (left network)");
                    break;
                case WIFI_REASON_ASSOC_NOT_AUTHED:
                    LOG_WARN("Disconnect reason: Association not authenticated");
                    break;
                case WIFI_REASON_DISASSOC_PWRCAP_BAD:
                    LOG_WARN("Disconnect reason: Bad power capability");
                    break;
                case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
                    LOG_WARN("Disconnect reason: Bad supported channels");
                    break;
                case WIFI_REASON_IE_INVALID:
                    LOG_WARN("Disconnect reason: Invalid information element");
                    break;
                case WIFI_REASON_MIC_FAILURE:
                    LOG_WARN("Disconnect reason: MIC failure");
                    break;
                case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                    LOG_WARN("Disconnect reason: 4-way handshake timeout");
                    break;
                case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
                    LOG_WARN("Disconnect reason: Group key update timeout");
                    break;
                case WIFI_REASON_IE_IN_4WAY_DIFFERS:
                    LOG_WARN("Disconnect reason: IE in 4-way handshake differs");
                    break;
                case WIFI_REASON_GROUP_CIPHER_INVALID:
                    LOG_WARN("Disconnect reason: Invalid group cipher");
                    break;
                case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
                    LOG_WARN("Disconnect reason: Invalid pairwise cipher");
                    break;
                case WIFI_REASON_AKMP_INVALID:
                    LOG_WARN("Disconnect reason: Invalid AKMP");
                    break;
                case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
                    LOG_WARN("Disconnect reason: Unsupported RSN IE version");
                    break;
                case WIFI_REASON_INVALID_RSN_IE_CAP:
                    LOG_WARN("Disconnect reason: Invalid RSN IE capability");
                    break;
                case WIFI_REASON_802_1X_AUTH_FAILED:
                    LOG_WARN("Disconnect reason: 802.1X authentication failed");
                    break;
                case WIFI_REASON_CIPHER_SUITE_REJECTED:
                    LOG_WARN("Disconnect reason: Cipher suite rejected");
                    break;
                case WIFI_REASON_BEACON_TIMEOUT:
                    LOG_WARN("Disconnect reason: Beacon timeout (AP lost)");
                    break;
                case WIFI_REASON_NO_AP_FOUND:
                    LOG_WARN("Disconnect reason: No AP found");
                    break;
                case WIFI_REASON_AUTH_FAIL:
                    LOG_WARN("Disconnect reason: Authentication failed");
                    break;
                case WIFI_REASON_ASSOC_FAIL:
                    LOG_WARN("Disconnect reason: Association failed");
                    break;
                case WIFI_REASON_HANDSHAKE_TIMEOUT:
                    LOG_WARN("Disconnect reason: Handshake timeout");
                    break;
                case WIFI_REASON_CONNECTION_FAIL:
                    LOG_WARN("Disconnect reason: Connection failed");
                    break;
                default:
                    LOG_WARNF("Disconnect reason: Unknown (%d)", reason);
                    break;
            }
            break;
        }
            
        case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
            LOG_DEBUG("WiFi Event: Authentication mode changed");
            break;
            
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            LOG_INFOF("WiFi Event: Got IP address: %s", WiFi.localIP().toString().c_str());
            break;
            
        case ARDUINO_EVENT_WIFI_STA_LOST_IP:
            LOG_WARN("WiFi Event: Lost IP address");
            break;
            
        default:
            LOG_DEBUGF("WiFi Event: Unhandled event %d", event);
            break;
    }
}

bool WiFiManager::connectStation(const String& ssid, const String& password) {
    // Validate SSID before attempting connection
    if (ssid.isEmpty()) {
        LOG_ERROR("Cannot connect to WiFi: SSID is empty");
        Logger::getInstance().dumpSavedLogs("wifi_config_error");
        return false;
    }
    
    if (ssid.length() > 32) {
        LOG_ERROR("Cannot connect to WiFi: SSID exceeds 32 character limit");
        LOGF("SSID length: %d characters", ssid.length());
        Logger::getInstance().dumpSavedLogs("wifi_config_error");
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
        
        // Persist logs for critical connection failures
        Logger::getInstance().dumpSavedLogs("wifi_connection_failed");
        connected = false;
        return false;
    }
}

bool WiFiManager::isConnected() const { 
    return connected && WiFi.status() == WL_CONNECTED; 
}

void WiFiManager::disconnect() {
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
    }
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

bool WiFiManager::startMDNS(const String& hostname) {
    if (!connected || WiFi.status() != WL_CONNECTED) {
        LOG_WARN("Cannot start mDNS: WiFi not connected");
        return false;
    }

    String name = hostname;
    if (name.isEmpty()) {
        name = "cpap"; // Default hostname
    }

    LOGF("Starting mDNS responder with hostname: %s.local", name.c_str());

    // Ensure stale responder state from prior reconnects is released first.
    if (mdnsStarted) {
        MDNS.end();
        mdnsStarted = false;
        delay(10);
    }
    
    if (MDNS.begin(name.c_str())) {
        LOG("mDNS responder started successfully");
        // Advertise web server service
        MDNS.addService("http", "tcp", 80);
        mdnsStarted = true;
        return true;
    } else {
        LOG_ERROR("Failed to start mDNS responder");
        mdnsStarted = false;
        return false;
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