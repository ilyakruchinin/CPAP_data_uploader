# Logging System

## Overview
The Logging System (`Logger.cpp/.h`) provides structured logging capabilities for the CPAP uploader with configurable output destinations, log levels, and circular buffer management for web interface access.

## Architecture

### Log Destinations
- **Serial output**: Primary logging to USB serial (always enabled)
- **Circular buffer**: 8 KB static BSS array for web interface (not heap-allocated)
- **LittleFS storage**: Optional persisted logging (`/syslog.A.txt` / `/syslog.B.txt`, 64 KB each)
- **Pre-reboot dump**: `/last_reboot_log.txt` on LittleFS — written unconditionally before every `esp_restart()`
- **SD card emergency dump**: `/uploader_error.txt` on SD card — written on boot-time failures (config error, WiFi failure) where the user has no network access
- **SSE live stream**: `/api/logs/stream` endpoint pushes new log lines to the browser in real-time via Server-Sent Events

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
- **PERSISTENT_LOGS**: Enable persisted internal logging (default: false). Aliases: `SAVE_LOGS`, `LOG_TO_SD_CARD`
- **SERIAL_BAUD**: Serial baud rate (default: 115200)
- **LOG_BUFFER_SIZE**: Circular buffer size (default: 8192 bytes, compile-time via `-DLOG_BUFFER_SIZE=N`)

### Persistent Log Saving
```ini
# Enable persisted debug logging (use with caution)
PERSISTENT_LOGS = true

# Debugging-focused feature; enable only when needed
# Recommended only for scheduled mode outside therapy times
# Legacy aliases SAVE_LOGS and LOG_TO_SD_CARD are also accepted
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
    
    // Persist logs if enabled
    if (saveLogs && level <= LogLevel::LOG_INFO) {
        writeToStorage(timestamp, levelStr, buffer);
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

## Persistent Log Saving

When `PERSISTENT_LOGS=true`, the circular buffer is flushed to LittleFS every **30 seconds** using direct chunk-buffer writes (zero heap allocation). Files rotate at **64 KB**:

- `/syslog.A.txt` — active log file (appended to)
- `/syslog.B.txt` — rotated older log file
- Total capacity: **128 KB** (~30–60 minutes of continuous logging)

## Two-Tier Emergency Log Strategy

### Tier 1: Pre-Reboot Flush to LittleFS

Before every `esp_restart()`, the circular buffer is dumped to `/last_reboot_log.txt` on LittleFS **unconditionally** (regardless of `PERSISTENT_LOGS` setting). This ensures watchdog kills, state resets, soft reboots, and post-upload reboots always leave a diagnostic trail accessible via the Web UI on the next boot.

On next boot, if `/last_reboot_log.txt` exists, a warning is logged.

### Tier 2: Boot-Failure Dump to SD Card

When the device cannot establish WiFi (config failure, wrong credentials, hardware error), the Web UI is unreachable. The log buffer is dumped to `/uploader_error.txt` on the **SD card** so the user can pull the card and read it on a PC.

- **Trigger points**: config load failure (`main.cpp`), WiFi connection failure (`main.cpp`)
- **Format**: Appends with a header containing reason, uptime, firmware version, and free heap
- **Size cap**: 64 KB (truncates from beginning if exceeded)
- **Next-boot detection**: on successful boot, if `/uploader_error.txt` exists on SD, a warning is logged

## Integration Points

### System Components
- **main.cpp**: System initialization and state changes
- **FileUploader**: Upload progress and errors
- **WiFiManager**: Connection status and errors
- **UploadFSM**: State transitions and timing
- **All uploaders**: Backend-specific logging

### Web Interface
- **`/api/logs`**: Streams circular buffer contents (polling fallback)
- **`/api/logs/full`**: Streams NAND saved logs + previous reboot log + circular buffer (initial backfill)
- **`/api/logs/stream`**: SSE endpoint for real-time log push (single client)
- **`/api/logs/saved`**: Downloads persisted LittleFS log files as attachment
- **Log viewing**: `/logs` SPA tab — fetches backfill on first open, then SSE live stream with polling fallback

### Configuration
- **PERSISTENT_LOGS**: Optional persisted file-based logging (aliases: `SAVE_LOGS`, `LOG_TO_SD_CARD`)
- **Serial output**: Always enabled for debugging
- **Buffer size**: 8 KB static array (configurable at compile time via `LOG_BUFFER_SIZE`)

## Performance Considerations

### Memory Usage
- **Circular buffer**: 8 KB static BSS array (not heap — zero fragmentation)
- **Formatting buffer**: 256 bytes stack for message formatting
- **Flush chunk buffer**: 256 bytes stack for direct-to-file writes
- **SSE push buffer**: 320 bytes stack per push cycle
- **Net heap impact**: Negative — removes previous 2 KB malloc, eliminates up to 8 KB transient String fragmentation

### CPU Overhead
- **String formatting**: snprintf() for message formatting
- **Buffer management**: Circular index calculations
- **File I/O**: LittleFS write operations (optional)
- **Serial output**: UART transmission time

### Optimization Strategies
- **Conditional compilation**: Exclude debug logs in production
- **Level filtering**: Early exit for low-priority messages
- **Buffer sizing**: Tune for memory vs. history tradeoff
- **Batch writes**: Buffer persisted-log writes for efficiency

## Security Considerations

### Sensitive Data
- **Password censoring**: Automatic password redaction
- **Credential protection**: No logging of secure credentials
- **URL sanitization**: Remove passwords from logged URLs
- **Error messages**: Avoid exposing system internals

### Log File Protection
- **Access control**: Log files on internal LittleFS
- **Rotation**: Prevent unlimited log growth
- **Integrity**: Basic log file validation
- **Privacy**: No personal data in logs

## Troubleshooting

### Common Issues
- **Missing logs**: Check log level configuration
- **Persisted logging failures**: Verify LittleFS mount and write access
- **Buffer overflow**: Increase buffer size or reduce log verbosity
- **Serial issues**: Check baud rate and USB connection

### Debug Features
- **Log level testing**: Verify level filtering works
- **Buffer inspection**: Check circular buffer contents
- **File verification**: Verify LittleFS log file creation
- **Performance monitoring**: Measure logging overhead

## Configuration Examples

### Production Settings
```ini
PERSISTENT_LOGS = false
```

### Debug Settings
```ini
PERSISTENT_LOGS = true
DEBUG = true
```

## Files on Filesystem

| File | Location | Purpose | When Written |
|:-----|:---------|:--------|:-------------|
| `/syslog.A.txt` | LittleFS | Active persisted log | Every 30s when `PERSISTENT_LOGS=true` |
| `/syslog.B.txt` | LittleFS | Rotated older log | When A exceeds 64 KB |
| `/last_reboot_log.txt` | LittleFS | Pre-reboot buffer dump | Before every `esp_restart()` |
| `/uploader_error.txt` | SD Card | Boot-failure emergency log | Config or WiFi failure in `setup()` |
