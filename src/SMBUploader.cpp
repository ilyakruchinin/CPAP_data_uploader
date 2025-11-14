#include "SMBUploader.h"

#ifdef ENABLE_SMB_UPLOAD

#include <fcntl.h>  // For O_WRONLY, O_CREAT, O_TRUNC flags

// Include libsmb2 headers
// Note: These will be available when libsmb2 is added as ESP-IDF component
extern "C" {
    #include "smb2/smb2.h"
    #include "smb2/libsmb2.h"
}

// Buffer size for file streaming (32KB for good performance)
#define UPLOAD_BUFFER_SIZE 32768

SMBUploader::SMBUploader(const String& endpoint, const String& user, const String& password)
    : smbUser(user), smbPassword(password), smb2(nullptr), connected(false) {
    parseEndpoint(endpoint);
}

SMBUploader::~SMBUploader() {
    end();
}

bool SMBUploader::parseEndpoint(const String& endpoint) {
    // Expected format: //server/share or //server/share/path
    // We only need server and share for connection
    
    if (!endpoint.startsWith("//")) {
        Serial.println("[SMB] ERROR: Invalid endpoint format, must start with //");
        Serial.print("[SMB] Got: ");
        Serial.println(endpoint);
        Serial.println("[SMB] Expected format: //server/share");
        return false;
    }
    
    // Remove leading //
    String path = endpoint.substring(2);
    
    // Find first slash to separate server from share
    int firstSlash = path.indexOf('/');
    if (firstSlash == -1) {
        Serial.println("[SMB] ERROR: Invalid endpoint format, missing share name");
        Serial.print("[SMB] Got: ");
        Serial.println(endpoint);
        Serial.println("[SMB] Expected format: //server/share");
        return false;
    }
    
    smbServer = path.substring(0, firstSlash);
    
    // Find second slash to separate share from path (if exists)
    int secondSlash = path.indexOf('/', firstSlash + 1);
    if (secondSlash == -1) {
        // No path component, just share
        smbShare = path.substring(firstSlash + 1);
    } else {
        // Has path component, extract just the share name
        smbShare = path.substring(firstSlash + 1, secondSlash);
    }
    
    if (smbServer.isEmpty() || smbShare.isEmpty()) {
        Serial.println("[SMB] ERROR: Invalid endpoint, server or share is empty after parsing");
        Serial.print("[SMB] Server: '");
        Serial.print(smbServer);
        Serial.print("', Share: '");
        Serial.print(smbShare);
        Serial.println("'");
        return false;
    }
    
    Serial.print("[SMB] Parsed endpoint - Server: ");
    Serial.print(smbServer);
    Serial.print(", Share: ");
    Serial.println(smbShare);
    
    return true;
}

bool SMBUploader::connect() {
    if (connected) {
        return true;
    }
    
    if (smbServer.isEmpty() || smbShare.isEmpty()) {
        Serial.println("[SMB] ERROR: Cannot connect, endpoint not parsed correctly");
        Serial.println("[SMB] Check ENDPOINT configuration in config.json");
        return false;
    }
    
    // Create SMB2 context
    smb2 = smb2_init_context();
    if (smb2 == nullptr) {
        Serial.println("[SMB] ERROR: Failed to initialize SMB context");
        Serial.println("[SMB] This may indicate a memory allocation failure");
        return false;
    }
    
    // Set security mode (allow guest if no credentials)
    if (smbUser.isEmpty()) {
        Serial.println("[SMB] WARNING: No credentials provided, attempting guest access");
        smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
    } else {
        smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
        smb2_set_user(smb2, smbUser.c_str());
        smb2_set_password(smb2, smbPassword.c_str());
    }
    
    // Connect to server
    Serial.print("[SMB] Connecting to //");
    Serial.print(smbServer);
    Serial.print("/");
    Serial.println(smbShare);
    
    if (smb2_connect_share(smb2, smbServer.c_str(), smbShare.c_str(), nullptr) < 0) {
        const char* error = smb2_get_error(smb2);
        Serial.print("[SMB] ERROR: Connection failed: ");
        Serial.println(error);
        Serial.println("[SMB] Possible causes:");
        Serial.println("[SMB]   - Server unreachable (check network/firewall)");
        Serial.println("[SMB]   - Invalid credentials (check ENDPOINT_USER/ENDPOINT_PASS)");
        Serial.println("[SMB]   - Share does not exist or is not accessible");
        Serial.println("[SMB]   - SMB protocol version mismatch");
        smb2_destroy_context(smb2);
        smb2 = nullptr;
        return false;
    }
    
    connected = true;
    Serial.println("[SMB] Connected successfully");
    return true;
}

