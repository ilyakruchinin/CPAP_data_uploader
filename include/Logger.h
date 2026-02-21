#ifndef LOGGER_H
#define LOGGER_H

#ifndef ARDUINO_H
#include <Arduino.h>
#endif

#ifndef UNIT_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <FS.h>
#include <SPIFFS.h>
#else
// Mock FreeRTOS types for native testing
typedef void* SemaphoreHandle_t;
#endif

// Compile-time configuration for circular buffer size
#ifndef LOG_BUFFER_SIZE
#define LOG_BUFFER_SIZE 2048  // Default: 2KB
#endif

// Validate buffer size at compile time
static_assert(LOG_BUFFER_SIZE > 0, "LOG_BUFFER_SIZE must be greater than zero");

/**
 * Logger - Singleton class for dual-output logging system
 * 
 * Provides thread-safe logging to both serial interface and circular RAM buffer.
 * Designed for ESP32 dual-core operation with FreeRTOS mutex protection.
 * 
 * Features:
 * - Dual output: Serial + Circular Buffer (and SPIFFS for persistence)
 * - Thread-safe for dual-core ESP32
 * - Automatic buffer overflow handling (overwrites oldest data)
 * - Lost data tracking for buffer overflow scenarios
 * - Configurable buffer size via LOG_BUFFER_SIZE preprocessor definition
 * 
 * Memory Impact:
 * - Buffer: LOG_BUFFER_SIZE bytes (default 2KB)
 * - Overhead: ~32 bytes for state + mutex handle
 * 
 * Configuration:
 * To change buffer size, add to platformio.ini build_flags:
 *   build_flags = -DLOG_BUFFER_SIZE=4096  ; 4KB buffer
 */
class Logger {
public:
    /**
     * Structure returned by retrieveLogs() containing log data and metadata
     */
    struct LogData {
        String content;        // Log content from buffer
        uint32_t bytesLost;    // Number of bytes lost due to overflow since last read
    };

    /**
     * Get singleton instance of Logger
     * Thread-safe initialization on first call
     */
    static Logger& getInstance();

    /**
     * Log a C-string message to both serial and buffer
     * Thread-safe for concurrent calls from multiple cores
     */
    void log(const char* message);

    /**
     * Log an Arduino String message to both serial and buffer
     * Thread-safe for concurrent calls from multiple cores
     */
    void log(const String& message);

    /**
     * Log a formatted message (printf-style) to both serial and buffer
     * Thread-safe for concurrent calls from multiple cores
     * 
     * Example: logf("WiFi connected, IP: %s", ipAddress.c_str());
     */
    void logf(const char* format, ...);

    /**
     * Retrieve all logs from circular buffer
     * Returns log content and count of bytes lost due to overflow
     * Thread-safe for concurrent access
     * 
     * Always returns all available logs in the circular buffer from oldest to newest.
     * The buffer is never cleared - logs are retained until overwritten by new data.
     * This ensures consistent log retrieval and prevents out-of-order issues.
     * 
     * Returns:
     * - All logs currently in the buffer (from tail to head)
     * - Count of bytes lost due to overflow since buffer creation
     * - Logs are always returned in chronological order (oldest first)
     */
    LogData retrieveLogs();

    /**
     * Print all logs to a Print destination (e.g., Serial or WebServer)
     * This streams the full history from SPIFFS (syslog.old.txt + syslog.txt)
     * and finally any unflushed logs in the RAM buffer.
     * Thread-safe.
     * 
     * @param output The Print destination to write logs to
     * @return Number of bytes written
     */
    size_t printLogs(Print& output);

    /**
     * Print only the newest tail of logs to a Print destination.
     * Useful for web polling paths where full-buffer dumps are too expensive.
     * Pulls exclusively from the fast RAM buffer.
     * Thread-safe.
     *
     * @param output The Print destination to write logs to
     * @param maxBytes Maximum number of newest bytes to print
     * @return Number of bytes written
     */
    size_t printLogsTail(Print& output, size_t maxBytes);

    /**
     * Enable or disable SPIFFS logging persistence
     * 
     * When enabled, logs are dumped to SPIFFS periodically (e.g. every 10s)
     * by calling flushSpiffsBuffer() from the main loop. Uses an A/B rotation
     * strategy bounded to 40KB total to prevent flash exhaustion.
     * 
     * @param enable True to enable SPIFFS logging, false to disable
     */
    void enableSpiffsLogging(bool enable);
    
    /**
     * Periodic SPIFFS log dump (call from main loop every 10 seconds)
     * Only dumps if there are new logs since last dump.
     * 
     * @return true if logs were dumped, false if skipped or failed
     */
    bool flushSpiffsBuffer();

    /**
     * Dump a critical error directly to the logs and flush to SPIFFS
     * 
     * @param reason Description of the critical error
     * @return true if logs were successfully flushed, false otherwise
     */
    bool dumpCriticalLogToSpiffs(const String& reason);

