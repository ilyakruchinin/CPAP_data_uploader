#include <unity.h>

// Define UNIT_TEST before including any headers
#ifndef UNIT_TEST
#define UNIT_TEST
#endif

// Mock Arduino types and functions needed by Logger
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>

// Mock FreeRTOS types and functions
typedef void* SemaphoreHandle_t;
#define portMAX_DELAY 0xFFFFFFFF
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)

SemaphoreHandle_t xSemaphoreCreateMutex() {
    return (SemaphoreHandle_t)0x12345678;
}

void vSemaphoreDelete(SemaphoreHandle_t mutex) {
    (void)mutex;
}

int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned long timeout) {
    (void)mutex;
    (void)timeout;
    return pdTRUE;
}

void xSemaphoreGive(SemaphoreHandle_t mutex) {
    (void)mutex;
}

// Mock Arduino String class
class String {
private:
    std::string data;
    
public:
    String() : data("") {}
    String(const char* str) : data(str ? str : "") {}
    String(const std::string& str) : data(str) {}
    String(int num) : data(std::to_string(num)) {}
    
    const char* c_str() const { return data.c_str(); }
    size_t length() const { return data.length(); }
    bool isEmpty() const { return data.empty(); }
    char charAt(size_t index) const { 
        return (index < data.length()) ? data[index] : '\0'; 
    }
    
    void reserve(size_t size) { data.reserve(size); }
    
    String& operator+=(const String& other) {
        data += other.data;
        return *this;
    }
    
    String& operator+=(const char* str) {
        if (str) data += str;
        return *this;
    }
    
    String& operator+=(char c) {
        data += c;
        return *this;
    }
    
    String operator+(const String& other) const {
        String result(*this);
        result += other;
        return result;
    }
    
    String operator+(const char* str) const {
        String result(*this);
        result += str;
        return result;
    }
};

// Mock FS namespace (empty, not used in our tests)
namespace fs {
    class FS {};
}

// Mock Serial
class MockSerial {
public:
    void println(const char* str) { (void)str; }
    void println(const String& str) { (void)str; }
    void write(const uint8_t* buffer, size_t size) { (void)buffer; (void)size; }
    void write(char c) { (void)c; }
};

MockSerial Serial;

// Forward declare SDCardManager (not used in tests)
class SDCardManager;

// Prevent Arduino.h and other mock headers from being included
#define ARDUINO_H
#define MOCK_FS_H

// Set small buffer size for testing
#define LOG_BUFFER_SIZE 64

// Include Logger header
#include "../../include/Logger.h"

// Now include Logger.cpp implementation
#include "../../src/Logger.cpp"

// ============================================================================
// TESTABLE LOGGER - Inherits from Logger and mocks only what we don't need
// ============================================================================

class TestableLogger : public Logger {
public:
    // Constructor - calls Logger's protected constructor
    TestableLogger() : Logger() {}
    
    // Destructor
    virtual ~TestableLogger() {}
    
    // Override virtual methods we don't want to execute during testing
    
    // Mock getTimestamp to return predictable value
    virtual String getTimestamp() override {
        return "[12:30:45] ";
    }
    
    // Mock writeToSerial to do nothing (we don't care about serial output in tests)
    virtual void writeToSerial(const char* data, size_t len) override {
        (void)data;
        (void)len;
        // Do nothing - we're testing buffer logic, not serial output
    }
    
    // Mock writeToStorage to do nothing (not used in our tests)
    virtual void writeToStorage(const char* data, size_t len) override {
        (void)data;
        (void)len;
        // Do nothing - not testing persisted-log functionality
    }
    
    // Mock trackLostBytes to do nothing (handled in writeToBuffer)
    virtual void trackLostBytes(uint32_t bytesLost) override {
        (void)bytesLost;
        // Do nothing - bytes lost tracking is done in writeToBuffer
    }
    
    // Keep writeToBuffer as-is - THIS IS WHAT WE'RE TESTING
    // It will call the real Logger::writeToBuffer implementation
    
    // Expose protected members for testing
    uint32_t getHeadIndexPublic() const { return headIndex; }
    uint32_t getTailIndexPublic() const { return tailIndex; }
    uint32_t getTotalBytesLostPublic() const { return totalBytesLost; }
    size_t getBufferSizePublic() const { return bufferSize; }
    
    // Helper to inspect buffer content directly
    char getBufferAt(size_t pos) const {
        if (buffer && pos < bufferSize) {
            return buffer[pos];
        }
        return '\0';
    }
};

// ============================================================================
// TEST SETUP AND TEARDOWN
// ============================================================================

