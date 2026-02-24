#ifndef LOGGER_H
#define LOGGER_H

#ifndef ARDUINO_H
#include <Arduino.h>
#endif

#ifndef UNIT_TEST
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <FS.h>
#else
// Mock FreeRTOS types for native testing
typedef void* SemaphoreHandle_t;
#endif

// Forward declaration for SDCardManager
class SDCardManager;

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
 * - Dual output: Serial + Circular Buffer
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
     * Writes directly from buffer to output without intermediate String allocation.
     * Thread-safe.
     * 
     * @param output The Print destination to write logs to
     * @return Number of bytes written
     */
    size_t printLogs(Print& output);

    /**
     * Print only the newest tail of logs to a Print destination.
     * Useful for web polling paths where full-buffer dumps are too expensive.
     * Thread-safe.
     *
     * @param output The Print destination to write logs to
     * @param maxBytes Maximum number of newest bytes to print
     * @return Number of bytes written
     */
    size_t printLogsTail(Print& output, size_t maxBytes);

    /**
     * Enable or disable persistent log saving.
     * 
     * Logs are saved to the provided filesystem (LittleFS in production) and
     * should only be enabled for troubleshooting.
     * 
     * When enabled, logs are flushed periodically (every 10 seconds)
     * by calling dumpSavedLogsPeriodic() from the main loop.
     * 
     * @param enable True to enable log saving, false to disable
     * @param logFS Pointer to filesystem used for persisted logs (required when enabling)
     */
    void enableLogSaving(bool enable, fs::FS* logFS = nullptr);
    
    /**
     * Periodic persisted-log flush (call from main loop every 10 seconds).
     * Only flushes if there are new logs since last dump.
     * 
     * @param sdManager Unused compatibility parameter
     * @return true if logs were flushed, false if skipped or failed
     */
    bool dumpSavedLogsPeriodic(class SDCardManager* sdManager);

    /**
     * Save current logs to persistent storage for critical failures.
     * 
     * @param reason Description of why logs are being saved (e.g., "wifi_connection_failed")
     * @return true if logs were successfully saved, false otherwise
     */
    bool dumpSavedLogs(const String& reason);

    /**
     * Dumps the current log buffer directly to a file on the provided filesystem.
     * This is useful for emergency boot failures (e.g. bad config) where the SD card
     * is the only way for the user to retrieve the failure reason.
     * @param fs The filesystem to write to (typically SD_MMC)
     * @param filename Absolute path to the file (e.g. "/uploader_error.txt")
     * @return true if successful
     */
    bool dumpToSD(fs::FS& fs, const char* filename);

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
     * Write data to persistent log storage (debugging only)
     * Virtual to allow mocking in tests
     */
    virtual void writeToStorage(const char* data, size_t len);

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

    // Persistent log saving (debugging only)
    bool logSavingEnabled;
    fs::FS* logFileSystem;
    String logFileName;
    
    // Periodic persisted-log tracking
    volatile uint32_t lastDumpedBytes;  // Track bytes already flushed
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
