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
    std::map<String, unsigned long> fileSizes;  // Fast size-based change detection
    std::set<String> completedDatalogFolders;
    std::map<String, unsigned long> pendingDatalogFolders;  // folderName -> firstSeenTimestamp
    String currentRetryFolder;
    int currentRetryCount;
    int totalFoldersCount;  // Total DATALOG folders found (for progress tracking)
    
    static const unsigned long PENDING_FOLDER_TIMEOUT_SECONDS = 7 * 24 * 60 * 60;  // 604800 seconds
    
    bool loadState(fs::FS &sd);
    bool saveState(fs::FS &sd);

public:
    String calculateChecksum(fs::FS &sd, const String& filePath);
    UploadStateManager();
    
    bool begin(fs::FS &sd);
    
    // Checksum-based tracking for root/SETTINGS files
    bool hasFileChanged(fs::FS &sd, const String& filePath);
    void markFileUploaded(const String& filePath, const String& checksum, unsigned long fileSize = 0);
    
    // Folder-based tracking for DATALOG
    bool isFolderCompleted(const String& folderName);
    void markFolderCompleted(const String& folderName);
    void removeFolderFromCompleted(const String& folderName);  // For delta scan re-upload
    int getCompletedFoldersCount() const;
    int getIncompleteFoldersCount() const;
    void setTotalFoldersCount(int count);
    
    // Pending folder tracking for empty folders
    bool isPendingFolder(const String& folderName);
    void markFolderPending(const String& folderName, unsigned long timestamp);
    void removeFolderFromPending(const String& folderName);
    bool shouldPromotePendingToCompleted(const String& folderName, unsigned long currentTime);
    void promotePendingToCompleted(const String& folderName);
    int getPendingFoldersCount() const;
    
    // Retry tracking (only for current folder)
    int getCurrentRetryCount();
    String getCurrentRetryFolder() const;
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