TestableLogger* logger = nullptr;

void setUp(void) {
    // Create a new TestableLogger for each test
    logger = new TestableLogger();
}

void tearDown(void) {
    // Clean up
    if (logger) {
        delete logger;
        logger = nullptr;
    }
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void printBufferState(const char* label) {
    printf("\n[%s]\n", label);
    printf("  Head: %u, Tail: %u, Lost: %u\n", 
           logger->getHeadIndexPublic(), 
           logger->getTailIndexPublic(),
           logger->getTotalBytesLostPublic());
    printf("  Buffer usage: %u / %zu bytes\n", 
           logger->getHeadIndexPublic() - logger->getTailIndexPublic(),
           logger->getBufferSizePublic());
}

// ============================================================================
// CIRCULAR BUFFER POINTER TESTS
// ============================================================================

// Test 1: Basic write and read
void test_logger_basic_write_read() {
    logger->log("Test");
    
    Logger::LogData logData = logger->retrieveLogs();
    
    TEST_ASSERT_TRUE(logData.content.length() > 0);
    TEST_ASSERT_TRUE(std::string(logData.content.c_str()).find("Test") != std::string::npos);
    TEST_ASSERT_EQUAL(0, logData.bytesLost);
}

// Test 2: Fill buffer to exact capacity
void test_logger_fill_buffer_exactly() {
    // Buffer is 64 bytes
    // Each log("X") writes "[12:30:45] X\n" = 13 bytes
    // 4 messages * 13 = 52 bytes (fits in 64-byte buffer)
    
    logger->log("A");
    logger->log("B");
    logger->log("C");
    logger->log("D");
    
    printBufferState("After 4 messages (52 bytes)");
    
    Logger::LogData logData = logger->retrieveLogs();
    std::string content(logData.content.c_str());
    
    printf("Content length: %zu bytes\n", logData.content.length());
    printf("Bytes lost: %u\n", logData.bytesLost);
    
    // All messages should be present
    TEST_ASSERT_TRUE_MESSAGE(content.find("A") != std::string::npos, "Message A should be present");
    TEST_ASSERT_TRUE_MESSAGE(content.find("B") != std::string::npos, "Message B should be present");
    TEST_ASSERT_TRUE_MESSAGE(content.find("C") != std::string::npos, "Message C should be present");
    TEST_ASSERT_TRUE_MESSAGE(content.find("D") != std::string::npos, "Message D should be present");
    
    // No bytes should be lost
    TEST_ASSERT_EQUAL_MESSAGE(0, logData.bytesLost, "No bytes should be lost when buffer is not full");
}

// Test 3: Overflow by one message - THIS WILL FAIL with current implementation
void test_logger_overflow_by_one_message() {
    printf("\n=== CRITICAL TEST: Overflow by one message ===\n");
    
    // Fill buffer to capacity (52 bytes)
    logger->log("A");  // 13 bytes
    logger->log("B");  // 13 bytes
    logger->log("C");  // 13 bytes
    logger->log("D");  // 13 bytes
    
    printBufferState("After filling to 52 bytes");
    
    // Now write one more message (13 bytes) - will overflow
    logger->log("E");  // 13 bytes - this will overflow by 1 byte (52+13=65, buffer=64)
    
    printBufferState("After overflow");
    
    Logger::LogData logData = logger->retrieveLogs();
    std::string content(logData.content.c_str());
    
    printf("Content length: %zu bytes\n", logData.content.length());
    printf("Bytes lost: %u\n", logData.bytesLost);
    printf("Content (first 30 chars): %.30s...\n", logData.content.c_str());
    
    // Expected: Should lose 1 byte (buffer overflow by 1) + partial line at start
    // After overflow, we skip the corrupted partial line at the start
    TEST_ASSERT_EQUAL_MESSAGE(13, logData.bytesLost, 
        "Should lose 1 byte overflow + 12 bytes partial line");
    
    // Buffer should contain less than 64 bytes (partial line skipped)
    TEST_ASSERT_TRUE_MESSAGE(logData.content.length() < 64 && logData.content.length() > 50, 
        "Buffer should contain ~52 bytes (64 - partial line)");
    
    // The newest message "E" should be present
    TEST_ASSERT_TRUE_MESSAGE(content.find("E") != std::string::npos, 
        "Newest message E should be present");
    
    // Note: The first byte of message "A" will be lost (the opening '[')
    // This is correct behavior for a byte-level circular buffer
}

// Test 4: Verify head doesn't overwrite tail before tail is advanced
void test_logger_head_tail_collision() {
    printf("\n=== CRITICAL TEST: Head/Tail collision ===\n");
    
    // Fill buffer completely (52 bytes)
    logger->log("A");
    logger->log("B");
    logger->log("C");
    logger->log("D");
    
    uint32_t headBefore = logger->getHeadIndexPublic();
    uint32_t tailBefore = logger->getTailIndexPublic();
    
    printf("Before overflow: head=%u, tail=%u, diff=%u\n", 
           headBefore, tailBefore, headBefore - tailBefore);
    
    // Write one more message - this should advance tail BEFORE head overwrites
    logger->log("E");
    
    uint32_t headAfter = logger->getHeadIndexPublic();
    uint32_t tailAfter = logger->getTailIndexPublic();
    
    printf("After overflow: head=%u, tail=%u, diff=%u\n", 
           headAfter, tailAfter, headAfter - tailAfter);
    
    // The difference should never exceed buffer size
    TEST_ASSERT_TRUE_MESSAGE(headAfter - tailAfter <= logger->getBufferSizePublic(),
        "Head - Tail should never exceed buffer size");
    
    // Tail should have advanced when overflow occurred
    TEST_ASSERT_TRUE_MESSAGE(tailAfter > tailBefore,
        "Tail should advance when buffer overflows");
}

// Test 5: Continuous overflow - write many messages
void test_logger_continuous_overflow() {
    printf("\n=== TEST: Continuous overflow ===\n");
    
    // Write 20 messages (each 13 bytes = 260 bytes total)
    // Buffer is 64 bytes, so we'll overflow multiple times
    for (int i = 0; i < 20; i++) {
        char msg[10];
        snprintf(msg, sizeof(msg), "MSG%d", i);
        logger->log(msg);
    }
    
    printBufferState("After 20 messages");
    
    Logger::LogData logData = logger->retrieveLogs();
    std::string content(logData.content.c_str());
    
    printf("Content length: %zu bytes\n", logData.content.length());
    printf("Bytes lost: %u\n", logData.bytesLost);
    
    // Should have lost many bytes
    TEST_ASSERT_TRUE_MESSAGE(logData.bytesLost > 100, 
        "Should have lost many bytes due to overflow");
    
    // Buffer should contain less than 64 bytes (partial line skipped at start)
    TEST_ASSERT_TRUE_MESSAGE(logData.content.length() < 64 && logData.content.length() > 50, 
        "Buffer should contain ~51-63 bytes (partial line skipped)");
    
    // Most recent messages should be present
    TEST_ASSERT_TRUE_MESSAGE(content.find("MSG19") != std::string::npos, 
        "Most recent message should be present");
    
    // Oldest messages should be gone
    TEST_ASSERT_FALSE_MESSAGE(content.find("MSG0") != std::string::npos, 
        "Oldest message should be lost");
}

// Test 6: Verify chronological order after overflow
void test_logger_chronological_order() {
    // Write messages that will cause overflow
    for (int i = 0; i < 10; i++) {
        char msg[10];
        snprintf(msg, sizeof(msg), "O%d", i);
        logger->log(msg);
    }
    
    Logger::LogData logData = logger->retrieveLogs();
    std::string content(logData.content.c_str());
    
    // Find positions of messages that should be present
    size_t pos8 = content.find("O8");
    size_t pos9 = content.find("O9");
    
    // Both should be present
    TEST_ASSERT_TRUE_MESSAGE(pos8 != std::string::npos, "O8 should be present");
    TEST_ASSERT_TRUE_MESSAGE(pos9 != std::string::npos, "O9 should be present");
    
    // O8 should come before O9 (chronological order)
    TEST_ASSERT_TRUE_MESSAGE(pos8 < pos9, 
        "Messages should be in chronological order");
}

// Test 7: Buffer wrapping with modulo arithmetic
void test_logger_buffer_wrapping() {
    printf("\n=== TEST: Buffer wrapping ===\n");
    
    // Write enough to wrap around multiple times
    for (int i = 0; i < 50; i++) {
        char msg[10];
        snprintf(msg, sizeof(msg), "W%d", i);
        logger->log(msg);
    }
    
    printBufferState("After 50 messages");
    
    uint32_t head = logger->getHeadIndexPublic();
    uint32_t tail = logger->getTailIndexPublic();
    
    printf("Head index: %u (physical: %zu)\n", head, head % logger->getBufferSizePublic());
    printf("Tail index: %u (physical: %zu)\n", tail, tail % logger->getBufferSizePublic());
    
    // Head should have wrapped around multiple times
    TEST_ASSERT_TRUE_MESSAGE(head > logger->getBufferSizePublic() * 5, 
        "Head should have wrapped around multiple times");
    
    // Buffer usage should be exactly bufferSize
    TEST_ASSERT_EQUAL_MESSAGE(logger->getBufferSizePublic(), head - tail,
        "Buffer usage should be exactly bufferSize");
    
    Logger::LogData logData = logger->retrieveLogs();
    // After overflow, partial line at start is skipped
    TEST_ASSERT_TRUE_MESSAGE(logData.content.length() < 64 && logData.content.length() > 50,
        "Buffer should contain ~51-63 bytes (partial line skipped)");
}

// Test 8: Bytes lost calculation accuracy
void test_logger_bytes_lost_accuracy() {
    printf("\n=== TEST: Bytes lost accuracy ===\n");
    
    // Write exactly 52 bytes (4 messages, no overflow)
    logger->log("A");
    logger->log("B");
    logger->log("C");
    logger->log("D");
    
    Logger::LogData logData1 = logger->retrieveLogs();
    printf("After 52 bytes: lost=%u\n", logData1.bytesLost);
    TEST_ASSERT_EQUAL_MESSAGE(0, logData1.bytesLost, "Should have 0 bytes lost");
    
    // Write one more message (13 bytes, overflow by 1)
    logger->log("E");
    
    Logger::LogData logData2 = logger->retrieveLogs();
    printf("After 65 bytes: lost=%u\n", logData2.bytesLost);
    
    // Should have lost 1 byte (overflow) + partial line at start (12 bytes)
    // After overflow, we skip the corrupted partial line
    TEST_ASSERT_EQUAL_MESSAGE(13, logData2.bytesLost, 
        "Should have lost 1 byte overflow + 12 bytes partial line");
}

// Test 9: Multiple retrieve calls return same data
void test_logger_multiple_retrieve_calls() {
    logger->log("PERSISTENT1");
    logger->log("PERSISTENT2");
    
    Logger::LogData logData1 = logger->retrieveLogs();
    Logger::LogData logData2 = logger->retrieveLogs();
    
    // Both should return the same content
    TEST_ASSERT_EQUAL(logData1.content.length(), logData2.content.length());
    TEST_ASSERT_EQUAL_STRING(logData1.content.c_str(), logData2.content.c_str());
    TEST_ASSERT_EQUAL(logData1.bytesLost, logData2.bytesLost);
}

// Test 10: Stress test - rapid writes
void test_logger_stress_rapid_writes() {
    printf("\n=== TEST: Stress test ===\n");
    
    // Write many messages rapidly
    for (int i = 0; i < 100; i++) {
        char msg[20];
        snprintf(msg, sizeof(msg), "STRESS%d", i);
        logger->log(msg);
    }
    
    printBufferState("After stress test");
    
    Logger::LogData logData = logger->retrieveLogs();
    
    // Should not crash and should return valid data
    TEST_ASSERT_TRUE(logData.content.length() > 0);
    TEST_ASSERT_TRUE(logData.bytesLost > 0);
    
    // Most recent message should be present
    std::string content(logData.content.c_str());
    TEST_ASSERT_TRUE(content.find("STRESS99") != std::string::npos);
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    printf("\n");
    printf("=================================================================\n");
    printf("Logger Circular Buffer Tests\n");
    printf("=================================================================\n");
    printf("Buffer Size: %d bytes\n", LOG_BUFFER_SIZE);
    printf("Testing ACTUAL Logger implementation using inheritance.\n");
    printf("\n");
    printf("The circular buffer operates at BYTE-LEVEL granularity.\n");
    printf("When overflow occurs, bytes are lost one at a time, which may\n");
    printf("result in partial messages at the buffer boundaries.\n");
    printf("\n");
    printf("After fixing the bugs, all tests should pass.\n");
    printf("=================================================================\n");
    printf("\n");
    
    RUN_TEST(test_logger_basic_write_read);
    RUN_TEST(test_logger_fill_buffer_exactly);
    RUN_TEST(test_logger_overflow_by_one_message);
    RUN_TEST(test_logger_head_tail_collision);
    RUN_TEST(test_logger_continuous_overflow);
    RUN_TEST(test_logger_chronological_order);
    RUN_TEST(test_logger_buffer_wrapping);
    RUN_TEST(test_logger_bytes_lost_accuracy);
    RUN_TEST(test_logger_multiple_retrieve_calls);
    RUN_TEST(test_logger_stress_rapid_writes);
    
    return UNITY_END();
}
