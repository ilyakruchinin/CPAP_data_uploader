#ifndef TIME_BUDGET_MANAGER_H
#define TIME_BUDGET_MANAGER_H

#include <Arduino.h>

/**
 * TimeBudgetManager
 * 
 * Manages time budgets for SD card access and estimates upload times.
 * Tracks transmission rates and enforces session duration limits.
 */
class TimeBudgetManager {
private:
    unsigned long sessionStartTime;
    unsigned long sessionDurationMs;
    unsigned long transmissionRateBytesPerSec;
    
    // Default transmission rate: 40 KB/s (conservative estimate for SMB over WiFi)
    static const unsigned long DEFAULT_RATE = 40 * 1024;
    
    // Running average for transmission rate
    static const int RATE_HISTORY_SIZE = 5;
    unsigned long rateHistory[RATE_HISTORY_SIZE];
    int rateHistoryIndex;
    int rateHistoryCount;
    
    void updateTransmissionRate(unsigned long bytesTransferred, unsigned long elapsedMs);
    unsigned long calculateAverageRate();

public:
    TimeBudgetManager();
    
    // Session management
    void startSession(unsigned long durationSeconds);
    void startSession(unsigned long durationSeconds, int retryMultiplier);
    
    // Budget checking
    unsigned long getRemainingBudgetMs();
    bool hasBudget();
    
    // Upload time estimation
    unsigned long estimateUploadTimeMs(unsigned long fileSize);
    bool canUploadFile(unsigned long fileSize);
    
    // Transmission rate tracking
    void recordUpload(unsigned long fileSize, unsigned long elapsedMs);
    unsigned long getTransmissionRate();  // Get current rate in bytes/sec
    
    // Wait time calculation
    unsigned long getWaitTimeMs();
};

#endif // TIME_BUDGET_MANAGER_H
