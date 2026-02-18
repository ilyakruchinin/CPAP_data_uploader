# SMB Uploader

## Overview
The SMB Uploader (`SMBUploader.cpp/.h`) handles file uploads to Windows shares, NAS devices, and Samba servers using the libsmb2 library. Features advanced transport resilience and automatic recovery mechanisms.

## Architecture

### libsmb2 Integration
- **Synchronous API**: Uses blocking calls with timeout configuration
- **Connection reuse**: Per-folder connections to avoid socket exhaustion
- **Buffer management**: Dynamic buffer sizing based on heap availability

### Transport Layer
```cpp
// libsmb2 context configuration
smb2_set_timeout(smb2, SMB_COMMAND_TIMEOUT_SECONDS);
smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_REQUIRED);
```

## Key Features

### Transport Resilience
```cpp
static bool isRecoverableSmbWriteError(int errorCode, const char* smbError) {
    // Network-level recoverable errors
    if (errorCode == ETIMEDOUT || errorCode == ENETRESET || 
        errorCode == EAGAIN || errorCode == EWOULDBLOCK) {
        return true;
    }
    
    // SMB-level recoverable errors
    if (smbError && (strstr(smbError, "Wrong signature") || 
                     strstr(smbError, "Wrong signature in received PDU"))) {
        return true;
    }
    return false;
}
```

### WiFi Recovery
```cpp
bool recoverWiFiAfterSmbTransportFailure() {
    // Cycle WiFi connection to reclaim poisoned sockets
    WiFi.disconnect();
    delay(1000);
    WiFi.reconnect();
    
    // Wait for reconnection with heartbeat updates
    uint32_t start = millis();
    while (!WiFi.connected() && (millis() - start) < 10000) {
        feedUploadHeartbeat();
        delay(100);
    }
    return WiFi.connected();
}
```

### Connection Management
- **Lazy connection**: Only connects when first file upload needed
- **Per-folder reuse**: Maintains connection across multiple files in same folder
- **Graceful teardown**: Proper cleanup on errors and completion

## Upload Process Flow

### 1. Connection Establishment
```cpp
bool begin() {
    if (!isConnected()) {
        return connect();
    }
    return true;
}
```

### 2. File Upload
```cpp
bool upload(const String& localPath, const String& remotePath, 
            fs::FS &sd, unsigned long& bytesTransferred) {
    // Open local file
    File file = sd.open(localPath, FILE_READ);
    
    // Create remote file
    smb2_file* remote = smb2_open(smb2, remotePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    
    // Stream data with recovery
    while (file.available()) {
        bytesRead = file.read(buffer, bufferSize);
        bytesWritten = smb2_write(smb2, remote, buffer, bytesRead);
        
        // Handle recoverable errors
        if (bytesWritten < 0 && isRecoverableSmbWriteError(errno, smb2_get_error(smb2))) {
            if (attemptRecovery()) {
                continue; // Retry the write
            }
        }
    }
}
```

### 3. Error Recovery
```cpp
bool attemptRecovery() {
    if (needsWifiRecovery) {
        recoverWiFiAfterSmbTransportFailure();
    }
    
    // Reconnect with up to 3 attempts
    for (int attempt = 0; attempt < 3; attempt++) {
        if (reconnect()) {
            return true;
        }
        delay(200 * (attempt + 1)); // 200/400/600ms backoff
    }
    return false;
}
```

## Advanced Features

### Dynamic Buffer Sizing
```cpp
uint32_t maxAlloc = ESP.getMaxAllocHeap();
if (maxAlloc > 50000) smbBufferSize = 8192;
else if (maxAlloc > 30000) smbBufferSize = 2048;
else smbBufferSize = 1024;
```

### Directory Creation
- **Automatic**: Creates remote directories as needed
- **Recursive**: Creates parent directories if missing
- **Error handling**: Continues upload if directory creation fails

### Progress Tracking
- **Byte-level**: Accurate transfer progress reporting
- **Heartbeat updates**: Prevents watchdog timeouts during large transfers
- **Web status**: Real-time progress via WebStatus integration

## Configuration

### Connection Parameters
- **Server**: From `ENDPOINT` configuration (e.g., `//192.168.1.100/share`)
- **Credentials**: Username/password from `ENDPOINT_USER`/`ENDPOINT_PASSWORD`
- **Timeout**: 15 seconds for SMB commands
- **Security**: SMB2 signing required

### Performance Tuning
- **Buffer sizes**: 8KB/4KB/1KB based on heap availability
- **Retry limits**: 2 attempts per file, 3 reconnect attempts
- **Backoff strategy**: Incremental delays for retries

## Error Handling

### Recoverable Errors
- **Network timeouts**: ETIMEDOUT, ENETRESET
- **Connection issues**: ECONNRESET, ENOTCONN, EPIPE
- **Resource exhaustion**: EAGAIN, EWOULDBLOCK
- **SMB protocol errors**: "Wrong signature" messages

### Fatal Errors
- **Authentication failures**: Invalid credentials
- **Permission errors**: Access denied
- **Filesystem errors**: Path not found, disk full
- **Unrecoverable transport**: Multiple failed recovery attempts

### Recovery Strategies
1. **Skip close**: Don't call `smb2_close` on poisoned transports
2. **WiFi cycle**: Reclaim network sockets
3. **Context reconnect**: Fresh SMB connection
4. **File retry**: Attempt same file once after recovery
5. **Abort**: Give up after multiple failed attempts

## Performance Characteristics

### Throughput
- **Typical**: 1-3 MB/s over WiFi (depends on network)
- **Limited by**: WiFi bandwidth and SMB protocol overhead
- **Optimized**: Streaming transfers, no in-memory buffering

### Memory Usage
- **Base**: ~2KB for SMB context
- **Buffer**: 1-8KB depending on heap availability
- **Peak**: During connection establishment

### Latency
- **Connection**: 100-500ms (depends on server)
- **File creation**: 10-50ms per file
- **Data transfer**: Near network line rate

## Integration Points

### FileUploader Integration
- **Orchestration**: Called by FileUploader for SMB files
- **State management**: Updates UploadStateManager on success
- **Progress reporting**: Provides real-time status updates

### Configuration Integration
- **Endpoint parsing**: Uses Config for server and credentials
- **Performance tuning**: Adapts to heap conditions
- **Security settings**: Implements required SMB signing

### Web Interface
- **Status updates**: Real-time upload progress
- **Error reporting**: Detailed error messages
- **Control**: Manual upload triggers

## Debugging Features

### Detailed Logging
- **Connection state**: All SMB connection events
- **Transfer progress**: Bytes transferred, file names
- **Error details**: errno values, SMB error strings
- **Recovery attempts**: All recovery actions logged

### Diagnostic Information
- **Network status**: WiFi connectivity during uploads
- **Heap monitoring**: Buffer sizing decisions
- **Performance metrics**: Transfer rates, timing data
