# Mock Infrastructure

This directory contains mock implementations of hardware-dependent components for unit testing.

## Available Mocks

### ArduinoJson.h
Mock implementation of the ArduinoJson library for JSON parsing:
- `StaticJsonDocument<SIZE>` class: Mock JSON document container with fixed capacity
- `DynamicJsonDocument` class: Mock JSON document with dynamic capacity
- `DeserializationError` class: Mock error handling with error codes
- `JsonObject` class: Mock JSON object with iterator support
- `JsonArray` class: Mock JSON array with iterator support
- `JsonPair` class: Mock key-value pair for object iteration
- `JsonVariant` class: Mock variant type supporting multiple value types
- `deserializeJson()` function: Parses JSON from files
- `serializeJson()` function: Writes JSON to files
- Supports string, int, long, and unsigned long value types
- Implements pipe operator (`|`) for default values

Key features:
- Parse JSON from MockFile objects
- Access values with `doc["key"]` syntax or `doc.getObject("key")` / `doc.getArray("key")`
- Provide default values with `doc["key"] | defaultValue`
- Handles nested objects and arrays
- Supports iteration over objects with `for (auto it = obj.begin(); it != obj.end(); ++it)`
- Supports iteration over arrays with range-based for loops
- Compatible with both test code and conditional compilation patterns

### Arduino.h
Mock implementation of Arduino core functions and types:
- Basic types: `byte`, `boolean`
- Time functions: `millis()`, `micros()`, `delay()`
- Math functions: `min()`, `max()`, `constrain()`, `map()`
- Serial communication: `Serial` object for logging
- Pin functions: `pinMode()`, `digitalWrite()`, `digitalRead()` (no-ops)

### MockFS.h
Mock filesystem implementation that simulates SD card operations:
- `MockFS` class: Simulates the `fs::FS` interface
- `MockFile` class: Simulates file operations
- `String` class: Mock Arduino String class
- In-memory file storage for testing

Key features:
- Add files with `addFile(path, content)`
- Add directories with `addDirectory(path)`
- Open files with `open(path, mode)`
- List directory contents with `listDir(path)`
- Clear all files with `clear()`

### MockTime.h
Mock time functions for deterministic testing:
- `MockTimeState` class: Controls mock time
- `setMillis(ms)`: Set current time in milliseconds
- `advanceMillis(ms)`: Advance time by milliseconds
- `setTime(t)`: Set current time in seconds since epoch
- `advanceTime(seconds)`: Advance time by seconds
- `reset()`: Reset all time values to zero

### MockLogger.h
Mock logging system for native testing:
- `Logger` class: Singleton logger that outputs to stdout
- `log(message)`: Log a message
- `logf(format, ...)`: Log a formatted message (printf-style)
- `retrieveLogs()`: Returns empty LogData (no buffering in mock)
- Convenience macros: `LOG()`, `LOGF()`, `LOG_INFO()`, etc.

Key features:
- Outputs directly to stdout for test visibility
- Compatible with real Logger API
- No circular buffer (not needed for tests)
- Thread-safe (single-threaded test environment)

**Important:** When including source files that use Logger in tests, prevent duplicate Logger definitions:

```cpp
#include "MockLogger.h"
#define LOGGER_H  // Prevent real Logger.h from being included
#include "../../src/YourComponent.cpp"
```

### FS.h
Wrapper that includes MockFS.h when UNIT_TEST is defined.

## Usage in Tests

### Basic Setup

```cpp
#include <unity.h>
#include "Arduino.h"
#include "FS.h"
#include "MockTime.h"

void setUp(void) {
    // Reset time before each test
    MockTimeState::reset();
}

void tearDown(void) {
    // Cleanup after each test
}
```

### Using MockFS

```cpp
void test_file_operations() {
    fs::FS mockSD;
    
    // Add a test file
    mockSD.addFile("/test.txt", "Hello, World!");
    
    // Open and read the file
    fs::File file = mockSD.open("/test.txt", "r");
    TEST_ASSERT_TRUE(file);
    
    uint8_t buffer[20];
    size_t bytesRead = file.read(buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(13, bytesRead);
    
    file.close();
}
```

### Using MockTime

```cpp
void test_timing() {
    // Set initial time
    MockTimeState::setMillis(1000);
    TEST_ASSERT_EQUAL(1000, millis());
    
    // Advance time
    MockTimeState::advanceMillis(500);
    TEST_ASSERT_EQUAL(1500, millis());
    
    // Test delay
    delay(100);
    TEST_ASSERT_EQUAL(1600, millis());
}
```

### Using Mock Serial

```cpp
void test_logging() {
    Serial.begin(115200);
    Serial.println("Test message");  // Prints to stdout
    Serial.print("Value: ");
    Serial.println(42);
}
```

