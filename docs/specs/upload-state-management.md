# Upload State Management

## Overview
The Upload State Manager (`UploadStateManager.cpp/.h`) provides persistent tracking of upload progress for each backend independently. Separate instances track SMB and Cloud upload history, ensuring no duplicate uploads and enabling efficient resume after interruptions. Uses a snapshot + journal architecture for performance and reliability.

## Architecture

### Storage Format Design
The v2 format uses a **line-based text approach** instead of JSON for critical performance and reliability reasons:

**Format Structure:**
- **Snapshot File**: Complete state in human-readable lines
- **Journal File**: Append-only incremental updates
- **Line Format**: `TYPE|DATA` (pipe-delimited, human-readable)

**Example Lines:**
```
U2|2|1704312000           # Version, timestamp
R|20240101|0             # Retry state
C|20240101                # Completed folder
F|1234567890abcdef|1024|- # File entry (hash, size, MD5)
```

### Why Line-Based Instead of JSON?

| Aspect | Line-Based (v2) | JSON (v1) | Winner |
|---|---|---|---|
| **Memory Usage** | Fixed buffers, no allocation | DynamicJsonDocument (8KB+) | Line-based |
| **Heap Fragmentation** | Minimal (stack strings) | High (frequent alloc/free) | Line-based |
| **Parse Speed** | Simple sscanf() | ArduinoJson parsing | Line-based |
| **File Size** | Compact (no whitespace) | Larger (JSON syntax) | Line-based |
| **Human Readable** | Yes (plain text) | Yes (formatted) | Tie |
| **Error Recovery** | Line-by-line parsing | All-or-nothing parse | Line-based |
| **Append Performance** | O(1) (just add line) | O(n) (rewrite entire file) | Line-based |

### Storage Files
- **SMB State**: `.upload_state.v2.smb` (snapshot) + `.upload_state.v2.smb.log` (journal)
- **Cloud State**: `.upload_state.v2.cloud` (snapshot) + `.upload_state.v2.cloud.log` (journal)
- **Rolling Window**: Tracks last 365 days of data (configurable)
- **Independent Tracking**: Each backend maintains separate upload history

### Line Format Specification

**Snapshot Lines (State):**
```
U2|2|1704312000           # Header: version|subversion|timestamp
R|20240101|0             # Retry: day|count
C|20240101               # Completed folder: day
P|20240101|1704224000     # Pending folder: day|first_seen
F|hash|size|md5           # File entry: path_hash|file_size|md5_hash
```

**Backward Compatibility:**  
The parser ignores extra `|`-delimited fields after the day token in `C|` lines, enabling migration from older snapshot formats that stored additional fields.

**Backend Summary Files (per-backend session info):**
```
/.backend_summary.smb     # Written at session START and END
/.backend_summary.cloud
# Content: ts=1704312000,done=12,total=20,empty=1
#   ts    = Unix timestamp of session start (used for cycling)
#   done  = completed folders last session
#   total = total eligible folders last session  
#   empty = pending-empty folders last session
```

**Journal Lines (Events):**
```
T|1704312000              # Timestamp update
R|20240101|1             # Retry update
C+|20240101               # Add completed folder
C-|20240101               # Remove completed folder
P+|20240101|1704224000     # Add pending folder
P-|20240101               # Remove pending folder
F|hash|size|md5           # Set file entry
F-|hash                   # Remove file entry
```

**Field Types:**
- `hash`: 16-char hex (64-bit path hash)
- `size`: Decimal bytes
- `md5`: 32-char hex MD5 or `-` for none
- `day`: 8-char YYYYMMDD
- `timestamp`: Unix timestamp (seconds)

### Data Structures
```cpp
struct FileFingerprintEntry {
    String path;           // File path
    String checksum;       // MD5 hash (for critical files)
    uint32_t fileSize;     // File size (for recent DATALOG files)
    uint32_t timestamp;    // Last modification time
};

struct PendingFolderEntry {
    String folderName;     // DATALOG folder name
    uint32_t firstSeen;    // When folder was first detected empty
};
```

