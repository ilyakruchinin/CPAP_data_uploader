#ifndef SLEEPHQ_UPLOADER_H
#define SLEEPHQ_UPLOADER_H

#include <Arduino.h>
#include <FS.h>

#ifdef ENABLE_SLEEPHQ_UPLOAD

#include <WiFiClientSecure.h>
#include "Config.h"

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
    bool connected;
    
    // TLS client
    WiFiClientSecure* tlsClient;
    
    // HTTP helpers
    bool httpRequest(const String& method, const String& path, 
                     const String& body, const String& contentType,
                     String& responseBody, int& httpCode);
    bool httpMultipartUpload(const String& path, const String& fileName,
                             const String& filePath, const String& contentHash,
                             unsigned long lockedFileSize,
                             fs::FS &sd, unsigned long& bytesTransferred,
                             String& responseBody, int& httpCode,
                             String* calculatedChecksum = nullptr,
                             bool useKeepAlive = true);
    
    // Content hash: MD5(file_content + filename)
    String computeContentHash(fs::FS &sd, const String& localPath, const String& fileName,
                               unsigned long& hashedSize);
    
    // OAuth
    bool authenticate();
    bool ensureAccessToken();
    
    // API operations
    bool discoverTeamId();
    
    // TLS setup
    void setupTLS();
    void resetTLS();
    void configureTLS();

public:
    SleepHQUploader(Config* cfg);
    ~SleepHQUploader();
    
    bool begin();
    bool upload(const String& localPath, const String& remotePath, 
                fs::FS &sd, unsigned long& bytesTransferred, String& fileChecksum);
    void end();
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
