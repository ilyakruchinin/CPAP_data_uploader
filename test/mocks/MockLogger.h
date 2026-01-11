#ifndef MOCK_LOGGER_H
#define MOCK_LOGGER_H

#ifdef UNIT_TEST

#include <cstdio>
#include <cstdarg>
#include <string>

// Mock Logger for native testing
class Logger {
public:
    struct LogData {
        std::string content;
        uint32_t bytesLost;
        
        LogData() : content(""), bytesLost(0) {}
    };

    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void log(const char* message) {
        if (message) {
            printf("%s", message);
        }
    }

    void log(const std::string& message) {
        log(message.c_str());
    }

    void logf(const char* format, ...) {
        if (format) {
            va_list args;
            va_start(args, format);
            vprintf(format, args);
            va_end(args);
        }
    }

    LogData retrieveLogs() {
        return LogData();
    }

    bool isInitialized() const {
        return true;
    }

private:
    Logger() {}
    ~Logger() {}
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
};

// Convenience macros
#define LOG(msg) Logger::getInstance().log(msg)
#define LOGF(fmt, ...) Logger::getInstance().logf(fmt, ##__VA_ARGS__)
#define LOG_INFO(msg) Logger::getInstance().log("[INFO] " msg)
#define LOG_ERROR(msg) Logger::getInstance().log("[ERROR] " msg)
#define LOG_DEBUG(msg) Logger::getInstance().log("[DEBUG] " msg)
#define LOG_WARN(msg) Logger::getInstance().log("[WARN] " msg)
#define LOG_INFOF(fmt, ...) Logger::getInstance().logf("[INFO] " fmt, ##__VA_ARGS__)
#define LOG_ERRORF(fmt, ...) Logger::getInstance().logf("[ERROR] " fmt, ##__VA_ARGS__)
#define LOG_DEBUGF(fmt, ...) Logger::getInstance().logf("[DEBUG] " fmt, ##__VA_ARGS__)
#define LOG_WARNF(fmt, ...) Logger::getInstance().logf("[WARN] " fmt, ##__VA_ARGS__)

#endif // UNIT_TEST

#endif // MOCK_LOGGER_H