### Dual Backend Architecture
```cpp
// In FileUploader.cpp
smbStateManager = new UploadStateManager();
smbStateManager->setPaths("/.upload_state.v2.smb", "/.upload_state.v2.smb.log");

cloudStateManager = new UploadStateManager();
cloudStateManager->setPaths("/.upload_state.v2.cloud", "/.upload_state.v2.cloud.log");
```

**Benefits:**
- **Independent tracking**: Each backend tracks its own upload history
- **Flexible configuration**: Can use SMB, Cloud, or both simultaneously
- **State isolation**: Backend failures don't affect other backend's state
- **Web interface**: Shows separate progress bars for each backend

## Key Features

### Intelligent File Tracking
- **Recent DATALOG files**: Size-only tracking (no MD5 for performance)
- **Critical files**: MD5 checksum tracking (Identification.*, SETTINGS/)
- **Folder completion**: Tracks when all files in folder uploaded
- **Pending folders**: Monitors empty folders that may acquire content

### Change Detection
```cpp
bool hasFileChanged(fs::FS &sd, const String& path) {
    // For recent DATALOG: size comparison only
    if (isRecentDatalogFile(path)) {
        return currentSize != storedSize;
    }
    // For critical files: MD5 checksum comparison
    return calculateChecksum(sd, path) != storedChecksum;
}
```

### Empty Folder Handling
- **7-day waiting period**: Before marking empty folders complete
- **Pending tracking**: `markFolderPending()` for newly detected empty folders
- **Auto-promotion**: `shouldPromotePendingToCompleted()` after timeout
- **Content detection**: Automatically removes from pending when files appear

### State Persistence
- **Atomic saves**: Write to temporary file, then rename
- **Journal replay**: Reconstructs current state from snapshot + journal
- **Compaction**: Periodic cleanup to prevent journal bloat
- **Error recovery**: Corrupted files trigger clean state rebuild

## Core Operations

### File Status Management
```cpp
// Mark file as uploaded
bool markFileUploaded(fs::FS &sd, const String& path, uint32_t size, const String& checksum);

// Check if file needs uploading
bool hasFileChanged(fs::FS &sd, const String& path);

// Get file fingerprint for comparison
FileFingerprintEntry getFileFingerprint(const String& path);
```

### Folder Status Management
```cpp
// Folder-based tracking for DATALOG
bool isFolderCompleted(const String& folderName);
void markFolderCompleted(const String& folderName);
void markFolderCompletedWithScan(const String& folderName, bool recentScanPassed);
void markFolderRecentScanFailed(const String& folderName);
void markFolderUploadProgress(const String& folderName, uint16_t filesTotal, uint16_t filesUploaded, bool uploadSuccess);
bool isFolderUploadSuccessful(const String& folderName);
bool shouldRescanRecentFolder(const String& folderName);
void removeFolderFromCompleted(const String& folderName);
int getCompletedFoldersCount() const;
int getSuccessfulFoldersCount() const;  // Only folders with all files uploaded successfully
int getIncompleteFoldersCount() const;
void setTotalFoldersCount(int count);

// Handle empty folders
bool markFolderPending(const String& folderName);
bool isPendingFolder(const String& folderName);
```

### State Persistence
```cpp
// Save each backend's state independently
smbStateManager->save(sd);    // Saves .upload_state.v2.smb + .log
cloudStateManager->save(sd);  // Saves .upload_state.v2.cloud + .log

// Load state from files
bool load(fs::FS &sd);

// Replay journal for current state
bool replayJournal(fs::FS &sd);
```

## Performance Benefits Over JSON

### Memory Efficiency
**JSON (v1) Issues:**
- `DynamicJsonDocument` required 8KB+ allocation for every save/load
- Frequent malloc/free cycles caused heap fragmentation
- String objects allocated on heap for every field
- Memory pressure increased during long upload sessions

**Line-Based (v2) Benefits:**
- Fixed stack buffers (256 chars) for line processing
- No dynamic allocation during normal operation
- Minimal heap footprint (~2KB total)
- Predictable memory usage patterns

