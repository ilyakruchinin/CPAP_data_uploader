#ifndef SLEEPHQ_UPLOADER_H
#define SLEEPHQ_UPLOADER_H

#include <Arduino.h>
#include <FS.h>

#ifdef ENABLE_SLEEPHQ_UPLOAD

#include <WiFiClientSecure.h>
#include "Config.h"

class SDCardManager;  // Forward declaration for SD release during network I/O

/**
 * SleepHQUploader - Uploads CPAP data to SleepHQ cloud service via REST API
 * 
 * Uses OAuth password grant for authentication (client_id + client_secret).
 * Upload flow: authenticate -> get team_id -> create import -> upload files -> process import.
 * Content dedup via MD5(file_content + filename) hash sent with each file.
 * 
 * TLS: Uses embedded ISRG Root X1 CA certificate by default.
 *       Set CLOUD_INSECURE_TLS=true to skip certificate validation.
 */
class SleepHQUploader {
private:
    Config* config;
    
    // OAuth state
    String accessToken;
    unsigned long tokenObtainedAt;  // millis() when token was obtained
    unsigned long tokenExpiresIn;   // seconds until token expires
    
    // API state
    String teamId;
    String currentImportId;
    int deviceId;  // Auto-discovered or from config
    String machineName;  // User's machine name (from /teams/{id}/machines)
    bool connected;
    
    // TLS client
    WiFiClientSecure* tlsClient;
    
    // HTTP helpers
    bool httpRequest(const String& method, const String& path, 
                     const String& body, const String& contentType,
                     String& responseBody, int& httpCode);
    
    // Single-read streaming upload with progressive hash computation.
    // Reads the file exactly once: each chunk is hashed and sent over TLS.
    // content_hash is placed after the file in the multipart body.
    // SD card is released between chunks when sdManager is provided.
    bool httpMultipartUpload(const String& path, const String& fileName,
                             const String& filePath, unsigned long fileSize,
                             fs::FS &sd, unsigned long& bytesTransferred,
                             String& responseBody, int& httpCode,
                             SDCardManager* sdManager = nullptr,
                             String* outContentHash = nullptr);
    
    // OAuth
    bool authenticate();
    bool ensureAccessToken();
    
    // API operations
    bool discoverTeamId();
    bool discoverDeviceId();
    bool discoverMachineInfo();
    
    // TLS setup
    void setupTLS();

public:
    SleepHQUploader(Config* cfg);
    ~SleepHQUploader();
    
    bool begin();
    bool upload(const String& localPath, const String& remotePath, 
                fs::FS &sd, unsigned long& bytesTransferred,
                SDCardManager* sdManager = nullptr);
    
    // Upload from pre-buffered data (no SD access needed).
    // Used by batch upload to avoid per-file SD mount/unmount overhead.
    bool uploadFromBuffer(const uint8_t* fileData, size_t fileSize,
                          const String& fileName, const String& filePath,
                          unsigned long& bytesTransferred);
    
    void end();
    void disconnectTls();  // Free TLS buffers (~32KB) without ending the session
    bool isConnected() const;
    
    // Import session management (called by FileUploader)
    bool createImport();
    bool processImport();
    
    // Status getters
    const String& getTeamId() const;
    const String& getCurrentImportId() const;
    unsigned long getTokenRemainingSeconds() const;
};

#endif // ENABLE_SLEEPHQ_UPLOAD

#endif // SLEEPHQ_UPLOADER_H
