#include "Logger.h"

#ifndef UNIT_TEST
#include "SDCardManager.h"
#include <WiFi.h>

#ifdef ENABLE_LOG_RESOURCE_SUFFIX
#include <esp_err.h>
#include <esp_vfs.h>
#include <errno.h>
#include <fcntl.h>
#endif
#endif

#include <stdarg.h>
#include <time.h>

#ifndef UNIT_TEST
#ifdef ENABLE_LOG_RESOURCE_SUFFIX
namespace {

int getFreeFileDescriptorCount() {
    int minFd = -1;
    int maxFd = -1;

    if (esp_vfs_get_fd_range(&minFd, &maxFd) != ESP_OK ||
        minFd < 0 ||
        maxFd < minFd) {
        return -1;
    }

    int openCount = 0;
    for (int fd = minFd; fd <= maxFd; ++fd) {
        errno = 0;
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
            openCount++;
        }
    }

    return (maxFd - minFd + 1) - openCount;
}

}  // namespace
#endif
#endif

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
    , lastDumpedBytes(0)
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

#ifndef UNIT_TEST
#ifdef ENABLE_LOG_RESOURCE_SUFFIX
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
    const int freeFdCount = getFreeFileDescriptorCount();

    timestampedMsg += " [res fh=" + String(freeHeap) +
                      " ma=" + String(maxAllocHeap) +
                      " fd=" + (freeFdCount >= 0 ? String(freeFdCount) : String("?")) +
                      "]";
#endif
#endif

    const char* finalMsg = timestampedMsg.c_str();
    size_t len = timestampedMsg.length();
    
    // Write to serial (outside critical section - Serial is thread-safe on ESP32)
    writeToSerial(finalMsg, len);
    
    // Write to buffer if initialized
    if (initialized && buffer != nullptr) {
        writeToBuffer(finalMsg, len);
    }
    
    // Note: SD card logging is now handled by periodic dump task
    // See dumpLogsToSDCardPeriodic() which should be called from main loop
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
        // Check if buffer is full BEFORE writing
        // This ensures tail is advanced before head overwrites it
        if (headIndex - tailIndex >= bufferSize) {
            // Buffer is full - advance tail to make room
            tailIndex++;
            totalBytesLost++;
        }
        
        // Calculate physical position in buffer
        size_t physicalPos = headIndex % bufferSize;
        
        // Write byte to buffer
        buffer[physicalPos] = data[i];
        
        // Advance head index (monotonic counter)
        headIndex++;
    }

    // Add newline if not already present
    if (len > 0 && data[len - 1] != '\n') {
        // Check if buffer is full BEFORE writing newline
        if (headIndex - tailIndex >= bufferSize) {
            tailIndex++;
            totalBytesLost++;
        }
        
        size_t physicalPos = headIndex % bufferSize;
        buffer[physicalPos] = '\n';
        headIndex++;
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

    // Find the start of the first complete log line
    // After buffer overflow, tailIndex might point to the middle of a line
    // Scan forward to find the first '[' character (start of timestamp)
    uint32_t startOffset = 0;
    bool foundStart = false;
    
    // Only scan if we've lost bytes (indicating buffer overflow occurred)
    if (totalBytesLost > 0 && availableBytes > 0) {
        // Scan up to 100 bytes to find start of line
        for (uint32_t i = 0; i < availableBytes && i < 100; i++) {
            uint32_t logicalIndex = tailIndex + i;
            size_t physicalPos = logicalIndex % bufferSize;
            
            if (buffer[physicalPos] == '[') {
                // Found start of a log line
                startOffset = i;
                foundStart = true;
                break;
            }
        }
        
        // If we found a start, skip the corrupted partial line
        if (foundStart && startOffset > 0) {
            // Update bytes lost to include the skipped partial line
            result.bytesLost += startOffset;
        }
    }

    // Read data from tail to head (oldest to newest)
    // This ensures chronological order even after buffer wraps
    for (uint32_t i = startOffset; i < availableBytes; i++) {
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

// Print all logs to a Print destination without intermediate String allocation
size_t Logger::printLogs(Print& output) {
    if (!initialized || buffer == nullptr || mutex == nullptr) {
        return 0;
    }

    // Acquire mutex for thread-safe buffer access
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    size_t bytesWritten = 0;

    // Calculate available data to read (from tail to head)
    uint32_t availableBytes = headIndex - tailIndex;
    
    if (availableBytes > bufferSize) {
        availableBytes = bufferSize;
    }

    // Find start of first valid line if we had overflow
    uint32_t startOffset = 0;
    if (totalBytesLost > 0 && availableBytes > 0) {
        for (uint32_t i = 0; i < availableBytes && i < 100; i++) {
            uint32_t logicalIndex = tailIndex + i;
            size_t physicalPos = logicalIndex % bufferSize;
            if (buffer[physicalPos] == '[') {
                startOffset = i;
                break;
            }
        }
    }

    // Release mutex temporarily to avoid holding it during slow I/O
    // We make a copy of indices
    uint32_t currentTail = tailIndex + startOffset;
    uint32_t currentHead = headIndex;
    xSemaphoreGive(mutex);

    char chunk[128];
    uint32_t pos = currentTail;
    
    while (pos < currentHead) {
        // Re-acquire mutex to read a chunk safely
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            break;
        }
        
        // Check if our reader position has been overtaken by writer (buffer wrap)
        if (tailIndex > pos) {
            pos = tailIndex; // Skip lost data
        }
        
        size_t bytesToRead = sizeof(chunk);
        if (currentHead - pos < bytesToRead) {
            bytesToRead = currentHead - pos;
        }
        
        if (bytesToRead == 0) {
            xSemaphoreGive(mutex);
            break;
        }
        
        for (size_t i = 0; i < bytesToRead; i++) {
            size_t physicalPos = (pos + i) % bufferSize;
            chunk[i] = buffer[physicalPos];
        }
        
        xSemaphoreGive(mutex);
        
        // Write chunk to output
        output.write((const uint8_t*)chunk, bytesToRead);
        bytesWritten += bytesToRead;
        pos += bytesToRead;
        
        // Yield to allow other tasks to run
        yield();
    }

    return bytesWritten;
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
        // Reset dump tracking when enabling
        lastDumpedBytes = 0;
        
        // Log a warning message about debugging use
        String warningMsg = getTimestamp() + "[WARN] SD card logging enabled - DEBUGGING ONLY - Logs will be dumped periodically\n";
        writeToSerial(warningMsg.c_str(), warningMsg.length());
        if (initialized && buffer != nullptr) {
            writeToBuffer(warningMsg.c_str(), warningMsg.length());
        }
    }
}

