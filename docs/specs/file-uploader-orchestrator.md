# File Uploader Orchestrator

## Overview
The File Uploader (`FileUploader.cpp/.h`) is the central orchestrator that coordinates all upload operations across multiple backends (SMB, Cloud, WebDAV). It manages upload state, performs pre-flight scans, and handles the complete upload lifecycle.

## Core Architecture

### Backend Pass Strategy
The uploader processes backends in stages to optimize memory usage:
1. **SMB Pass** - While heap is fresh (max_alloc ~73KB)
2. **Cloud Pass** - After SMB teardown, with optimized TLS handling

### Pre-flight Scans
Before any network activity, performs SD-only scans:
```cpp
bool smbHasWork = !folders.empty() || mandatoryChanged;
if (!smbHasWork) {
    LOG("[FileUploader] SMB: nothing to upload — skipping");
}
```

## Key Features

### Intelligent Folder Scanning
- **Recent completed folders**: Only included if files changed size
- **Old completed folders**: Skipped entirely
- **Pending folders**: Tracked for when they acquire content
- **Fresh vs Old data**: Different scheduling rules

### Multi-Backend Coordination
- **SMBUploader**: Network share uploads with transport resilience
- **SleepHQUploader**: Cloud uploads with OAuth and import sessions
- **WebDAVUploader**: Placeholder for future implementation

### Upload State Management
- **UploadStateManager**: Tracks file/folder completion status
- **Snapshot + Journal**: Efficient state persistence
- **Size-only tracking**: Optimized for recent DATALOG files
- **Checksum tracking**: For mandatory/SETTINGS files

### Memory Optimization
- **Staged passes**: SMB then Cloud to isolate memory pressure
- **Buffer management**: Dynamic SMB buffer sizing based on heap
- **TLS reuse**: Persistent connections for cloud operations

## Upload Process Flow

### 1. Initialization
```cpp
bool begin(fs::FS &sd) {
    // Create state managers for each backend
    smbStateManager = new UploadStateManager(...);
    cloudStateManager = new UploadStateManager(...);
    
    // Create uploaders
    smbUploader = new SMBUploader(...);
    sleephqUploader = new SleepHQUploader(...);
}
```

### 2. Upload Execution
```cpp
void uploadWithExclusiveAccess(fs::FS &sd, DataFilter filter) {
    // SMB Pass
    if (smbHasWork) {
        uploadMandatoryFilesSmb(...);
        for (folder : folders) {
            uploadDatalogFolderSmb(...);
        }
    }
    
    // Cloud Pass  
    if (cloudHasWork) {
        sleephqUploader->begin(); // OAuth + team + import
        for (folder : folders) {
            uploadDatalogFolderCloud(...);
        }
        finalizeCloudImport();
    }
}
```

### 3. File Upload Logic
- **Mandatory files**: Identification.*, STR.edf, SETTINGS/
- **DATALOG folders**: Date-named folders with therapy data
- **Change detection**: Size comparison for recent files, checksums for critical files
- **Progress tracking**: Real-time status updates via WebStatus

## Advanced Features

### Transport Resilience (SMB)
- **Recoverable errors**: EAGAIN, EWOULDBLOCK, "Wrong signature"
- **WiFi cycling**: Reclaim poisoned sockets
- **Retry logic**: Up to 3 connect attempts with backoff
- **Connection reuse**: Per-folder to avoid socket exhaustion

### Cloud Import Management
- **Lazy import creation**: Only when files exist
- **Session reuse**: OAuth + team + import in one TLS session
- **Streaming uploads**: Stack-allocated multipart buffers
- **Low-memory handling**: Graceful degradation when max_alloc < 40KB

### Empty Folder Handling
- **7-day waiting**: Before marking empty folders complete
- **Pending tracking**: Monitors folders that acquire content
- **Automatic promotion**: From pending to normal processing

## Configuration Integration
- **Schedule Manager**: Enforces upload windows for old data
- **Traffic Monitor**: Detects SD bus activity for smart mode
- **Web Server**: Real-time progress monitoring
- **WiFi Manager**: Network connectivity for cloud operations

## Error Handling
- **Graceful degradation**: Skip failed backends, continue with others
- **State preservation**: Always save progress even on failures
- **Retry mechanisms**: Built into each backend
- **Timeout protection**: Per-file and per-session timeouts

## Performance Optimizations
- **Pre-flight gating**: No network if nothing to upload
- **Bulk operations**: Directory creation, batch uploads
- **Memory awareness**: Buffer sizing based on available heap
- **Connection reuse**: Persistent sessions where possible

## Integration Points
- **UploadFSM**: Main state machine calls uploadWithExclusiveAccess
- **SDCardManager**: Provides SD access control
- **Config**: Supplies all backend and timing parameters
- **WebStatus**: Real-time progress reporting
- **All Backend Uploaders**: Orchestrates their operations
