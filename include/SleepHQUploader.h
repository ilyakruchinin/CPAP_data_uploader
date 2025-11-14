#ifndef SLEEPHQ_UPLOADER_H
#define SLEEPHQ_UPLOADER_H

#include <Arduino.h>
#include <FS.h>

#ifdef ENABLE_SLEEPHQ_UPLOAD

/**
 * SleepHQUploader - Handles direct file uploads to SleepHQ service
 * 
 * TODO: Implementation pending
 * 
 * This uploader will support direct upload to the SleepHQ cloud service
 * for CPAP data analysis and tracking.
 * 
 * Planned features:
 * - HTTPS API integration
 * - OAuth or API key authentication
 * - Automatic file format detection
 * - Metadata extraction and submission
 * - Upload progress tracking
 * - Retry logic for failed uploads
 * 
 * Requirements: 10.7
 */
class SleepHQUploader {
private:
    String apiEndpoint;    // SleepHQ API endpoint
    String apiKey;         // API key or OAuth token
    String userId;         // User ID for SleepHQ account
    bool authenticated;
    
    bool parseEndpoint(const String& endpoint);
    bool authenticate();
    void disconnect();

public:
    SleepHQUploader(const String& endpoint, const String& user, const String& apiKey);
    ~SleepHQUploader();
    
    bool begin();
    bool upload(const String& localPath, const String& remotePath, 
                fs::FS &sd, unsigned long& bytesTransferred);
    void end();
    bool isConnected() const;
    
    // Note: SleepHQ may not need createDirectory as it manages its own structure
};

#endif // ENABLE_SLEEPHQ_UPLOAD

#endif // SLEEPHQ_UPLOADER_H
