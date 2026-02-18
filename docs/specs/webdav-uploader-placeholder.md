# WebDAV Uploader (Placeholder)

## Overview
The WebDAV Uploader (`WebDAVUploader.cpp/.h`) is currently a placeholder implementation intended for future support of WebDAV-compatible servers such as Nextcloud, ownCloud, and other WebDAV-enabled cloud storage.

## Current Status: NOT IMPLEMENTED

### Implementation Status
- **Configuration parsing**: ✅ Supported in Config class
- **Backend detection**: ✅ Recognized in ENDPOINT_TYPE
- **Upload logic**: ❌ Placeholder only - returns errors
- **Connection handling**: ❌ Not implemented
- **Directory creation**: ❌ Not implemented

### Placeholder Behavior
```cpp
bool WebDAVUploader::begin() {
    LOG_ERROR("[WebDAV] ERROR: WebDAV uploader not yet implemented");
    LOG_ERROR("[WebDAV] Please use SMB upload or wait for WebDAV implementation");
    return false;
}

bool WebDAVUploader::upload(const String& localPath, const String& remotePath, 
                            fs::FS &sd, unsigned long& bytesTransferred) {
    LOG_ERROR("[WebDAV] ERROR: WebDAV uploader not yet implemented");
    LOG_ERROR("[WebDAV] Please use SMB upload or wait for WebDAV implementation");
    return false;
}
```

## Planned Architecture

### WebDAV Protocol Support
- **HTTP/HTTPS**: Secure and insecure connections
- **Authentication**: Basic and Digest authentication
- **Operations**: PROPFIND, PUT, MKCOL, DELETE
- **Compliance**: WebDAV Class 1 and 2 compliance

### Integration Points
- **Configuration**: ENDPOINT_TYPE=WEBDAV, ENDPOINT=URL
- **Credentials**: Username/password authentication
- **File operations**: Upload, directory creation, listing
- **Error handling**: HTTP status code mapping

### Planned Features
- **Chunked uploads**: Support for large files
- **Resume capability**: Resume interrupted uploads
- **Directory sync**: Mirror local directory structure
- **Conflict resolution**: Handle existing files

## Configuration Format

### Basic WebDAV Configuration
```ini
ENDPOINT_TYPE = WEBDAV
ENDPOINT = https://cloud.example.com/remote.php/dav/files/username/
ENDPOINT_USER = username
ENDPOINT_PASSWORD = password
```

### Nextcloud/ownCloud Example
```ini
ENDPOINT_TYPE = WEBDAV
ENDPOINT = https://nextcloud.example.com/remote.php/dav/files/user/CPAP/
ENDPOINT_USER = user@example.com
ENDPOINT_PASSWORD = app_password
```

### Dual Backend Example
```ini
ENDPOINT_TYPE = SMB,WEBDAV
ENDPOINT = //nas.local/backups
ENDPOINT_USER = smb_user
ENDPOINT_PASSWORD = smb_pass

# WebDAV credentials would need separate handling
```

## Implementation Requirements

### Dependencies
- **HTTPClient**: For WebDAV HTTP operations
- **ArduinoJson**: For parsing WebDAV responses
- **TLS support**: For HTTPS connections
- **Authentication**: Base64 encoding for Basic auth

### Core Operations
```cpp
// Planned implementation structure
class WebDAVUploader {
private:
    String baseUrl;        // Base WebDAV URL
    String username;        // Authentication username
    String password;        // Authentication password
    bool connected;         // Connection status
    
    bool parseEndpoint(const String& endpoint);
    bool authenticate();
    String encodeBase64(const String& data);
    
public:
    bool begin();
    bool createDirectory(const String& path);
    bool upload(const String& localPath, const String& remotePath, 
                fs::FS &sd, unsigned long& bytesTransferred);
    bool isConnected() const;
};
```

