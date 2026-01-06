#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

// Forward declarations for power management enums
enum class WifiTxPower;
enum class WifiPowerSaving;

class WiFiManager {
private:
    bool connected;

public:
    WiFiManager();
    
    bool connectStation(const String& ssid, const String& password);
    bool isConnected() const;
    void disconnect();
    String getIPAddress() const;
    int getSignalStrength() const;  // Returns RSSI in dBm
    String getSignalQuality() const;  // Returns quality description
    
    // Power management methods
    void setHighPerformanceMode();    // Disable power save for uploads
    void setPowerSaveMode();          // Enable power save for idle periods
    void setMaxPowerSave();          // Maximum power savings
    void applyPowerSettings(WifiTxPower txPower, WifiPowerSaving powerSaving);  // Apply config settings
};

#endif // WIFI_MANAGER_H
