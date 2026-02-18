# Logging System

## Overview
The Logging System (`Logger.cpp/.h`) provides structured logging capabilities for the CPAP uploader with configurable output destinations, log levels, and circular buffer management for web interface access.

## Architecture

### Log Destinations
- **Serial output**: Primary logging to USB serial
- **SD card**: Optional file logging (`/debug.log`)
- **Circular buffer**: In-memory buffer for web interface
- **Conditional compilation**: Feature flags for different builds

### Log Levels
```cpp
enum LogLevel {
    LOG_ERROR = 0,    // Critical errors, system failures
    LOG_WARN  = 1,    // Warnings, recoverable issues
    LOG_INFO  = 2,    // Information, normal operations
    LOG_DEBUG = 3     // Debug information, detailed tracing
};
```

## Core Features

### Structured Logging
```cpp
#define LOG_ERROR(fmt, ...) Logger::log(LogLevel::LOG_ERROR, "[ERROR] " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  Logger::log(LogLevel::LOG_WARN, "[WARN] " fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  Logger::log(LogLevel::LOG_INFO, "[INFO] " fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) Logger::log(LogLevel::LOG_DEBUG, "[DEBUG] " fmt, ##__VA_ARGS__)
```

### Circular Buffer Management
```cpp
class Logger {
private:
    static constexpr size_t BUFFER_SIZE = 8192;
    static constexpr size_t MAX_ENTRIES = 100;
    
    struct LogEntry {
        uint32_t timestamp;
        LogLevel level;
        char message[256];
    };
    
    static LogEntry logBuffer[MAX_ENTRIES];
    static size_t bufferHead;
    static size_t bufferCount;
};
```

## Configuration

### Log Settings
- **LOG_LEVEL**: Minimum log level to output (default: INFO)
- **LOG_TO_SD_CARD**: Enable SD card logging (default: false)
- **SERIAL_BAUD**: Serial baud rate (default: 115200)
- **BUFFER_SIZE**: Circular buffer size (8KB)

### SD Card Logging
```ini
# Enable debug logging to SD card (use with caution)
LOG_TO_SD_CARD = true

# Can interfere with CPAP SD card access
# Recommended only for scheduled mode outside therapy times
```

## Operations

### 1. Log Entry
```cpp
void log(LogLevel level, const char* format, ...) {
    // Format message with va_args
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    // Add timestamp and level prefix
    uint32_t timestamp = millis();
    const char* levelStr = getLevelString(level);
    
    // Output to serial
    if (level <= currentLogLevel) {
        Serial.printf("[%lu] %s %s\n", timestamp, levelStr, buffer);
    }
    
    // Add to circular buffer
    addToBuffer(timestamp, level, buffer);
    
    // Write to SD card if enabled
    if (logToSdCard && level <= LogLevel::LOG_INFO) {
        writeToSdCard(timestamp, levelStr, buffer);
    }
}
```

### 2. Circular Buffer Management
```cpp
void addToBuffer(uint32_t timestamp, LogLevel level, const char* message) {
    LogEntry& entry = logBuffer[bufferHead];
    entry.timestamp = timestamp;
    entry.level = level;
    strncpy(entry.message, message, sizeof(entry.message) - 1);
    entry.message[sizeof(entry.message) - 1] = '\0';
    
    bufferHead = (bufferHead + 1) % MAX_ENTRIES;
    if (bufferCount < MAX_ENTRIES) {
        bufferCount++;
    }
}
```

### 3. Web Interface Access
```cpp
String getRecentLogs(size_t maxBytes = 2048) {
    String output = "";
    size_t startIdx = (bufferHead + MAX_ENTRIES - bufferCount) % MAX_ENTRIES;
    
    for (size_t i = 0; i < bufferCount && output.length() < maxBytes; i++) {
        size_t idx = (startIdx + i) % MAX_ENTRIES;
        const LogEntry& entry = logBuffer[idx];
        
        output += String(entry.timestamp) + " " + 
                 getLevelString(entry.level) + " " + 
                 String(entry.message) + "\n";
    }
    
    return output;
}
```

## Advanced Features

### Formatted Logging
```cpp
// Formatted logging with type safety
template<typename... Args>
void logf(LogLevel level, const char* format, Args... args) {
    if (level <= currentLogLevel) {
        char buffer[256];
        snprintf(buffer, sizeof(buffer), format, args...);
        log(level, "%s", buffer);
    }
}

// Convenience macros
#define LOGF(level, fmt, ...) Logger::logf(level, fmt, ##__VA_ARGS__)
#define LOGF_ERROR(fmt, ...) LOGF(LogLevel::LOG_ERROR, fmt, ##__VA_ARGS__)
#define LOGF_WARN(fmt, ...)  LOGF(LogLevel::LOG_WARN, fmt, ##__VA_ARGS__)
#define LOGF_INFO(fmt, ...)  LOGF(LogLevel::LOG_INFO, fmt, ##__VA_ARGS__)
#define LOGF_DEBUG(fmt, ...) LOGF(LogLevel::LOG_DEBUG, fmt, ##__VA_ARGS__)
```

