# SleepHQ Cloud Uploader

## Overview
The SleepHQ Cloud Uploader (`SleepHQUploader.cpp/.h`) handles secure file uploads to the SleepHQ cloud service using OAuth authentication and REST API integration. Features streaming uploads, TLS optimization, and low-memory handling.

## Architecture

### API Integration
- **Base URL**: `https://sleephq.com/api/v1`
- **Authentication**: OAuth 2.0 with client credentials
- **TLS Security**: GTS Root R4 CA certificate validation
- **Import Sessions**: Batch file uploads with process-once semantics

### Connection Strategy
- **Persistent TLS**: Reuse connection across OAuth, team discovery, and import
- **Streaming uploads**: 4KB stack buffers to avoid heap fragmentation
- **Low-memory mode**: Graceful degradation when `max_alloc < 40KB`

## Core Features

### OAuth Authentication
```cpp
bool authenticate() {
    // POST /oauth/token with client credentials
    String postData = "grant_type=client_credentials&" +
                     "client_id=" + config->getCloudClientId() + "&" +
                     "client_secret=" + config->getCloudClientSecret();
    
    // Parse JSON response for access token
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, response);
    accessToken = doc["access_token"].as<String>();
    tokenExpiresIn = doc["expires_in"].as<int>();
}
```

### Team Discovery
```cpp
bool discoverTeam() {
    // GET /v1/me to get current team ID
    // GET /v1/teams/{id}/machines to find machine
    // Match by serial number or create new machine entry
}
```

### Import Session Management
```cpp
bool createImport() {
    // POST /v1/imports with machine_id
    // Returns import_id for subsequent file uploads
    // Associates all files with therapy session
}
```

## Upload Process Flow

### 1. Session Initialization
```cpp
bool begin() {
    if (!authenticate()) return false;
    if (!discoverTeam()) return false;
    if (!createImport()) return false;
    return true;
}
```

### 2. File Upload
```cpp
bool upload(const String& localPath, const String& remotePath, 
            fs::FS &sd, unsigned long& bytesTransferred) {
    // Calculate MD5 hash: MD5(file_content + filename)
    String contentHash = calculateFileHash(sd, localPath, remotePath);
    
    // Streaming multipart upload
    return httpMultipartUpload("/api/v1/imports/" + importId + "/process_files",
                              localPath, remotePath, contentHash);
}
```

### 3. Import Finalization
```cpp
bool finalizeImport() {
    // POST /v1/imports/{id}/process
    // Triggers data processing on SleepHQ servers
    // Only called if files were actually uploaded
}
```

## Advanced Features

### Streaming Multipart Uploads
```cpp
bool httpMultipartUpload(const String& path, const String& localPath,
                        const String& remotePath, const String& contentHash) {
    // Stack-allocated buffers to prevent heap churn
    char boundary[32];
    char head1[256], head2[256], head3[256];
    char foot1[128], foot2[128];
    
    // Stream file directly without loading into memory
    while (file.available()) {
        int bytesRead = file.read(buffer, CLOUD_UPLOAD_BUFFER_SIZE);
        client.write(buffer, bytesRead);
        feedUploadHeartbeat();
    }
}
```

### Low-Memory Handling
```cpp
bool setupTLS() {
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    if (!tlsClient->connected() && maxAlloc < 36000) {
        LOG_ERROR("Insufficient contiguous heap for SSL");
        return false;
    }
    return tlsClient->connect(host, 443);
}
```

### Connection Resilience
- **WiFi cycling**: Reclaim poisoned sockets
- **TLS reuse**: Persistent connections when possible
- **Retry logic**: Automatic retry on network failures
- **Graceful degradation**: Skip operations when memory insufficient

## Configuration

### Authentication
- **Client ID**: From `CLOUD_CLIENT_ID` configuration
- **Client Secret**: From `CLOUD_CLIENT_SECRET` configuration
- **Base URL**: `https://sleephq.com/api/v1`
- **TLS Mode**: Secure with CA validation (configurable insecure mode)

### Performance Tuning
- **Buffer size**: 4KB streaming buffer
- **Timeout**: 30 seconds for HTTP operations
- **Retry limits**: 3 attempts per operation
- **Memory threshold**: 40KB minimum for SSL operations

## File Handling

### Hash Calculation
```cpp
String calculateFileHash(fs::FS &sd, const String& localPath, const String& remotePath) {
    // MD5(file_content + filename) as required by SleepHQ API
    MD5Builder md5;
    md5.begin();
    
    // Hash file content
    File file = sd.open(localPath, FILE_READ);
    while (file.available()) {
        md5.add(file.read());
    }
    
    // Add filename and finalize
    md5.add(remotePath.c_str(), remotePath.length());
    md5.calculate();
    return md5.toString();
}
```

### Path Format
- **DATALOG folders**: `"./DATALOG/20241114/"` (note leading `./` and trailing `/`)
- **Root files**: `"./"` (root directory)
- **SETTINGS files**: `"./SETTINGS/"` (settings directory)

## Error Handling

### Authentication Errors
- **Invalid credentials**: 401 Unauthorized
- **Expired token**: Automatic re-authentication
- **Network failures**: Retry with WiFi cycling

### Upload Errors
- **File not found**: Skip and continue
- **Hash mismatch**: Log warning, continue upload
- **Import errors**: Create new import session

### Recovery Strategies
1. **Token refresh**: Automatic on 401 errors
2. **WiFi cycle**: On connection failures
3. **Import recreation**: On session corruption
4. **Graceful skip**: Continue with other files

## Performance Characteristics

### Throughput
- **Typical**: 500KB/s - 2MB/s (depends on internet connection)
- **Limited by**: Internet bandwidth and API server capacity
- **Optimized**: Streaming transfers, minimal memory usage

### Memory Usage
- **Base**: ~8KB for TLS client and buffers
- **Peak**: During TLS handshake (~40KB contiguous needed)
- **Streaming**: Constant 4KB buffer regardless of file size

### Latency
- **Authentication**: 1-3 seconds
- **Team discovery**: 500ms - 1 second
- **File upload**: Depends on size and network
- **Import finalization**: 1-2 seconds

## Integration Points

### FileUploader Integration
- **Cloud pass**: Called after SMB pass completion
- **Pre-flight scanning**: Only authenticates if files need upload
- **State management**: Updates UploadStateManager on success

### Configuration Integration
- **Credentials**: OAuth client ID/secret
- **TLS settings**: CA validation, insecure mode option
- **Performance**: Buffer sizes, timeout values

### Web Interface
- **Progress tracking**: Real-time upload status
- **Error reporting**: Detailed API error messages
- **Manual control**: Force upload triggers

## Debugging Features

### Detailed Logging
- **API calls**: All HTTP requests and responses
- **Authentication**: Token acquisition and refresh
- **Upload progress**: File-by-file progress
- **Memory status**: Heap usage during operations

### Diagnostic Information
- **TLS status**: Connection state and certificate validation
- **API responses**: Parsed error messages and data
- **Performance metrics**: Transfer rates and timing

## Security Considerations

### Credential Protection
- **Secure storage**: Client secret stored in ESP32 flash
- **TLS encryption**: All communication encrypted
- **Certificate validation**: CA pinning for sleephq.com

### Data Privacy
- **Hash verification**: Ensures file integrity
- **Secure transmission**: HTTPS only
- **No local storage**: Sensitive data not logged
