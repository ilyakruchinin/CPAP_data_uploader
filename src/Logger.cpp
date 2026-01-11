#include "Logger.h"
#include "SDCardManager.h"
#include <stdarg.h>
#include <time.h>
#include <WiFi.h>

// Singleton instance getter
Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

// Constructor - initialize the logging system
Logger::Logger() 
    : buffer(nullptr)
    , bufferSize(LOG_BUFFER_SIZE)
    , headIndex(0)
    , tailIndex(0)
    , totalBytesLost(0)
    , mutex(nullptr)
    , initialized(false)
    , sdCardLoggingEnabled(false)
    , sdFileSystem(nullptr)
    , logFileName("/debug_log.txt")
{
    // Allocate circular buffer
    buffer = (char*)malloc(bufferSize);
    if (buffer == nullptr) {
        // Memory allocation failed - fall back to serial-only mode
        Serial.println("[LOGGER] ERROR: Failed to allocate circular buffer, falling back to serial-only mode");
        return;
    }

    // Initialize buffer to zeros for cleaner debugging
    memset(buffer, 0, bufferSize);

    // Create FreeRTOS mutex for thread-safe operations
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        // Mutex creation failed - fall back to serial-only mode
        Serial.println("[LOGGER] ERROR: Failed to create mutex, falling back to serial-only mode");
        free(buffer);
        buffer = nullptr;
        return;
    }

    // Initialization successful
    initialized = true;
    #ifdef ENABLE_VERBOSE_LOGGING
    Serial.println("[LOGGER] Initialized successfully with " + String(bufferSize) + " byte circular buffer");
    #endif
}

// Destructor - clean up resources
Logger::~Logger() {
    if (mutex != nullptr) {
        vSemaphoreDelete(mutex);
        mutex = nullptr;
    }
    
    if (buffer != nullptr) {
        free(buffer);
        buffer = nullptr;
    }
}

// Get current timestamp as string (HH:MM:SS format)
String Logger::getTimestamp() {
    time_t now = time(nullptr);
    
    // Check if time is synchronized (timestamp > Jan 1, 2000)
    if (now < 946684800) {
        return "[--:--:--] ";
    }
    
    struct tm timeinfo;
    if (!localtime_r(&now, &timeinfo)) {
        return "[--:--:--] ";
    }
    
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "[%02d:%02d:%02d] ", 
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    return String(buffer);
}

// Log a C-string message
void Logger::log(const char* message) {
    if (message == nullptr) {
        return;
    }

    // Prepend timestamp
    String timestampedMsg = getTimestamp() + String(message);
    const char* finalMsg = timestampedMsg.c_str();
    size_t len = timestampedMsg.length();
    
    // Write to serial (outside critical section - Serial is thread-safe on ESP32)
    writeToSerial(finalMsg, len);
    
    // Write to buffer if initialized
    if (initialized && buffer != nullptr) {
        writeToBuffer(finalMsg, len);
    }
    
    // Write to SD card if enabled (debugging only)
    if (sdCardLoggingEnabled && sdFileSystem != nullptr) {
        writeToSdCard(finalMsg, len);
    }
}

// Log an Arduino String message
void Logger::log(const String& message) {
    log(message.c_str());
}

// Log a formatted message (printf-style)
void Logger::logf(const char* format, ...) {
    if (format == nullptr) {
        return;
    }

    // Format the message using vsnprintf
    char buffer[256];  // Temporary buffer for formatted message
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    // Check if message was truncated
    if (len >= (int)sizeof(buffer)) {
        len = sizeof(buffer) - 1;
    }

    // Log the formatted message
    if (len > 0) {
        log(buffer);
    }
}

// Write data to serial interface
void Logger::writeToSerial(const char* data, size_t len) {
    // Serial.write is thread-safe on ESP32
    Serial.write((const uint8_t*)data, len);
    
    // Add newline if not already present
    if (len > 0 && data[len - 1] != '\n') {
        Serial.write('\n');
    }
}