### I/O Performance
**JSON Approach:**
```cpp
// Required rewriting entire file for every change
DynamicJsonDocument doc(8192);
doc["files"][path]["checksum"] = checksum;
serializeJson(doc, file);  // Writes entire state
```

**Line-Based Approach:**
```cpp
// Append-only journal for incremental changes
snprintf(line, sizeof(line), "F|%016llx|%lu|%s", hash, size, md5);
file.println(line);  // O(1) append operation
```

### Reliability Improvements
- **Partial corruption**: JSON parse fails entirely, line-based skips bad lines
- **Power loss**: Journal preserves recent changes, JSON loses all in-progress writes
- **Debugging**: Human-readable lines (manual inspection and recovery)

## Version History
- **v1**: JSON-based format (deprecated, caused heap fragmentation)
- **v2**: Line-based format (current, optimized for low memory systems)

**Note**: There is no automatic migration from v1 to v2. The v2 format was introduced to solve critical heap fragmentation issues, and systems were upgraded manually during development. New installations will only create v2 files.

## Performance Optimizations

### Size-Only Tracking
Recent DATALOG files (within `RECENT_FOLDER_DAYS`) use size comparison instead of MD5:
- **Performance**: ~10x faster than MD5 calculation
- **Memory**: No need to load entire file for hashing
- **Sufficient**: CPAP files rarely change size without content change

### Hybrid Completion Tracking
Recent folders use enhanced completion tracking to handle upload failures:
- **Recent Scan Passed**: Flag indicating successful recent upload scan
- **Last Scan Time**: Timestamp of most recent scan attempt
- **Failure Recovery**: Failed recent scans are retried on next upload cycle
- **Data Safety**: Prevents data loss from network failures or CPAP downtime

**Logic Flow:**
1. Recent folder marked as completed + `recentScanPassed = true`
2. If upload fails → `recentScanPassed = false` (folder marked complete but scan failed)
3. Next scan → Re-scans folders with `recentScanPassed = false`
4. When folder becomes old → Clear recent scan flags

### Journal Efficiency
- **Append-only**: Minimal write amplification
- **Batch operations**: Multiple changes in single journal entry
- **Selective replay**: Only applies to relevant file types

### Memory Management
- **Streaming**: Large files processed without full loading
- **Stack allocation**: Temporary buffers on stack where possible
- **Lazy loading**: Only loads state when needed

## Configuration Integration
- **MAX_DAYS**: Maximum age of tracked data (default: 365)
- **RECENT_FOLDER_DAYS**: Threshold for size-only tracking (default: 2)
- **Backend-specific**: Separate state managers for SMB and Cloud

## Error Handling & Recovery

### Corruption Detection
- **Magic numbers**: Verify file format integrity
- **Checksum validation**: Detect data corruption
- **Size checks**: Validate reasonable file sizes

### Recovery Strategies
- **Journal replay**: Rebuild from journal if snapshot corrupted
- **Clean reset**: Clear all state if recovery fails
- **Partial recovery**: Salvage valid entries when possible

### Data Safety
- **Atomic operations**: Prevent partial writes
- **Backup creation**: Keep previous snapshot during save
- **Validation**: Verify saved data integrity

## Integration Points
- **FileUploader**: Creates and manages separate SMB and Cloud state managers
- **SDCardManager**: File system access for state files
- **Config**: Provides timing and retention parameters
- **SMBUploader**: Updates smbStateManager after successful uploads
- **SleepHQUploader**: Updates cloudStateManager after successful uploads
- **WebServer**: Displays separate progress for each backend

## Usage Patterns

### Typical Upload Flow
1. `scanDatalogFolders()` checks `isFolderCompleted()`
2. `hasFileChanged()` determines if file needs upload
3. Backend uploads file successfully
4. `markFileUploaded()` updates state
5. `markFolderCompleted()` when folder done
6. `save()` persists changes

### Recovery After Interruption
1. System restarts (power loss, crash, etc.)
2. `load()` reads snapshot + replays journal
3. Upload process resumes with accurate state
4. No duplicate uploads due to accurate tracking
