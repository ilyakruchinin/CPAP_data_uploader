#ifndef UPLOAD_STATE_MANAGER_H
#define UPLOAD_STATE_MANAGER_H

#include <Arduino.h>
#include <FS.h>
#include <map>
#include <set>

class UploadStateManager {
private:
    String stateFilePath;
    unsigned long lastUploadTimestamp;
    std::map<String, String> fileChecksums;
    std::set<String> completedDatalogFolders;
    String currentRetryFolder;
    int currentRetryCount;
    
    String calculateChecksum(fs::FS &sd, const String& filePath);
    bool loadState(fs::FS &sd);
    bool saveState(fs::FS &sd);

public:
    UploadStateManager();
    
    bool begin(fs::FS &sd);
    
    // Checksum-based tracking for root/SETTINGS files
    bool hasFileChanged(fs::FS &sd, const String& filePath);
    void markFileUploaded(const String& filePath, const String& checksum);
    
    // Folder-based tracking for DATALOG
    bool isFolderCompleted(const String& folderName);
    void markFolderCompleted(const String& folderName);
    
    // Retry tracking (only for current folder)
    int getCurrentRetryCount();
    void setCurrentRetryFolder(const String& folderName);
    void incrementCurrentRetryCount();
    void clearCurrentRetry();
    
    // Timestamp tracking
    unsigned long getLastUploadTimestamp();
    void setLastUploadTimestamp(unsigned long timestamp);
    
    // Persistence
    bool save(fs::FS &sd);
};

#endif // UPLOAD_STATE_MANAGER_H