// Write data to circular buffer with overflow handling
void Logger::writeToBuffer(const char* data, size_t len) {
    if (!initialized || buffer == nullptr || mutex == nullptr) {
        return;
    }

    // Acquire mutex for thread-safe buffer access
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        // Failed to acquire mutex - skip buffer write
        return;
    }

    // Write each byte to the circular buffer
    for (size_t i = 0; i < len; i++) {
        // Calculate physical position in buffer
        size_t physicalPos = headIndex % bufferSize;
        
        // Write byte to buffer
        buffer[physicalPos] = data[i];
        
        // Advance head index (monotonic counter)
        headIndex++;
        
        // Check if we've wrapped around and need to advance tail
        // This happens when we've written more than bufferSize bytes
        if (headIndex - tailIndex > bufferSize) {
            // Calculate how many bytes we're about to lose
            uint32_t bytesToLose = (headIndex - tailIndex) - bufferSize;
            totalBytesLost += bytesToLose;
            
            // Move tail to maintain buffer size constraint
            tailIndex = headIndex - bufferSize;
        }
    }

    // Add newline if not already present
    if (len > 0 && data[len - 1] != '\n') {
        size_t physicalPos = headIndex % bufferSize;
        buffer[physicalPos] = '\n';
        headIndex++;
        
        if (headIndex - tailIndex > bufferSize) {
            uint32_t bytesToLose = (headIndex - tailIndex) - bufferSize;
            totalBytesLost += bytesToLose;
            tailIndex = headIndex - bufferSize;
        }
    }

    // Release mutex
    xSemaphoreGive(mutex);
}

// Track bytes lost due to buffer overflow
void Logger::trackLostBytes(uint32_t bytesLost) {
    // This method is now handled directly in writeToBuffer
    // Kept for interface compatibility if needed
}

// Retrieve all logs from buffer
Logger::LogData Logger::retrieveLogs() {
    LogData result;
    result.bytesLost = 0;

    if (!initialized || buffer == nullptr || mutex == nullptr) {
        // Not initialized - return empty result
        return result;
    }

    // Acquire mutex for thread-safe buffer access
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        // Failed to acquire mutex - return empty result
        return result;
    }

    // Return total bytes lost since buffer creation
    result.bytesLost = totalBytesLost;

    // Calculate available data to read (from tail to head)
    uint32_t availableBytes = headIndex - tailIndex;
    
    // Safety check - should never exceed buffer size due to our overflow handling
    if (availableBytes > bufferSize) {
        // This should not happen with proper overflow handling, but be defensive
        availableBytes = bufferSize;
        tailIndex = headIndex - bufferSize;
    }

    // Reserve space in String to avoid multiple reallocations
    result.content.reserve(availableBytes);

    // Read data from tail to head (oldest to newest)
    // This ensures chronological order even after buffer wraps
    for (uint32_t i = 0; i < availableBytes; i++) {
        uint32_t logicalIndex = tailIndex + i;
        size_t physicalPos = logicalIndex % bufferSize;
        result.content += buffer[physicalPos];
    }

    // Never clear the buffer - logs are retained until overwritten
    // This eliminates the complexity of tracking read positions and
    // ensures consistent behavior regardless of call frequency

    // Release mutex
    xSemaphoreGive(mutex);

    return result;
}
// Enable or disable SD card logging (debugging only)
void Logger::enableSdCardLogging(bool enable, fs::FS* sdFS) {
    if (enable && sdFS == nullptr) {
        // Cannot enable without valid filesystem
        return;
    }
    
    sdCardLoggingEnabled = enable;
    sdFileSystem = enable ? sdFS : nullptr;
    
    if (enable) {
        // Log a warning message about debugging use
        String warningMsg = getTimestamp() + "[WARN] SD card logging enabled - DEBUGGING ONLY - May cause SD access conflicts\n";
        writeToSerial(warningMsg.c_str(), warningMsg.length());
        if (initialized && buffer != nullptr) {
            writeToBuffer(warningMsg.c_str(), warningMsg.length());
        }
    }
}