### Using ArduinoJson Mock

```cpp
void test_json_parsing() {
    fs::FS mockSD;
    
    // Create a JSON config file
    std::string jsonContent = R"({
        "WIFI_SSID": "TestNetwork",
        "UPLOAD_MODE": "scheduled",
        "UPLOAD_START_HOUR": 14,
        "UPLOAD_END_HOUR": 16,
        "GMT_OFFSET_HOURS": -8
    })";
    mockSD.addFile("/config.txt", jsonContent);
    
    // Parse the JSON
    fs::File file = mockSD.open("/config.txt", "r");
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    TEST_ASSERT_FALSE(error);
    
    // Access values with defaults
    String ssid = doc["WIFI_SSID"] | "";
    String mode = doc["UPLOAD_MODE"] | "scheduled";
    int startHour = doc["UPLOAD_START_HOUR"] | 8;
    int endHour = doc["UPLOAD_END_HOUR"] | 22;
    int offset = doc["GMT_OFFSET_HOURS"] | 0;
    
    TEST_ASSERT_EQUAL_STRING("TestNetwork", ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("scheduled", mode.c_str());
    TEST_ASSERT_EQUAL(14, startHour);
    TEST_ASSERT_EQUAL(16, endHour);
    TEST_ASSERT_EQUAL(-8, offset);
}

void test_json_nested_objects() {
    fs::FS mockSD;
    
    // Create JSON with nested objects and arrays
    std::string jsonContent = R"({
        "version": 1,
        "file_checksums": {
            "/file1.txt": "abc123",
            "/file2.txt": "def456"
        },
        "completed_folders": ["20241101", "20241102"]
    })";
    mockSD.addFile("/state.json", jsonContent);
    
    // Parse and access nested data
    fs::File file = mockSD.open("/state.json", "r");
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    
    TEST_ASSERT_FALSE(error);
    
    // Access nested object using getObject()
    JsonObject checksums = doc.getObject("file_checksums");
    TEST_ASSERT_FALSE(checksums.isNull());
    
    // Iterate over object
    for (auto it = checksums.begin(); it != checksums.end(); ++it) {
        String key = String(it->first.c_str());
        String value = String(it->second.as<const char*>());
        // Process key-value pairs
    }
    
    // Access array using getArray()
    JsonArray folders = doc.getArray("completed_folders");
    TEST_ASSERT_FALSE(folders.isNull());
    
    // Iterate over array
    for (JsonVariant v : folders) {
        String folder = String(v.as<const char*>());
        // Process array items
    }
}
```

## Conditional Compilation

All mocks are only active when `UNIT_TEST` is defined. This is automatically set by the native test environment in platformio.ini.

In production code:
- Real Arduino.h is used
- Real FS.h from ESP32 SDK is used
- Real time functions from ESP32 SDK are used
- Real ArduinoJson library is used

In test code:
- Mock implementations are used
- Tests run on native platform (not ESP32)
- Deterministic behavior for reliable testing

### Handling API Differences

Some components have API differences between the real implementation and the mock. Use conditional compilation to handle these:

```cpp
#ifdef UNIT_TEST
    // Mock ArduinoJson uses getObject() and getArray()
    JsonObject obj = doc.getObject("key");
    JsonArray arr = doc.getArray("key");
    
    // Mock uses std::map::iterator
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        String key = String(it->first.c_str());
        String value = String(it->second.as<const char*>());
    }
#else
    // Real ArduinoJson v6 uses operator[] with implicit conversion
    JsonObject obj = doc["key"];
    JsonArray arr = doc["key"];
    
    // Real uses JsonPair
    for (JsonPair kv : obj) {
        String key = String(kv.key().c_str());
        String value = String(kv.value().as<const char*>());
    }
#endif
```

This pattern allows the same source file to work in both test and production environments.

## Adding New Mocks

To add a new mock:

1. Create a new header file in this directory (e.g., `MockWiFi.h`)
2. Wrap all mock code with `#ifdef UNIT_TEST`
3. Implement the interface that matches the real component
4. Document the mock in this README
5. Include the mock in test files as needed

Example:

```cpp
#ifndef MOCK_WIFI_H
#define MOCK_WIFI_H

#ifdef UNIT_TEST

class MockWiFi {
private:
    bool connected;
    
public:
    MockWiFi() : connected(false) {}
    
    void begin(const char* ssid, const char* password) {
        connected = true;
    }
    
    bool isConnected() {
        return connected;
    }
    
    void disconnect() {
        connected = false;
    }
};

extern MockWiFi WiFi;

#endif // UNIT_TEST

#endif // MOCK_WIFI_H
```