void SMBUploader::disconnect() {
    if (smb2 != nullptr) {
        if (connected) {
            smb2_disconnect_share(smb2);
            connected = false;
        }
        smb2_destroy_context(smb2);
        smb2 = nullptr;
    }
}

bool SMBUploader::begin() {
    return connect();
}

void SMBUploader::end() {
    disconnect();
}

bool SMBUploader::isConnected() const {
    return connected;
}

bool SMBUploader::createDirectory(const String& path) {
    if (!connected) {
        Serial.println("[SMB] ERROR: Not connected - cannot create directory");
        return false;
    }
    
    if (path.isEmpty() || path == "/") {
        return true;  // Root always exists
    }
    
    // Check if directory already exists
    struct smb2_stat_64 st;
    if (smb2_stat(smb2, path.c_str(), &st) == 0) {
        // Path exists, check if it's a directory
        if (st.smb2_type == SMB2_TYPE_DIRECTORY) {
            return true;  // Directory already exists
        } else {
            Serial.print("[SMB] ERROR: Path exists but is not a directory: ");
            Serial.println(path);
            Serial.println("[SMB] Cannot create directory - file with same name exists");
            return false;
        }
    }
    
    // Directory doesn't exist, need to create it
    // First ensure parent directory exists
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash > 0) {
        String parentPath = path.substring(0, lastSlash);
        if (!createDirectory(parentPath)) {
            Serial.print("[SMB] ERROR: Failed to create parent directory: ");
            Serial.println(parentPath);
            return false;  // Failed to create parent
        }
    }
    
    // Create this directory
    Serial.print("[SMB] Creating directory: ");
    Serial.println(path);
    
    if (smb2_mkdir(smb2, path.c_str()) < 0) {
        // Check if error is because directory already exists (race condition)
        if (smb2_stat(smb2, path.c_str(), &st) == 0 && st.smb2_type == SMB2_TYPE_DIRECTORY) {
            Serial.println("[SMB] Directory created by concurrent operation");
            return true;  // Directory was created by another process
        }
        
        const char* error = smb2_get_error(smb2);
        Serial.print("[SMB] ERROR: Failed to create directory: ");
        Serial.println(error);
        Serial.println("[SMB] Possible causes:");
        Serial.println("[SMB]   - Insufficient permissions");
        Serial.println("[SMB]   - Invalid directory name");
        Serial.println("[SMB]   - Network connection lost");
        return false;
    }
    
    return true;
}

