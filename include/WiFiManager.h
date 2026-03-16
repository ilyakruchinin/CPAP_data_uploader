#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

// Forward declarations for power management enums
enum class WifiTxPower;
enum class WifiPowerSaving;

class WiFiManager {
private:
    bool connected;
    bool mdnsStarted;
    int8_t _pendingTxPower;   // Deferred TX power (dBm×4), applied in connectStation()
    bool _hasPendingTxPower;
    static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info);

public:
    WiFiManager();
    
    void setupEventHandlers();
    bool connectStation(const String& ssid, const String& password);
    bool isConnected() const;
    void disconnect();
    String getIPAddress() const;
    int getSignalStrength() const;  // Returns RSSI in dBm
    String getSignalQuality() const;  // Returns quality description
    
    // mDNS support
    bool startMDNS(const String& hostname);
    
    // Power management methods
    void setHighPerformanceMode();    // Disable power save for uploads
    void setPowerSaveMode();          // Enable power save for idle periods
    void setMaxPowerSave();          // Maximum power savings
    void applyTxPowerEarly(WifiTxPower txPower);  // Set TX power before WiFi.begin()
    void applyPowerSettings(WifiTxPower txPower, WifiPowerSaving powerSaving);  // Apply config settings
};

#endif // WIFI_MANAGER_H
