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
    POWER_LOW,     // 5.0 dBm — minimum practical, router must be very close
    POWER_MID,     // 8.5 dBm — default, good for typical bedroom placement
    POWER_HIGH,    // 11.0 dBm — router in adjacent room or through walls
    POWER_MAX      // 19.5 dBm — maximum power, only if other settings fail
};

enum class WifiPowerSaving {
    SAVE_NONE,     // No power saving — maximum responsiveness, highest power
    SAVE_MID,      // WIFI_PS_MIN_MODEM — default, wakes every DTIM for broadcasts
    SAVE_MAX       // WIFI_PS_MAX_MODEM — maximum WiFi savings, may miss mDNS queries
};
```

### Protocol Restriction
802.11b (DSSS) is disabled at connection time via `esp_wifi_set_protocol()`. Only 802.11g/n (OFDM) is allowed. This caps peak TX current to ~205-250 mA (down from 370 mA with 802.11b).

### Compile-Time Power Caps
- `CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=11` in `sdkconfig.defaults` caps PHY-level TX power at 11 dBm even before runtime code runs
- `CONFIG_BT_ENABLED=n` disables Bluetooth at compile time (WiFi-only firmware)

### Dynamic Frequency Scaling (DFS)
With `CONFIG_PM_ENABLE=y`, the ESP-IDF power management framework enables automatic CPU frequency scaling. After WiFi connects, `esp_pm_configure()` is called with max=160 MHz, min=80 MHz. The WiFi driver automatically holds a PM lock during active operations.

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
bool connectStation(ssid, password) {
    WiFi.mode(WIFI_STA);
    
    // Disable 802.11b (DSSS) — caps peak TX current from 370 mA to ~250 mA
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    
    WiFi.begin(ssid, password);
    // Wait up to 15s for connection
}
```

### 2a. Early TX Power Application
`applyTxPowerEarly()` pre-initializes WiFi STA mode and sets TX power before `connectStation()`, preventing full-power spikes during the initial scan and association.

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

### Power Consumption (v0.11.0 defaults)
- **Peak TX** (802.11n @ 8.5 dBm): ~120-150 mA
- **Idle connected** (MIN_MODEM, 80 MHz): ~22-31 mA
- **Idle connected** (MIN_MODEM + DFS): ~20-25 mA
- **Boot pre-WiFi** (80 MHz, BT disabled): ~20-25 mA
- **During upload** (DFS boosts to 160 MHz): ~80-150 mA

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