    /**
     * Check if logger is properly initialized
     * Returns false if memory allocation or mutex creation failed
     */
    bool isInitialized() const { return initialized; }

protected:
    // Protected constructor for testing - allows inheritance in test code
    Logger();
    
    // Virtual destructor for proper cleanup in derived classes
    virtual ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /**
     * Get current timestamp as formatted string
     * Returns [HH:MM:SS] format or [--:--:--] if time not synced
     * Virtual to allow mocking in tests
     */
    virtual String getTimestamp();

    /**
     * Write data to serial interface
     * Called outside critical section for optimal performance
     * Virtual to allow mocking in tests
     */
    virtual void writeToSerial(const char* data, size_t len);

    /**
     * Write data to circular buffer with overflow handling
     * Must be called within mutex protection
     * NOT virtual - this is what we're testing!
     */
    void writeToBuffer(const char* data, size_t len);

    /**
     * Write data to SPIFFS log file
     * Virtual to allow mocking in tests
     */
    virtual void writeToSpiffs(const char* data, size_t len);

    /**
     * Track bytes lost due to buffer overflow
     * Called when data is overwritten in the circular buffer
     * Virtual to allow mocking in tests
     */
    virtual void trackLostBytes(uint32_t bytesLost);

    // Circular buffer storage
    char* buffer;
    size_t bufferSize;

    // Monotonic 32-bit indices for circular buffer management
    // These wrap at 2^32 but use modulo arithmetic for physical position
    volatile uint32_t headIndex;      // Next write position (monotonic counter)
    volatile uint32_t tailIndex;      // Oldest valid data position (monotonic counter)
    volatile uint32_t totalBytesLost; // Total bytes lost due to overflow since creation

    // Thread safety for dual-core ESP32
    SemaphoreHandle_t mutex;

    // Initialization state
    bool initialized;

    // SPIFFS logging
    bool spiffsLoggingEnabled;
    String logFileName;
    String logFileNameOld;
    
    // Periodic SPIFFS dump tracking
    volatile uint32_t lastDumpedBytes;  // Track bytes already dumped to SPIFFS
};

// Runtime debug mode flag — set from config DEBUG=true after config load.
// Controls: [res fh= ma= fd=] suffix on every log line, and verbose pre-flight
// scan output in FileUploader. Declared here so Logger.cpp and callers can access it.
extern bool g_debugMode;

// Convenience macros for logging

/**
 * Basic logging macro - logs message to both serial and buffer with INFO level
 * Usage: LOG("System started");
 */
#define LOG(msg) Logger::getInstance().log("[INFO] " msg)

/**
 * Printf-style logging macro with format string and INFO level
 * Usage: LOGF("Temperature: %d°C", temp);
 */
#define LOGF(fmt, ...) Logger::getInstance().logf("[INFO] " fmt, ##__VA_ARGS__)

/**
 * Level-based logging macros for structured logging
 * These add severity prefixes to messages
 */
#define LOG_INFO(msg) Logger::getInstance().log("[INFO] " msg)
#define LOG_ERROR(msg) Logger::getInstance().log("[ERROR] " msg)
#define LOG_WARN(msg) Logger::getInstance().log("[WARN] " msg)

/**
 * Printf-style level-based logging macros
 * Usage: LOG_INFOF("Connected to %s", ssid);
 */
#define LOG_INFOF(fmt, ...) Logger::getInstance().logf("[INFO] " fmt, ##__VA_ARGS__)
#define LOG_ERRORF(fmt, ...) Logger::getInstance().logf("[ERROR] " fmt, ##__VA_ARGS__)
#define LOG_WARNF(fmt, ...) Logger::getInstance().logf("[WARN] " fmt, ##__VA_ARGS__)

/**
 * Debug logging macros - compiled out unless ENABLE_VERBOSE_LOGGING is defined
 * These are for detailed diagnostics, progress updates, and troubleshooting information
 * that are useful during development but add overhead in production.
 * 
 * To enable: Add -DENABLE_VERBOSE_LOGGING to build_flags in platformio.ini
 * 
 * Usage:
 *   LOG_DEBUG("Detailed operation info");
 *   LOG_DEBUGF("Processing file: %s", filename);
 */
#ifdef ENABLE_VERBOSE_LOGGING
    #define LOG_DEBUG(msg) Logger::getInstance().log("[DEBUG] " msg)
    #define LOG_DEBUGF(fmt, ...) Logger::getInstance().logf("[DEBUG] " fmt, ##__VA_ARGS__)
#else
    // Compile out debug logging - zero overhead
    #define LOG_DEBUG(msg) ((void)0)
    #define LOG_DEBUGF(fmt, ...) ((void)0)
#endif

#endif // LOGGER_H
