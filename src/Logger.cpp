#include "Logger.h"

#ifndef UNIT_TEST
#include "SDCardManager.h"
#include <WiFi.h>
#include <LittleFS.h>

#ifdef ENABLE_LOG_RESOURCE_SUFFIX
#include <errno.h>
#include <fcntl.h>
#endif
#endif

#include <stdarg.h>
#include <time.h>
#include "version.h"

#ifndef UNIT_TEST
#ifdef ENABLE_LOG_RESOURCE_SUFFIX
namespace {

int getFreeFileDescriptorCount() {
    // ESP-IDF/Arduino doesn't expose a stable public API for fd range across all versions.
    // Use a conservative bounded scan for diagnostics.
    static constexpr int kFdScanLimit = 64;

    int openCount = 0;
    for (int fd = 0; fd < kFdScanLimit; ++fd) {
        errno = 0;
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
            openCount++;
        }
    }

    int freeCount = kFdScanLimit - openCount;
    return freeCount >= 0 ? freeCount : 0;
}

}  // namespace
#endif
#endif

// Static circular buffer — lives in BSS, not heap
char Logger::s_logBuffer[LOG_BUFFER_SIZE];

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
    , logSavingEnabled(false)
    , logFileSystem(nullptr)
    , logFileName("/debug_log.txt")
    , lastDumpedBytes(0)
{
    // Use static BSS buffer (zero heap allocation)
    buffer = s_logBuffer;
    memset(buffer, 0, bufferSize);

    // Create FreeRTOS mutex for thread-safe operations
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        // Mutex creation failed - fall back to serial-only mode
        Serial.println("[LOGGER] ERROR: Failed to create mutex, falling back to serial-only mode");
        buffer = nullptr;  // static buffer, do not free
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
    // buffer points to static s_logBuffer — do not free
    buffer = nullptr;
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
    if (g_debugMode) {
        const uint32_t freeHeap = ESP.getFreeHeap();
        const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
        const int freeFdCount = getFreeFileDescriptorCount();

        timestampedMsg += " [res fh=" + String(freeHeap) +
                          " ma=" + String(maxAllocHeap) +
                          " fd=" + (freeFdCount >= 0 ? String(freeFdCount) : String("?")) +
                          "]";
    }
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
    
    // Note: persistent log saving is handled by the periodic flush task.
    // See dumpSavedLogsPeriodic() which should be called from main loop.
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

    // If the buffer has wrapped, the tail likely starts mid-line (e.g. "nitializing...").
    // Skip past the first newline to start output on a clean line boundary.
    uint32_t startOffset = 0;
    if (totalBytesLost > 0 && availableBytes > 0) {
        for (uint32_t i = 0; i < availableBytes && i < 512; i++) {
            uint32_t logicalIndex = tailIndex + i;
            size_t physicalPos = logicalIndex % bufferSize;
            if (buffer[physicalPos] == '\n') {
                startOffset = i + 1;  // skip past the newline itself
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

// Print only the newest tail of logs to a Print destination
size_t Logger::printLogsTail(Print& output, size_t maxBytes) {
    if (!initialized || buffer == nullptr || mutex == nullptr || maxBytes == 0) {
        return 0;
    }

    // Acquire mutex for thread-safe buffer access
    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    // Calculate available data to read (from tail to head)
    uint32_t availableBytes = headIndex - tailIndex;
    if (availableBytes > bufferSize) {
        availableBytes = bufferSize;
    }

    if (availableBytes == 0) {
        xSemaphoreGive(mutex);
        return 0;
    }

    // Snapshot indices under lock, but trim the start to requested tail size
    uint32_t currentHead = headIndex;
    uint32_t currentTail = tailIndex;

    uint32_t tailBytes = (maxBytes < availableBytes) ? (uint32_t)maxBytes : availableBytes;
    uint32_t desiredStart = (currentHead > tailBytes) ? (currentHead - tailBytes) : 0;
    if (desiredStart > currentTail) {
        currentTail = desiredStart;
        // desiredStart may land mid-line — skip to the first newline for clean output
        for (uint32_t i = 0; i < (currentHead - currentTail) && i < 512; i++) {
            size_t physicalPos = (currentTail + i) % bufferSize;
            if (buffer[physicalPos] == '\n') {
                currentTail += i + 1;
                break;
            }
        }
    }

    xSemaphoreGive(mutex);

    size_t bytesWritten = 0;
    char chunk[128];
    uint32_t pos = currentTail;

    while (pos < currentHead) {
        // Re-acquire mutex to read a chunk safely
        if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) {
            break;
        }

        // Check if our reader position has been overtaken by writer (buffer wrap)
        if (tailIndex > pos) {
            pos = tailIndex;
        }

        if (pos >= currentHead) {
            xSemaphoreGive(mutex);
            break;
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

// Enable or disable persistent log saving with multi-file rotation
void Logger::enableLogSaving(bool enable, fs::FS* logFS) {
    if (enable && logFS == nullptr) {
        return;
    }
    
    logSavingEnabled = enable;
    logFileSystem = enable ? logFS : nullptr;
    
    if (enable) {
        // Reset dump tracking when enabling
        lastDumpedBytes = 0;
        
        // One-time migration: remove legacy log files from old 2-file scheme
        logFS->remove("/syslog.A.txt");
        logFS->remove("/syslog.B.txt");
        logFS->remove("/last_reboot_log.txt");
        logFS->remove("/crash_log.txt");
        logFS->remove("/debug_log.txt");
        
        // Write a boot separator to syslog.0.txt so boot boundaries are visible
        File sepFile = logFS->open("/syslog.0.txt", FILE_APPEND);
        if (sepFile) {
            char sep[128];
            int n = snprintf(sep, sizeof(sep),
                             "\n=== BOOT %s (heap %u/%u) ===\n",
                             FIRMWARE_VERSION,
                             (unsigned)ESP.getFreeHeap(),
                             (unsigned)ESP.getMaxAllocHeap());
            if (n > 0) sepFile.write((const uint8_t*)sep, n);
            sepFile.close();
        }
    }
}

// Periodic log flush with multi-file rotation (syslog.0..3.txt)
bool Logger::dumpSavedLogsPeriodic(SDCardManager* sdManager) {
#ifdef UNIT_TEST
    return false;
#else
    (void)sdManager;

    if (!logSavingEnabled || logFileSystem == nullptr) {
        return false;
    }
    if (!initialized || buffer == nullptr || mutex == nullptr) {
        return false;
    }
    
    // Acquire mutex to safely read headIndex
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) != pdTRUE) {
        return false;
    }
    
    uint32_t currentBytes = headIndex;
    uint32_t newBytes = currentBytes - lastDumpedBytes;
    xSemaphoreGive(mutex);
    
    if (newBytes == 0) {
        return false;
    }
    
    // Acquire mutex again for reading buffer content
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }
    
    // If writer overtook our read pointer, skip to tail on a clean line boundary
    if (lastDumpedBytes < tailIndex) {
        lastDumpedBytes = tailIndex;
        uint32_t scanLimit = (headIndex - tailIndex);
        if (scanLimit > 512) scanLimit = 512;
        for (uint32_t i = 0; i < scanLimit; i++) {
            size_t physicalPos = (tailIndex + i) % bufferSize;
            if (buffer[physicalPos] == '\n') {
                lastDumpedBytes = tailIndex + i + 1;
                break;
            }
        }
    }
    
    uint32_t bytesToDump = headIndex - lastDumpedBytes;
    if (bytesToDump > bufferSize) {
        bytesToDump = bufferSize;
        lastDumpedBytes = headIndex - bufferSize;
    }
    
    uint32_t dumpStart = lastDumpedBytes;
    lastDumpedBytes = headIndex;
    xSemaphoreGive(mutex);
    
    // Append to syslog.0.txt (the active log file)
    File logFile = logFileSystem->open("/syslog.0.txt", FILE_APPEND);
    if (!logFile) {
        logFile = logFileSystem->open("/syslog.0.txt", FILE_WRITE);
        if (!logFile) {
            return false;
        }
    }
    
    char chunk[256];
    size_t chunkPos = 0;
    for (uint32_t i = 0; i < bytesToDump; i++) {
        size_t physicalPos = (dumpStart + i) % bufferSize;
        chunk[chunkPos++] = buffer[physicalPos];
        if (chunkPos == sizeof(chunk)) {
            logFile.write((const uint8_t*)chunk, chunkPos);
            chunkPos = 0;
        }
    }
    if (chunkPos > 0) {
        logFile.write((const uint8_t*)chunk, chunkPos);
    }
    
    size_t fileSize = logFile.size();
    logFile.close();
    
    // Rotate when syslog.0.txt exceeds the per-file limit
    if (fileSize > SYSLOG_MAX_FILE_SIZE) {
        // Shift files: delete oldest, rename each one level down
        char oldPath[24], newPath[24];
        // Delete the oldest file (N-1)
        snprintf(oldPath, sizeof(oldPath), "/syslog.%d.txt", SYSLOG_MAX_FILES - 1);
        logFileSystem->remove(oldPath);
        // Rename N-2 → N-1, N-3 → N-2, ... , 0 → 1
        for (int i = SYSLOG_MAX_FILES - 2; i >= 0; i--) {
            snprintf(oldPath, sizeof(oldPath), "/syslog.%d.txt", i);
            snprintf(newPath, sizeof(newPath), "/syslog.%d.txt", i + 1);
            logFileSystem->rename(oldPath, newPath);
        }
        // syslog.0.txt is now free for the next flush
    }
    
    return true;
#endif
}

// Stream all saved log files (oldest first) to a Print destination
size_t Logger::streamSavedLogs(Print& output) {
#ifdef UNIT_TEST
    return 0;
#else
    if (!logFileSystem) return 0;
    size_t total = 0;
    char path[24];
    uint8_t buf[256];
    // Stream from oldest (highest index) to newest (0)
    for (int i = SYSLOG_MAX_FILES - 1; i >= 0; i--) {
        snprintf(path, sizeof(path), "/syslog.%d.txt", i);
        File f = logFileSystem->open(path, FILE_READ);
        if (!f) continue;
        while (f.available()) {
            size_t n = f.read(buf, sizeof(buf));
            if (n > 0) { output.write(buf, n); total += n; }
            yield();
        }
        f.close();
    }
    return total;
#endif
}

// Write data to persistent log storage (debugging only) - DEPRECATED
// This method is no longer used - periodic saving is handled by dumpSavedLogsPeriodic()
void Logger::writeToStorage(const char* data, size_t len) {
    // This method is deprecated and no longer used
    // Persistent log saving is handled by periodic flush task
    // Kept for interface compatibility
}
// Dump current logs directly to a file on the provided filesystem (SD card).
// Used for boot-failure emergency dumps where SD card is the only accessible storage.
// Appends with a header and uses chunk writes for performance. Caps at 64 KB.
bool Logger::dumpToSD(fs::FS& fs, const char* filename, const char* reason) {
    if (!initialized || !this->buffer) return false;

    xSemaphoreTake(mutex, portMAX_DELAY);

    // Append mode — preserves previous boot-failure dumps
    File f = fs.open(filename, FILE_APPEND);
    if (!f) {
        // If file doesn't exist yet, create it
        f = fs.open(filename, FILE_WRITE);
        if (!f) {
            xSemaphoreGive(mutex);
            return false;
        }
    }

    // Cap file size at 64 KB — if already near limit, truncate and start fresh
    if (f.size() > 65536) {
        f.close();
        fs.remove(filename);
        f = fs.open(filename, FILE_WRITE);
        if (!f) {
            xSemaphoreGive(mutex);
            return false;
        }
    }

    // Write header with reason, uptime, and firmware version
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "\n=== CPAP UPLOADER BOOT ERROR ===\n"
             "Reason: %s\n"
             "Uptime: %lu ms\n"
             "Firmware: %s\n"
             "Free heap: %lu bytes\n"
             "=== Log Buffer ===\n",
             reason, (unsigned long)millis(), FIRMWARE_VERSION,
             (unsigned long)ESP.getFreeHeap());
    f.write((const uint8_t*)hdr, strlen(hdr));

    uint32_t availableBytes = headIndex - tailIndex;
    if (availableBytes > bufferSize) availableBytes = bufferSize;

    // Chunk-buffer writes for SD card performance
    if (availableBytes > 0) {
        char chunk[256];
        size_t chunkPos = 0;
        for (uint32_t i = 0; i < availableBytes; i++) {
            size_t physicalPos = (tailIndex + i) % bufferSize;
            chunk[chunkPos++] = this->buffer[physicalPos];
            if (chunkPos == sizeof(chunk)) {
                f.write((const uint8_t*)chunk, chunkPos);
                chunkPos = 0;
            }
        }
        if (chunkPos > 0) {
            f.write((const uint8_t*)chunk, chunkPos);
        }
    }

    f.write((const uint8_t*)"\n=== End of Boot Error Dump ===\n", 31);
    f.close();
    xSemaphoreGive(mutex);
    return true;
}

// Flush all pending circular-buffer content to NAND before reboot.
// Simply calls the periodic flush — no separate file needed.
bool Logger::flushBeforeReboot() {
    return dumpSavedLogsPeriodic(nullptr);
}

// Check if a previous boot failure left an emergency log on the SD card
void Logger::checkPreviousBootError(fs::FS& fs, const char* filename) {
#ifndef UNIT_TEST
    File f = fs.open(filename, FILE_READ);
    if (f) {
        size_t sz = f.size();
        f.close();
        logf("[WARN] Previous boot failure log found on SD card: %s (%u bytes)", filename, (unsigned)sz);
        log("[WARN] Check the file for details about the prior failure.");
    }
#endif
}

// Legacy compatibility — flush to syslog rotation
bool Logger::dumpSavedLogs(const String& reason) {
    (void)reason;
    return dumpSavedLogsPeriodic(nullptr);
}