// Write data to SD card log file (debugging only)
void Logger::writeToSdCard(const char* data, size_t len) {
#ifndef UNIT_TEST
    if (!sdCardLoggingEnabled || sdFileSystem == nullptr) {
        return;
    }
    
    // Try to open/create log file in append mode
    File logFile = sdFileSystem->open(logFileName, FILE_APPEND);
    if (!logFile) {
        // If file doesn't exist, try to create it
        logFile = sdFileSystem->open(logFileName, FILE_WRITE);
        if (!logFile) {
            // Failed to create/open file - disable SD logging to prevent spam
            sdCardLoggingEnabled = false;
            return;
        }
    }
    
    // Write data to file
    logFile.write((const uint8_t*)data, len);
    
    // Ensure newline is present
    if (len > 0 && data[len - 1] != '\n') {
        logFile.write('\n');
    }
    
    // Close file to ensure data is written
    logFile.close();
#endif
}
// Dump current logs to SD card for critical failures
bool Logger::dumpLogsToSDCard(const String& reason) {
#ifdef UNIT_TEST
    return false; // Not supported in unit tests
#else
    // Create a temporary SD card manager to dump logs
    SDCardManager tempSDManager;
    
    if (!tempSDManager.begin()) {
        // Try to log error, but don't recurse into SD dump
        String errorMsg = getTimestamp() + "[ERROR] Failed to initialize SD card for log dump\n";
        writeToSerial(errorMsg.c_str(), errorMsg.length());
        if (initialized && buffer != nullptr) {
            writeToBuffer(errorMsg.c_str(), errorMsg.length());
        }
        return false;
    }
    
    if (!tempSDManager.takeControl()) {
        // Try to log error, but don't recurse into SD dump
        String errorMsg = getTimestamp() + "[ERROR] Cannot dump logs - SD card in use by CPAP\n";
        writeToSerial(errorMsg.c_str(), errorMsg.length());
        if (initialized && buffer != nullptr) {
            writeToBuffer(errorMsg.c_str(), errorMsg.length());
        }
        return false;
    }
    
    // Get current logs from logger
    LogData logData = retrieveLogs();
    
    if (logData.content.isEmpty()) {
        String warnMsg = getTimestamp() + "[WARN] No logs available to dump\n";
        writeToSerial(warnMsg.c_str(), warnMsg.length());
        if (initialized && buffer != nullptr) {
            writeToBuffer(warnMsg.c_str(), warnMsg.length());
        }
        tempSDManager.releaseControl();
        return false;
    }
    
    // Use the existing logFileName for debug logs
    String filename = logFileName;
    
    // Write logs to SD card (append mode to preserve previous dumps)
    File logFile = tempSDManager.getFS().open(filename, FILE_APPEND);
    if (!logFile) {
        // If file doesn't exist, create it
        logFile = tempSDManager.getFS().open(filename, FILE_WRITE);
        if (!logFile) {
            String errorMsg = getTimestamp() + "[ERROR] Failed to create log dump file: " + filename + "\n";
            writeToSerial(errorMsg.c_str(), errorMsg.length());
            if (initialized && buffer != nullptr) {
                writeToBuffer(errorMsg.c_str(), errorMsg.length());
            }
            tempSDManager.releaseControl();
            return false;
        }
    }
    
    // Write separator and reason header
    logFile.println("\n===== REASON: " + reason + " =====");
    logFile.println("Timestamp: " + String(millis()) + "ms");
    logFile.println("Free heap: " + String(ESP.getFreeHeap()) + " bytes");
    logFile.println("WiFi status: " + String(WiFi.status()));
    if (logData.bytesLost > 0) {
        logFile.println("WARNING: " + String(logData.bytesLost) + " bytes of logs were lost due to buffer overflow");
    }
    logFile.println("Buffer retention: Logs always retained in circular buffer");
    logFile.println("=== Log Content ===");
    
    // Write log content
    logFile.print(logData.content);
    
    // Write end separator
    logFile.println("=== End of Log Dump ===\n");
    
    logFile.close();
    tempSDManager.releaseControl();
    
    // Log success message
    String successMsg = getTimestamp() + "[INFO] Debug logs dumped to SD card: " + filename + " (reason: " + reason + ")\n";
    writeToSerial(successMsg.c_str(), successMsg.length());
    if (initialized && buffer != nullptr) {
        writeToBuffer(successMsg.c_str(), successMsg.length());
    }
    
    return true;
#endif
}