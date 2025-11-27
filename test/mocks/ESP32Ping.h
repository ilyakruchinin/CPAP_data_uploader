#ifndef ESP32PING_H
#define ESP32PING_H

#ifdef UNIT_TEST

// Mock Ping object for native testing (matches ESP32Ping library interface)
class PingClass {
public:
    bool ping(const char* host, int count = 4) {
        // Always succeed in tests
        return true;
    }
    
    bool ping(const char* host) {
        return ping(host, 4);
    }
    
    float averageTime() {
        // Return a reasonable ping time
        return 25.5f;  // 25.5ms
    }
};

// Global Ping object (matches ESP32Ping library)
extern PingClass Ping;

#endif // UNIT_TEST

#endif // ESP32PING_H