// Periodic SD card log dump (call from main loop every 10 seconds)
bool Logger::dumpLogsToSDCardPeriodic(SDCardManager* sdManager) {
#ifdef UNIT_TEST
    return false; // Not supported in unit tests
#else
    if (!sdCardLoggingEnabled || sdFileSystem == nullptr || sdManager == nullptr) {
        return false;
    }
    
    if (!initialized || buffer == nullptr || mutex == nullptr) {
        return false;
    }
    
    // Acquire mutex to safely read headIndex
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        // Failed to acquire mutex quickly - skip this dump
        return false;
    }
    
    // Check if there are new logs to dump
    uint32_t currentBytes = headIndex;
    uint32_t newBytes = currentBytes - lastDumpedBytes;
    
    // Release mutex immediately after reading
    xSemaphoreGive(mutex);
    
    // Skip if no new logs
    if (newBytes == 0) {
        return false;
    }
    
    // Try to take control of SD card (non-blocking)
    if (!sdManager->takeControl()) {
        // SD card in use by CPAP - skip this dump
        return false;
    }
    
    // Calculate how much data to dump
    // We need to read from lastDumpedBytes to currentBytes
    // But we need to be careful about buffer wrapping
    
    // Acquire mutex again for reading buffer content
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        sdManager->releaseControl();
        return false;
    }
    
    // Calculate available data in buffer
    uint32_t availableBytes = headIndex - tailIndex;
    
    // Check if we've lost data since last dump
    if (lastDumpedBytes < tailIndex) {
        // Some logs were overwritten before we could dump them
        // Start from tail (oldest available data)
        lastDumpedBytes = tailIndex;
    }
    
    // Calculate how much we can actually dump
    uint32_t bytesToDump = headIndex - lastDumpedBytes;
    
    // Safety check
    if (bytesToDump > bufferSize) {
        bytesToDump = bufferSize;
        lastDumpedBytes = headIndex - bufferSize;
    }
    
    // Build string with new log data
    String logContent;
    logContent.reserve(bytesToDump);
    
    for (uint32_t i = 0; i < bytesToDump; i++) {
        uint32_t logicalIndex = lastDumpedBytes + i;
        size_t physicalPos = logicalIndex % bufferSize;
        logContent += buffer[physicalPos];
    }
    
    // Update lastDumpedBytes before releasing mutex
    lastDumpedBytes = headIndex;
    
    // Release mutex
    xSemaphoreGive(mutex);
    
    // Write to SD card
    File logFile = sdFileSystem->open(logFileName, FILE_APPEND);
    if (!logFile) {
        // Try to create file if it doesn't exist
        logFile = sdFileSystem->open(logFileName, FILE_WRITE);
        if (!logFile) {
            sdManager->releaseControl();
            return false;
        }
    }
    
    // Write log content
    logFile.print(logContent);
    logFile.close();
    
    // Release SD card
    sdManager->releaseControl();
    
    return true;
#endif
}

// Write data to SD card log file (debugging only) - DEPRECATED
// This method is no longer used - SD logging is now handled by dumpLogsToSDCardPeriodic()
void Logger::writeToSdCard(const char* data, size_t len) {
    // This method is deprecated and no longer used
    // SD card logging is now handled by periodic dump task
    // Kept for interface compatibility
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