### Conditional Logging
```cpp
#ifdef DEBUG
    #define DEBUG_LOG(fmt, ...) LOG_DEBUG(fmt, ##__VA_ARGS__)
#else
    #define DEBUG_LOG(fmt, ...)
#endif

#ifdef VERBOSE_LOGGING
    #define VERBOSE_LOG(fmt, ...) LOG_INFO(fmt, ##__VA_ARGS__)
#else
    #define VERBOSE_LOG(fmt, ...)
#endif
```

### Performance Monitoring
```cpp
void logPerformance(const char* operation, uint32_t durationMs) {
    LOGF_INFO("[PERF] %s took %lu ms", operation, durationMs);
}

#define PERF_LOG(operation) \
    uint32_t start = millis(); \
    operation; \
    logPerformance(#operation, millis() - start);
```

## SD Card Logging

### File Management
```cpp
bool writeToSdCard(uint32_t timestamp, const char* level, const char* message) {
    File logFile = SD_MMC.open("/debug.log", FILE_APPEND);
    if (!logFile) {
        return false;
    }
    
    // Format: [timestamp] LEVEL message\n
    logFile.printf("[%lu] %s %s\n", timestamp, level, message);
    logFile.close();
    
    // Rotate log file if too large
    rotateIfNeeded();
    
    return true;
}
```

### Log Rotation
```cpp
void rotateIfNeeded() {
    File logFile = SD_MMC.open("/debug.log", FILE_READ);
    if (logFile && logFile.size() > MAX_LOG_SIZE) {
        logFile.close();
        
        // Rename current log
        SD_MMC.rename("/debug.log", "/debug.log.old");
        
        // Delete old log if exists
        SD_MMC.remove("/debug.log.old");
        
        LOG_INFO("[Logger] Log file rotated");
    }
}
```

## Integration Points

### System Components
- **main.cpp**: System initialization and state changes
- **FileUploader**: Upload progress and errors
- **WiFiManager**: Connection status and errors
- **UploadFSM**: State transitions and timing
- **All uploaders**: Backend-specific logging

### Web Interface
- **Log viewing**: `/logs` endpoint shows recent logs
- **Real-time updates**: Auto-refreshing log display
- **Filtering**: Filter by log level (future)
- **Export**: Download log files (future)

### Configuration
- **Log level**: Configurable minimum log level
- **SD logging**: Optional file-based logging
- **Serial output**: Always enabled for debugging
- **Buffer size**: Tunable for memory constraints

## Performance Considerations

### Memory Usage
- **Circular buffer**: 8KB for ~100 log entries
- **Formatting buffer**: 256 bytes for message formatting
- **SD logging**: Additional file handle overhead
- **Serial output**: Minimal memory impact

### CPU Overhead
- **String formatting**: snprintf() for message formatting
- **Buffer management**: Circular index calculations
- **File I/O**: SD card write operations (optional)
- **Serial output**: UART transmission time

### Optimization Strategies
- **Conditional compilation**: Exclude debug logs in production
- **Level filtering**: Early exit for low-priority messages
- **Buffer sizing**: Tune for memory vs. history tradeoff
- **Batch writes**: Buffer SD writes for efficiency

## Security Considerations

### Sensitive Data
- **Password censoring**: Automatic password redaction
- **Credential protection**: No logging of secure credentials
- **URL sanitization**: Remove passwords from logged URLs
- **Error messages**: Avoid exposing system internals

### Log File Protection
- **Access control**: Log files on SD card
- **Rotation**: Prevent unlimited log growth
- **Integrity**: Basic log file validation
- **Privacy**: No personal data in logs

## Troubleshooting

### Common Issues
- **Missing logs**: Check log level configuration
- **SD logging failures**: Verify card access permissions
- **Buffer overflow**: Increase buffer size or reduce log verbosity
- **Serial issues**: Check baud rate and USB connection

### Debug Features
- **Log level testing**: Verify level filtering works
- **Buffer inspection**: Check circular buffer contents
- **File verification**: Verify SD log file creation
- **Performance monitoring**: Measure logging overhead

## Configuration Examples

### Production Settings
```ini
LOG_LEVEL = INFO
LOG_TO_SD_CARD = false
```

### Debug Settings
```ini
LOG_LEVEL = DEBUG
LOG_TO_SD_CARD = true
```

### Minimal Settings
```ini
LOG_LEVEL = ERROR
LOG_TO_SD_CARD = false
```

## Future Enhancements

### Advanced Features
- **Log filtering**: Pattern-based log filtering
- **Remote logging**: Send logs to remote server
- **Compression**: Compress log files for storage
- **Encryption**: Encrypt sensitive log entries

### Web Interface
- **Live streaming**: WebSocket log streaming
- **Search functionality**: Search log history
- **Export options**: Download logs in various formats
- **Alert system**: Error notifications
