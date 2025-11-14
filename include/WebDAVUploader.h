#ifndef WEBDAV_UPLOADER_H
#define WEBDAV_UPLOADER_H

#include <Arduino.h>
#include <FS.h>

#ifdef ENABLE_WEBDAV_UPLOAD

/**
 * WebDAVUploader - Handles file uploads to WebDAV servers
 * 
 * TODO: Implementation pending
 * 
 * This uploader will support uploading files to WebDAV-compatible servers
 * such as Nextcloud, ownCloud, or standard WebDAV shares.
 * 
 * Planned features:
 * - HTTP/HTTPS support
 * - Basic and Digest authentication
 * - Automatic directory creation
 * - Chunked upload for large files
 * - Resume capability for interrupted uploads
 * 
 * Requirements: 10.6
 */
class WebDAVUploader {
private:
    String webdavUrl;      // Full WebDAV URL (e.g., https://cloud.example.com/remote.php/dav/files/user/)
    String webdavUser;     // Username for authentication
    String webdavPassword; // Password for authentication
    bool connected;
    
    bool parseEndpoint(const String& endpoint);
    bool connect();
    void disconnect();

public:
    WebDAVUploader(const String& endpoint, const String& user, const String& password);
    ~WebDAVUploader();
    
    bool begin();
    bool createDirectory(const String& path);
    bool upload(const String& localPath, const String& remotePath, 
                fs::FS &sd, unsigned long& bytesTransferred);
    void end();
    bool isConnected() const;
};

#endif // ENABLE_WEBDAV_UPLOAD

#endif // WEBDAV_UPLOADER_H
