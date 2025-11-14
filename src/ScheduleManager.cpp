#include "ScheduleManager.h"

ScheduleManager::ScheduleManager() :
    uploadHour(12),
    lastUploadTimestamp(0),
    ntpSynced(false),
    ntpServer("pool.ntp.org"),
    gmtOffsetSeconds(0),
    daylightOffsetSeconds(0)
{}

bool ScheduleManager::begin(int uploadHour, long gmtOffset, int daylightOffset) {
    this->uploadHour = uploadHour;
    this->gmtOffsetSeconds = gmtOffset;
    this->daylightOffsetSeconds = daylightOffset;
    
    // Validate upload hour
    if (uploadHour < 0 || uploadHour > 23) {
        Serial.println("Invalid upload hour, using default (12)");
        this->uploadHour = 12;
    }
    
    return true;
}

bool ScheduleManager::syncTime() {
    Serial.println("Syncing time with NTP server...");
    
    // Configure time with NTP server and timezone offsets
    configTime(gmtOffsetSeconds, daylightOffsetSeconds, ntpServer);
    
    // Wait for time to be set (with timeout)
    int retries = 0;
    const int maxRetries = 10;
    
    while (retries < maxRetries) {
        time_t now = time(nullptr);
        if (now > 24 * 3600) {  // Time is set if it's past Jan 1, 1970 + 1 day
            struct tm timeinfo;
            if (getLocalTime(&timeinfo)) {
                ntpSynced = true;
                Serial.println("NTP time synchronized successfully");
                Serial.print("Current time: ");
                Serial.println(asctime(&timeinfo));
                return true;
            }
        }
        delay(500);
        retries++;
    }
    
    Serial.println("Failed to sync NTP time");
    ntpSynced = false;
    return false;
}

bool ScheduleManager::isUploadTime() {
    if (!ntpSynced) {
        Serial.println("Time not synced, cannot check upload schedule");
        return false;
    }
    
    time_t now = time(nullptr);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to get local time");
        return false;
    }
    
    // Check if we're in the upload hour
    if (timeinfo.tm_hour != uploadHour) {
        return false;
    }
    
    // Check if we've already uploaded today
    // Convert lastUploadTimestamp to tm struct
    if (lastUploadTimestamp > 0) {
        time_t lastUpload = lastUploadTimestamp;
        struct tm lastUploadInfo;
        localtime_r(&lastUpload, &lastUploadInfo);
        
        // If we uploaded today already, don't upload again
        if (timeinfo.tm_year == lastUploadInfo.tm_year &&
            timeinfo.tm_mon == lastUploadInfo.tm_mon &&
            timeinfo.tm_mday == lastUploadInfo.tm_mday) {
            return false;
        }
    }
    
    return true;
}

void ScheduleManager::markUploadCompleted() {
    lastUploadTimestamp = time(nullptr);
    Serial.print("Upload marked as completed at timestamp: ");
    Serial.println(lastUploadTimestamp);
}

unsigned long ScheduleManager::calculateNextUploadTime() {
    if (!ntpSynced) {
        return 0;
    }
    
    time_t now = time(nullptr);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return 0;
    }
    
    // Create a tm struct for the next upload time
    struct tm nextUpload = timeinfo;
    nextUpload.tm_hour = uploadHour;
    nextUpload.tm_min = 0;
    nextUpload.tm_sec = 0;
    
    // If current time is past the upload hour today, schedule for tomorrow
    if (timeinfo.tm_hour >= uploadHour) {
        nextUpload.tm_mday += 1;
    }
    
    // Convert to time_t
    time_t nextUploadTime = mktime(&nextUpload);
    
    return (unsigned long)nextUploadTime;
}

unsigned long ScheduleManager::getSecondsUntilNextUpload() {
    if (!ntpSynced) {
        return 0;
    }
    
    unsigned long nextUploadTime = calculateNextUploadTime();
    time_t now = time(nullptr);
    
    if (nextUploadTime > (unsigned long)now) {
        return nextUploadTime - (unsigned long)now;
    }
    
    return 0;
}

bool ScheduleManager::isTimeSynced() const {
    return ntpSynced;
}

unsigned long ScheduleManager::getLastUploadTimestamp() const {
    return lastUploadTimestamp;
}

void ScheduleManager::setLastUploadTimestamp(unsigned long timestamp) {
    lastUploadTimestamp = timestamp;
}
