#ifndef UPLOAD_STATE_MANAGER_H
#define UPLOAD_STATE_MANAGER_H

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>

class UploadStateManager {
private:
    using DayKey = uint32_t;
    using UnixTs = uint32_t;
    using PathHash = uint64_t;

    struct PendingFolderEntry {
        DayKey day;
        UnixTs firstSeenTs;
    };

    struct FileFingerprintEntry {
        PathHash pathHash;
        uint32_t fileSize;
        uint8_t md5[16];
        uint8_t flags;
    };

    enum class JournalEventType : uint8_t {
        SetTimestamp,
        SetRetry,
        AddCompleted,
        RemoveCompleted,
        AddPending,
        RemovePending,
        SetFile,
        RemoveFile
    };

    struct JournalEvent {
        JournalEventType type;
        DayKey day;
        UnixTs timestamp;
        uint16_t retryCount;
        PathHash pathHash;
        uint32_t fileSize;
        uint8_t md5[16];
        bool hasMd5;
    };

    static const uint16_t MAX_COMPLETED_FOLDERS = 4096;
    static const uint16_t MAX_PENDING_FOLDERS = 256;
    static const uint16_t MAX_FILE_ENTRIES = 384;
    static const uint16_t MAX_JOURNAL_EVENTS = 256;
    static const uint16_t COMPACTION_LINE_THRESHOLD = 256;
    static const uint32_t COMPACTION_SIZE_THRESHOLD_BYTES = 8192;

    static const uint8_t FILE_FLAG_ACTIVE = 0x01;
    static const uint8_t FILE_FLAG_HAS_MD5 = 0x02;
    static const uint8_t FILE_FLAG_PERSISTENT = 0x04;

    String stateSnapshotPath;
    String stateJournalPath;
    UnixTs lastUploadTimestamp;
    DayKey completedFolders[MAX_COMPLETED_FOLDERS];
    uint16_t completedCount;
    PendingFolderEntry pendingFolders[MAX_PENDING_FOLDERS];
    uint16_t pendingCount;
    FileFingerprintEntry fileEntries[MAX_FILE_ENTRIES];
    uint16_t fileEntryCount;
    DayKey currentRetryFolderDay;
    int currentRetryCount;
    JournalEvent journalEvents[MAX_JOURNAL_EVENTS];
    uint16_t journalEventCount;
    uint16_t journalLineCount;
    bool forceCompaction;
    int totalFoldersCount;  // Total DATALOG folders found (for progress tracking)
    
    static const unsigned long PENDING_FOLDER_TIMEOUT_SECONDS = 7 * 24 * 60 * 60;  // 604800 seconds
    
    void clearState();

    static bool isDatalogPath(const String& path);
    static bool parseDayKey(const String& text, DayKey& outDay);
    static void dayKeyToChars(DayKey day, char* out, size_t outLen);
    static PathHash hashPath(const String& path);
    static bool parseHexMd5(const char* hex, uint8_t out[16]);
    static void md5ToHex(const uint8_t md5[16], char out[33]);

    int findCompletedIndex(DayKey day) const;
    int findPendingIndex(DayKey day) const;
    int findFileIndex(PathHash pathHash) const;

    bool upsertFileEntry(PathHash pathHash, uint32_t fileSize, const uint8_t* md5, bool hasMd5, bool persistent, bool queue);
    bool removeFileEntry(PathHash pathHash, bool queue);

    bool addCompletedInternal(DayKey day, bool queue);
    bool removeCompletedInternal(DayKey day, bool queue);
    bool addPendingInternal(DayKey day, UnixTs ts, bool queue);
    bool removePendingInternal(DayKey day, bool queue);
    void setRetryInternal(DayKey day, int retryCount, bool queue);
    void setTimestampInternal(UnixTs ts, bool queue);

    void queueEvent(const JournalEvent& event);
    bool flushJournal(fs::FS &sd);
    bool appendJournalLine(File& file, const JournalEvent& event);
    bool applySnapshotLine(const char* line);
    bool applyJournalLine(const char* line);
    bool shouldCompact(fs::FS &sd) const;
    bool compactState(fs::FS &sd);
    bool replayJournal(fs::FS &sd);

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
