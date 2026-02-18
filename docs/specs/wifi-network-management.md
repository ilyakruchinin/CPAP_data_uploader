# WiFi Network Management

## Overview
The WiFi Manager (`WiFiManager.cpp/.h`) handles all network connectivity including connection establishment, recovery, monitoring, and network-level error handling for both upload operations and web interface access.

## Core Features

### Connection Management
- **Automatic connection**: Connects to configured WiFi network on startup
- **Persistent settings**: Remembers connection parameters across reboots
- **Reconnection logic**: Automatic recovery from connection drops
- **Network monitoring**: Continuous connectivity and signal strength monitoring

### Recovery Mechanisms
```cpp
bool recoverConnection() {
    // Cycle WiFi connection to recover from errors
    WiFi.disconnect();
    delay(1000);
    WiFi.reconnect();
    
    // Wait for reconnection with timeout
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 30000) {
        delay(500);
    }
    return WiFi.status() == WL_CONNECTED;
}
```

## Configuration

### Network Settings
- **SSID**: From `WIFI_SSID` configuration
- **Password**: From `WIFI_PASSWORD` configuration (securely stored)
- **Hostname**: From `HOSTNAME` configuration (default: "cpap")
- **Power management**: Configurable TX power and saving modes

### Power Options
```cpp
enum class WifiTxPower {
    POWER_LOW,    // ~2dBm output
    POWER_MID,    // ~10dBm output  
    POWER_HIGH    // ~20dBm output (default)
};

enum class WifiPowerSaving {
    SAVE_NONE,    // No power saving (default)
    SAVE_MID,     // Moderate power saving
    SAVE_MAX      // Maximum power saving
};
```

## Connection Process

### 1. Initialization
```cpp
bool begin() {
    // Configure hostname for mDNS
    WiFi.setHostname(config->getHostname().c_str());
    
    // Set power management
    setTxPower(config->getWifiTxPower());
    setPowerSaving(config->getWifiPowerSaving());
    
    // Start connection
    return connect();
}
```

### 2. Connection Establishment
```cpp
bool connect() {
    LOGF("[WiFi] Connecting to: %s", config->getWifiSsid().c_str());
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(config->getWifiSsid().c_str(), 
               config->getWifiPassword().c_str());
    
    // Wait for connection with timeout
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < 30000) {
        delay(500);
        feedUploadHeartbeat();
    }
    
    return WiFi.status() == WL_CONNECTED;
}
```

### 3. Monitoring
```cpp
void update() {
    // Called from main loop
    if (WiFi.status() != WL_CONNECTED) {
        if (shouldReconnect()) {
            recoverConnection();
        }
    }
    
    // Update signal strength for status reporting
    if (WiFi.status() == WL_CONNECTED) {
        _rssi = WiFi.RSSI();
    }
}
```

## Advanced Features

### mDNS Integration
```cpp
void setupMDNS() {
    if (MDNS.begin(config->getHostname().c_str())) {
        MDNS.addService("http", "tcp", 80);
        LOGF("[WiFi] mDNS responder started: http://%s.local", 
             config->getHostname().c_str());
    }
}
```

### Network Diagnostics
- **Signal strength**: RSSI monitoring and reporting
- **Connection quality**: Packet loss detection
- **IP configuration**: DHCP status and IP address
- **Network uptime**: Connection duration tracking

### Error Recovery
- **Authentication failures**: Log error, don't retry continuously
- **DHCP failures**: Retry with exponential backoff
- **Connection drops**: Automatic reconnection
- **WiFi hardware errors**: Reset WiFi subsystem

## Performance Characteristics

### Connection Timing
- **Initial connection**: 10-30 seconds (depends on network)
- **Reconnection**: 5-15 seconds (faster with remembered networks)
- **IP acquisition**: 2-5 seconds via DHCP
- **mDNS registration**: <1 second

### Power Consumption
- **High power**: ~150mA during active transmission
- **Low power**: ~80mA with power saving enabled
- **Sleep mode**: ~20mA (not used in this application)
- **Idle**: ~50mA connected but not transmitting

## Integration Points

### Upload Operations
- **Cloud uploads**: Required for SleepHQ API access
- **OTA updates**: Required for firmware downloads
- **Time sync**: Required for NTP time synchronization
- **Web interface**: Required for user access

### System Monitoring
- **Status reporting**: Signal strength and connection status
- **Error detection**: Connection failure detection
- **Performance metrics**: Connection quality and timing
- **Recovery coordination**: Works with upload recovery logic

### Configuration System
- **Credential management**: Secure password storage
- **Network settings**: SSID, hostname, power management
- **Validation**: Network parameter validation
- **Defaults**: Reasonable default configurations

## Error Handling

### Connection Failures
```cpp
enum WifiError {
    ERROR_NONE = 0,
    ERROR_CREDENTIALS,     // Wrong password
    ERROR_NOT_FOUND,       // SSID not found
    ERROR_DHCP_FAILED,     // No IP address
    ERROR_TIMEOUT,         // Connection timeout
    ERROR_HARDWARE         // WiFi hardware failure
};
```

### Recovery Strategies
1. **Immediate retry**: For transient failures
2. **Backoff retry**: Exponential delay for persistent issues
3. **WiFi reset**: Hardware reset of WiFi subsystem
4. **Manual intervention**: User action required for credential issues

### Logging and Diagnostics
- **Connection events**: All connection state changes
- **Error details**: Specific failure reasons
- **Performance metrics**: Connection timing and quality
- **Recovery actions**: All recovery attempts logged

## Security Considerations

### Credential Protection
- **Secure storage**: Passwords stored in ESP32 flash
- **Memory protection**: Clear passwords from RAM after use
- **No plaintext logging**: Passwords never logged in plaintext
- **WPA3 support**: Uses strongest available security

### Network Security
- **Enterprise support**: WPA2-Enterprise compatible
- **Certificate validation**: For HTTPS connections
- **No open networks**: Requires password-protected networks
- **Isolation**: No WiFi client mode for security

## Configuration Examples

### Basic Setup
```ini
WIFI_SSID = HomeNetwork
WIFI_PASSWORD = mypassword
HOSTNAME = cpap-uploader
```

### Power Optimized
```ini
WIFI_SSID = HomeNetwork
WIFI_PASSWORD = mypassword
HOSTNAME = cpap-uploader
WIFI_TX_PWR = low
WIFI_PWR_SAVING = max
```

### High Performance
```ini
WIFI_SSID = HomeNetwork
WIFI_PASSWORD = mypassword
HOSTNAME = cpap-uploader
WIFI_TX_PWR = high
WIFI_PWR_SAVING = none
```

## Troubleshooting

### Common Issues
- **Connection failures**: Check SSID/password, signal strength
- **Intermittent drops**: Check interference, distance from router
- **Slow performance**: Check channel congestion, QoS settings
- **mDNS issues**: Check network supports multicast

### Diagnostic Tools
- **Status endpoint**: `/status` shows connection details
- **Signal monitoring**: RSSI strength in web interface
- **Log analysis**: Detailed connection logging
- **Network scanning**: Available networks (development mode)