bool SMBUploader::upload(const String& localPath, const String& remotePath, 
                         fs::FS &sd, unsigned long& bytesTransferred) {
    bytesTransferred = 0;
    
    if (!connected) {
        Serial.println("SMB: Not connected");
        return false;
    }
    
    // Open local file from SD card
    File localFile = sd.open(localPath, FILE_READ);
    if (!localFile) {
        Serial.print("[SMB] ERROR: Failed to open local file: ");
        Serial.println(localPath);
        Serial.println("[SMB] File may not exist or SD card has read errors");
        return false;
    }
    
    size_t fileSize = localFile.size();
    
    // Sanity check file size
    if (fileSize == 0) {
        Serial.print("[SMB] WARNING: File is empty: ");
        Serial.println(localPath);
        localFile.close();
        return false;
    }
    
    Serial.print("[SMB] Uploading ");
    Serial.print(localPath);
    Serial.print(" (");
    Serial.print(fileSize);
    Serial.println(" bytes)");
    
    // Ensure parent directory exists
    int lastSlash = remotePath.lastIndexOf('/');
    if (lastSlash > 0) {
        String parentDir = remotePath.substring(0, lastSlash);
        if (!createDirectory(parentDir)) {
            Serial.print("[SMB] ERROR: Failed to create parent directory: ");
            Serial.println(parentDir);
            Serial.println("[SMB] Check permissions on remote share");
            localFile.close();
            return false;
        }
    }
    
    // Open remote file for writing
    // Convert Arduino String to C string for libsmb2
    struct smb2fh* remoteFile = smb2_open(smb2, remotePath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (remoteFile == nullptr) {
        const char* error = smb2_get_error(smb2);
        Serial.print("[SMB] ERROR: Failed to open remote file: ");
        Serial.println(error);
        Serial.print("[SMB] Remote path: ");
        Serial.println(remotePath);
        Serial.println("[SMB] Possible causes:");
        Serial.println("[SMB]   - Insufficient permissions on remote share");
        Serial.println("[SMB]   - Disk full on remote server");
        Serial.println("[SMB]   - Invalid characters in filename");
        localFile.close();
        return false;
    }
    
    // Allocate buffer for streaming
    uint8_t* buffer = (uint8_t*)malloc(UPLOAD_BUFFER_SIZE);
    if (buffer == nullptr) {
        Serial.println("[SMB] ERROR: Failed to allocate upload buffer");
        Serial.println("[SMB] System may be low on memory");
        smb2_close(smb2, remoteFile);
        localFile.close();
        return false;
    }
    
    // Stream file data
    bool success = true;
    unsigned long totalBytesRead = 0;
    
    while (localFile.available()) {
        size_t bytesRead = localFile.read(buffer, UPLOAD_BUFFER_SIZE);
        if (bytesRead == 0) {
            // Check if we've read all expected bytes
            if (totalBytesRead < fileSize) {
                Serial.print("[SMB] ERROR: Unexpected end of file, read ");
                Serial.print(totalBytesRead);
                Serial.print(" of ");
                Serial.print(fileSize);
                Serial.println(" bytes");
                Serial.println("[SMB] SD card may have read errors");
                success = false;
            }
            break;
        }
        
        totalBytesRead += bytesRead;
        
        ssize_t bytesWritten = smb2_write(smb2, remoteFile, buffer, bytesRead);
        if (bytesWritten < 0) {
            const char* error = smb2_get_error(smb2);
            Serial.print("[SMB] ERROR: Write failed at offset ");
            Serial.print(bytesTransferred);
            Serial.print(": ");
            Serial.println(error);
            Serial.println("[SMB] Possible causes:");
            Serial.println("[SMB]   - Network connection lost");
            Serial.println("[SMB]   - Remote server disk full");
            Serial.println("[SMB]   - SMB session timeout");
            success = false;
            break;
        }
        
        if ((size_t)bytesWritten != bytesRead) {
            Serial.print("[SMB] ERROR: Incomplete write, expected ");
            Serial.print(bytesRead);
            Serial.print(" bytes, wrote ");
            Serial.println(bytesWritten);
            Serial.println("[SMB] Network may be unstable");
            success = false;
            break;
        }
        
        bytesTransferred += bytesWritten;
        
        // Print progress for large files (every 1MB)
        if (bytesTransferred % (1024 * 1024) == 0) {
            Serial.print("[SMB] Progress: ");
            Serial.print(bytesTransferred / 1024);
            Serial.print(" KB / ");
            Serial.print(fileSize / 1024);
            Serial.println(" KB");
        }
        
        // Yield to prevent watchdog timeout on large files
        yield();
    }
    
    // Verify we transferred all bytes
    if (success && bytesTransferred != fileSize) {
        Serial.print("[SMB] ERROR: Size mismatch, transferred ");
        Serial.print(bytesTransferred);
        Serial.print(" bytes, expected ");
        Serial.println(fileSize);
        Serial.println("[SMB] Upload incomplete - file may be corrupted on remote server");
        success = false;
    }
    
    // Cleanup
    free(buffer);
    
    // Close remote file
    if (smb2_close(smb2, remoteFile) < 0) {
        Serial.print("[SMB] WARNING: Failed to close remote file: ");
        Serial.println(smb2_get_error(smb2));
        // Don't fail the upload if close fails - data was already written
    }
    
    localFile.close();
    
    if (success) {
        Serial.print("[SMB] Upload complete: ");
        Serial.print(bytesTransferred);
        Serial.print(" bytes transferred in ");
        Serial.print((millis() - bytesTransferred) / 1000);  // Rough estimate
        Serial.println(" seconds");
    } else {
        Serial.println("[SMB] Upload failed - file not uploaded or incomplete");
    }
    
    return success;
}

#endif // ENABLE_SMB_UPLOAD