### WebDAV Methods
- **PROPFIND**: Check if files/directories exist
- **PUT**: Upload files
- **MKCOL**: Create directories
- **DELETE**: Remove files/directories (optional)
- **GET**: Download files (optional, for verification)

## Security Considerations

### Authentication
- **Basic Auth**: Username/password with Base64 encoding
- **Digest Auth**: Challenge-response authentication
- **App Passwords**: For services like Nextcloud
- **Token Auth**: OAuth 2.0 (future enhancement)

### TLS Security
- **Certificate validation**: CA verification for HTTPS
- **Insecure mode**: Option for self-signed certificates
- **Certificate pinning**: Enhanced security (future)

### Data Protection
- **HTTPS preferred**: Encrypted data transmission
- **Credential storage**: Secure flash storage like other backends
- **Path validation**: Prevent directory traversal attacks

## Integration Strategy

### Configuration Integration
- **Extend Config class**: Add WebDAV-specific settings
- **Endpoint validation**: Verify WebDAV URL format
- **Credential handling**: Secure storage integration
- **Error handling**: Consistent with other backends

### FileUploader Integration
- **Backend pass**: Add WebDAV pass after SMB/Cloud
- **State management**: Integrate with UploadStateManager
- **Progress tracking**: WebStatus integration
- **Error recovery**: Consistent error handling

### Web Interface Integration
- **Status display**: WebDAV upload progress
- **Configuration**: WebDAV endpoint settings
- **Error reporting**: WebDAV-specific errors
- **Testing**: Manual upload triggers

## Development Roadmap

### Phase 1: Basic Implementation
- HTTP client setup and authentication
- Basic file upload (PUT operations)
- Directory creation (MKCOL operations)
- Error handling and status reporting

### Phase 2: Advanced Features
- Chunked uploads for large files
- Resume capability for interrupted uploads
- Directory synchronization
- Conflict resolution strategies

### Phase 3: Security & Performance
- HTTPS certificate validation
- OAuth 2.0 authentication support
- Performance optimization
- Comprehensive testing

### Phase 4: Integration & Polish
- Web interface integration
- Configuration validation
- Error message improvements
- Documentation completion

## Testing Strategy

### Test Servers
- **Nextcloud**: Local Nextcloud instance
- **ownCloud**: Local ownCloud instance
- **Generic WebDAV**: Apache mod_dav
- **Cloud services**: Commercial WebDAV providers

### Test Cases
- **Authentication**: Valid and invalid credentials
- **File operations**: Upload various file sizes
- **Directory operations**: Create nested directories
- **Error handling**: Network failures, permission errors
- **TLS security**: Certificate validation

## Current Limitations

### Functionality
- **No upload capability**: Returns errors for all operations
- **No connection handling**: Cannot establish WebDAV connections
- **Limited configuration**: Basic URL parsing only
- **No error recovery**: Placeholder error messages only

### Integration
- **FileUploader skips**: WebDAV backend ignored during upload
- **No state tracking**: UploadStateManager doesn't track WebDAV
- **No web interface**: WebDAV not visible in status pages
- **No testing**: Cannot test WebDAV functionality

## Migration Path

### For Users
1. **Current**: Use SMB or Cloud backends only
2. **Future**: Add WebDAV as third backend option
3. **Migration**: Existing configurations unaffected
4. **Documentation**: Update configuration examples

### For Developers
1. **Implementation**: Follow existing backend patterns
2. **Testing**: Comprehensive test suite required
3. **Integration**: Seamless integration with existing code
4. **Documentation**: Complete API documentation

## Notes for Implementation

When implementing WebDAV support, consider:
- **Consistent patterns**: Follow SMB/Cloud uploader patterns
- **Error handling**: Use same error reporting mechanisms
- **State management**: Integrate with existing UploadStateManager
- **Web interface**: Add WebDAV status to dashboard
- **Configuration**: Extend existing configuration system
- **Testing**: Comprehensive test coverage required
- **Security**: Prioritize HTTPS and secure authentication
