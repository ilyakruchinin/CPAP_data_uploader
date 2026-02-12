#include "SleepHQUploader.h"
#include "Logger.h"

#ifdef ENABLE_SLEEPHQ_UPLOAD

#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp32/rom/md5_hash.h>

// GTS Root R4 - Google Trust Services root CA certificate (expires June 22, 2036)
// Used for TLS validation of sleephq.com (which uses Google Trust Services)
// If sleephq.com changes CA provider, set CLOUD_INSECURE_TLS=true as fallback
static const char* GTS_ROOT_R4_CA = \
    "-----BEGIN CERTIFICATE-----\n" \
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n" \
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n" \
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n" \
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n" \
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n" \
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n" \
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n" \
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n" \
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n" \
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n" \
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n" \
    "-----END CERTIFICATE-----\n";

// Upload buffer size for streaming files
#define CLOUD_UPLOAD_BUFFER_SIZE 4096

SleepHQUploader::SleepHQUploader(Config* cfg)
    : config(cfg),
      tokenObtainedAt(0),
      tokenExpiresIn(0),
      connected(false),
      tlsClient(nullptr) {
}

SleepHQUploader::~SleepHQUploader() {
    end();
    if (tlsClient) {
        delete tlsClient;
        tlsClient = nullptr;
    }
}

void SleepHQUploader::setupTLS() {
    if (!tlsClient) {
        tlsClient = new WiFiClientSecure();
    }
    
    if (config->getCloudInsecureTls()) {
        LOG_WARN("[SleepHQ] TLS certificate validation DISABLED (insecure mode)");
        tlsClient->setInsecure();
    } else {
        LOG_DEBUG("[SleepHQ] Using GTS Root R4 CA certificate for TLS validation");
        tlsClient->setCACert(GTS_ROOT_R4_CA);
    }
    
    // Set reasonable timeout for ESP32
    tlsClient->setTimeout(15);  // 15 seconds
}

bool SleepHQUploader::begin() {
    LOG("[SleepHQ] Initializing cloud uploader...");
    
    // Setup TLS
    setupTLS();
    
    // Authenticate
    if (!authenticate()) {
        LOG_ERROR("[SleepHQ] Authentication failed");
        return false;
    }
    
    // Discover team_id if not configured
    String configTeamId = config->getCloudTeamId();
    if (!configTeamId.isEmpty()) {
        teamId = configTeamId;
        LOGF("[SleepHQ] Using configured team ID: %s", teamId.c_str());
    } else {
        if (!discoverTeamId()) {
            LOG_ERROR("[SleepHQ] Failed to discover team ID");
            return false;
        }
    }
    
    connected = true;
    LOG("[SleepHQ] Cloud uploader initialized successfully");
    return true;
}

bool SleepHQUploader::authenticate() {
    LOG("[SleepHQ] Authenticating with OAuth...");
    
    String baseUrl = config->getCloudBaseUrl();
    String tokenPath = "/oauth/token";
    
    // Build OAuth request body
    String body = "grant_type=password";
    body += "&client_id=" + config->getCloudClientId();
    body += "&client_secret=" + config->getCloudClientSecret();
    body += "&scope=read+write";
    
    String responseBody;
    int httpCode;
    
    if (!httpRequest("POST", tokenPath, body, "application/x-www-form-urlencoded", responseBody, httpCode)) {
        LOG_ERROR("[SleepHQ] OAuth request failed");
        return false;
    }
    
    if (httpCode != 200) {
        LOG_ERRORF("[SleepHQ] OAuth failed with HTTP %d", httpCode);
        LOG_ERRORF("[SleepHQ] Response: %s", responseBody.c_str());
        return false;
    }
    
    // Parse response
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, responseBody);
    if (error) {
        LOG_ERRORF("[SleepHQ] Failed to parse OAuth response: %s", error.c_str());
        return false;
    }
    
    accessToken = doc["access_token"].as<String>();
    tokenExpiresIn = doc["expires_in"] | 7200;
    tokenObtainedAt = millis();
    
    if (accessToken.isEmpty()) {
        LOG_ERROR("[SleepHQ] No access token in OAuth response");
        return false;
    }
    
    LOGF("[SleepHQ] Authenticated successfully (token expires in %lu seconds)", tokenExpiresIn);
    return true;
}

bool SleepHQUploader::ensureAccessToken() {
    if (accessToken.isEmpty()) {
        return authenticate();
    }
    
    // Check if token has expired (with 60 second safety margin)
    unsigned long elapsed = (millis() - tokenObtainedAt) / 1000;
    if (tokenExpiresIn <= 60 || elapsed >= (tokenExpiresIn - 60)) {
        LOG("[SleepHQ] Access token expired, re-authenticating...");
        return authenticate();
    }
    
    return true;
}

bool SleepHQUploader::discoverTeamId() {
    LOG("[SleepHQ] Discovering team ID...");
    
    String responseBody;
    int httpCode;
    
    if (!httpRequest("GET", "/api/v1/me", "", "", responseBody, httpCode)) {
        LOG_ERROR("[SleepHQ] Failed to request /api/v1/me");
        return false;
    }
    
    if (httpCode != 200) {
        LOG_ERRORF("[SleepHQ] /api/v1/me failed with HTTP %d", httpCode);
        return false;
    }
    
    // Parse response to get current_team_id
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, responseBody);
    if (error) {
        LOG_ERRORF("[SleepHQ] Failed to parse /me response: %s", error.c_str());
        return false;
    }
    
    // Try data.attributes.current_team_id or data.current_team_id
    long teamIdVal = 0;
    if (doc.containsKey("data")) {
        JsonObject data = doc["data"];
        if (data.containsKey("attributes")) {
            teamIdVal = data["attributes"]["current_team_id"] | 0L;
        }
        if (teamIdVal == 0) {
            teamIdVal = data["current_team_id"] | 0L;
        }
    }
    
    if (teamIdVal == 0) {
        LOG_ERROR("[SleepHQ] Could not find current_team_id in /me response");
        LOG_DEBUG("[SleepHQ] Response body:");
        LOG_DEBUG(responseBody.c_str());
        return false;
    }
    
    teamId = String(teamIdVal);
    LOGF("[SleepHQ] Discovered team ID: %s", teamId.c_str());
    return true;
}

bool SleepHQUploader::createImport() {
    if (!ensureAccessToken()) {
        return false;
    }
    
    LOG("[SleepHQ] Creating new import session...");
    
    String path = "/api/v1/teams/" + teamId + "/imports";
    
    // Build request body with device_id
    String body = "";
    int deviceId = config->getCloudDeviceId();
    if (deviceId > 0) {
        body = "device_id=" + String(deviceId);
    }
    
    String responseBody;
    int httpCode;
    
    if (!httpRequest("POST", path, body, 
                     body.isEmpty() ? "" : "application/x-www-form-urlencoded",
                     responseBody, httpCode)) {
        LOG_ERROR("[SleepHQ] Failed to create import");
        return false;
    }
    
    if (httpCode != 201 && httpCode != 200) {
        LOG_ERRORF("[SleepHQ] Create import failed with HTTP %d", httpCode);
        LOG_ERRORF("[SleepHQ] Response: %s", responseBody.c_str());
        return false;
    }
    
    // Parse import_id from response
    DynamicJsonDocument doc(2048);
    DeserializationError error = deserializeJson(doc, responseBody);
    if (error) {
        LOG_ERRORF("[SleepHQ] Failed to parse import response: %s", error.c_str());
        return false;
    }
    
    long importIdVal = 0;
    if (doc.containsKey("data")) {
        JsonObject data = doc["data"];
        if (data.containsKey("attributes")) {
            importIdVal = data["attributes"]["id"] | 0L;
        }
        if (importIdVal == 0) {
            importIdVal = data["id"] | 0L;
        }
    }
    
    if (importIdVal == 0) {
        LOG_ERROR("[SleepHQ] Could not find import ID in response");
        return false;
    }
    
    currentImportId = String(importIdVal);
    LOGF("[SleepHQ] Import created: %s", currentImportId.c_str());
    return true;
}

bool SleepHQUploader::processImport() {
    if (currentImportId.isEmpty()) {
        LOG_WARN("[SleepHQ] No active import to process");
        return true;
    }
    
    if (!ensureAccessToken()) {
        return false;
    }
    
    LOG("[SleepHQ] Processing import...");
    
    String path = "/api/v1/imports/" + currentImportId + "/process_files";
    
    String responseBody;
    int httpCode;
    
    if (!httpRequest("POST", path, "", "", responseBody, httpCode)) {
        LOG_ERROR("[SleepHQ] Failed to process import");
        return false;
    }
    
    if (httpCode != 201 && httpCode != 200) {
        LOG_ERRORF("[SleepHQ] Process import failed with HTTP %d", httpCode);
        return false;
    }
    
    LOGF("[SleepHQ] Import %s submitted for processing", currentImportId.c_str());
    currentImportId = "";
    return true;
}

String SleepHQUploader::computeContentHash(fs::FS &sd, const String& localPath, const String& fileName,
                                             unsigned long& hashedSize) {
    // SleepHQ content_hash = MD5(file_content + filename)
    // Note: filename only (no path)
    // hashedSize returns the exact byte count that was hashed, so the upload
    // can read the same number of bytes ("size-locked" to avoid hash mismatch
    // when the CPAP machine appends data between hash and upload).
    
    File file = sd.open(localPath, FILE_READ);
    if (!file) {
        LOG_ERRORF("[SleepHQ] Cannot open file for hashing: %s", localPath.c_str());
        hashedSize = 0;
        return "";
    }
    
    // Snapshot file size at open time - only hash this many bytes
    unsigned long snapshotSize = file.size();
    
    struct MD5Context md5ctx;
    MD5Init(&md5ctx);
    
    // Hash exactly snapshotSize bytes (not file.available() which can grow)
    uint8_t buffer[CLOUD_UPLOAD_BUFFER_SIZE];
    unsigned long totalHashed = 0;
    while (totalHashed < snapshotSize) {
        size_t toRead = sizeof(buffer);
        if (snapshotSize - totalHashed < toRead) {
            toRead = snapshotSize - totalHashed;
        }
        size_t bytesRead = file.read(buffer, toRead);
        if (bytesRead == 0) break;  // EOF or read error
        MD5Update(&md5ctx, buffer, bytesRead);
        totalHashed += bytesRead;
    }
    file.close();
    hashedSize = totalHashed;
    
    // Append filename to hash
    MD5Update(&md5ctx, (const uint8_t*)fileName.c_str(), fileName.length());
    
    // Finalize
    uint8_t digest[16];
    MD5Final(digest, &md5ctx);
    
    // Convert to hex string
    char hashStr[33];
    for (int i = 0; i < 16; i++) {
        sprintf(hashStr + (i * 2), "%02x", digest[i]);
    }
    hashStr[32] = '\0';
    
    return String(hashStr);
}

bool SleepHQUploader::upload(const String& localPath, const String& remotePath,
                              fs::FS &sd, unsigned long& bytesTransferred) {
    bytesTransferred = 0;
    
    if (!ensureAccessToken()) {
        return false;
    }
    
    if (currentImportId.isEmpty()) {
        LOG_ERROR("[SleepHQ] No active import - call createImport() first");
        return false;
    }
    
    // Extract filename from path
    String fileName = localPath;
    int lastSlash = fileName.lastIndexOf('/');
    if (lastSlash >= 0) {
        fileName = fileName.substring(lastSlash + 1);
    }
    
    // Compute content hash (returns the exact byte count hashed for size-locked upload)
    unsigned long lockedFileSize = 0;
    String contentHash = computeContentHash(sd, localPath, fileName, lockedFileSize);
    if (contentHash.isEmpty()) {
        return false;
    }
    
    LOG_DEBUGF("[SleepHQ] Uploading: %s (%lu bytes, hash: %s)", localPath.c_str(), lockedFileSize, contentHash.c_str());
    
    // Upload via multipart POST using the same byte count that was hashed
    String path = "/api/v1/imports/" + currentImportId + "/files";
    String responseBody;
    int httpCode;
    
    if (!httpMultipartUpload(path, fileName, localPath, contentHash, lockedFileSize, sd, bytesTransferred, responseBody, httpCode)) {
        LOG_ERRORF("[SleepHQ] Upload failed for: %s", localPath.c_str());
        return false;
    }
    
    if (httpCode != 201 && httpCode != 200) {
        LOG_ERRORF("[SleepHQ] Upload returned HTTP %d for: %s", httpCode, localPath.c_str());
        LOG_ERRORF("[SleepHQ] Response: %s", responseBody.c_str());
        return false;
    }
    
    LOG_DEBUGF("[SleepHQ] Uploaded: %s (%lu bytes)", fileName.c_str(), bytesTransferred);
    return true;
}

void SleepHQUploader::end() {
    if (!currentImportId.isEmpty()) {
        processImport();
    }
    connected = false;
    accessToken = "";
    currentImportId = "";
}

bool SleepHQUploader::isConnected() const {
    return connected && !accessToken.isEmpty();
}

const String& SleepHQUploader::getTeamId() const { return teamId; }
const String& SleepHQUploader::getCurrentImportId() const { return currentImportId; }

unsigned long SleepHQUploader::getTokenRemainingSeconds() const {
    if (accessToken.isEmpty() || tokenExpiresIn == 0) return 0;
    unsigned long elapsed = (millis() - tokenObtainedAt) / 1000;
    if (elapsed >= tokenExpiresIn) return 0;
    return tokenExpiresIn - elapsed;
}

// ============================================================================
// HTTP Helpers
// ============================================================================

bool SleepHQUploader::httpRequest(const String& method, const String& path,
                                   const String& body, const String& contentType,
                                   String& responseBody, int& httpCode) {
    if (!tlsClient) {
        setupTLS();
    }
    
    String baseUrl = config->getCloudBaseUrl();
    String url = baseUrl + path;
    
    HTTPClient http;
    http.begin(*tlsClient, url);
    http.setTimeout(15000);  // 15 second timeout
    
    // Set headers
    http.addHeader("Accept", "application/vnd.api+json");
    if (!accessToken.isEmpty()) {
        http.addHeader("Authorization", "Bearer " + accessToken);
    }
    if (!contentType.isEmpty()) {
        http.addHeader("Content-Type", contentType);
    }
    
    // Send request
    if (method == "GET") {
        httpCode = http.GET();
    } else if (method == "POST") {
        if (body.isEmpty()) {
            httpCode = http.POST("");
        } else {
            httpCode = http.POST(body);
        }
    } else {
        LOG_ERRORF("[SleepHQ] Unsupported HTTP method: %s", method.c_str());
        http.end();
        return false;
    }
    
    if (httpCode < 0) {
        LOG_ERRORF("[SleepHQ] HTTP request failed: %s", http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }
    
    responseBody = http.getString();
    http.end();
    return true;
}

bool SleepHQUploader::httpMultipartUpload(const String& path, const String& fileName,
                                           const String& filePath, const String& contentHash,
                                           unsigned long lockedFileSize,
                                           fs::FS &sd, unsigned long& bytesTransferred,
                                           String& responseBody, int& httpCode) {
    if (!tlsClient) {
        setupTLS();
    }
    
    // Open the file
    File file = sd.open(filePath, FILE_READ);
    if (!file) {
        LOG_ERRORF("[SleepHQ] Cannot open file: %s", filePath.c_str());
        return false;
    }
    // Use the locked file size (same byte count that was hashed) instead of
    // file.size() which may have grown since the hash was computed.
    unsigned long fileSize = lockedFileSize;
    
    // Extract the directory path for the 'path' field
    String dirPath = filePath;
    int lastSlash = dirPath.lastIndexOf('/');
    if (lastSlash > 0) {
        dirPath = dirPath.substring(0, lastSlash);
    } else {
        dirPath = "/";
    }
    
    // Build multipart body
    String boundary = "----ESP32Boundary" + String(millis());
    
    // Build the parts before and after the file content
    String partBefore = "";
    
    // name field
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"name\"\r\n\r\n";
    partBefore += fileName + "\r\n";
    
    // path field
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"path\"\r\n\r\n";
    partBefore += dirPath + "\r\n";
    
    // content_hash field
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"content_hash\"\r\n\r\n";
    partBefore += contentHash + "\r\n";
    
    // file field header
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"file\"; filename=\"" + fileName + "\"\r\n";
    partBefore += "Content-Type: application/octet-stream\r\n\r\n";
    
    String partAfter = "\r\n--" + boundary + "--\r\n";
    
    // Calculate total content length
    unsigned long totalLength = partBefore.length() + fileSize + partAfter.length();
    
    // Build full URL
    String baseUrl = config->getCloudBaseUrl();
    String url = baseUrl + path;
    
    // For ESP32 HTTPClient, we need to build the complete payload
    // For files up to ~48KB, assemble in memory; larger files need streaming
    if (fileSize <= 49152) {  // 48KB limit for in-memory assembly
        size_t totalBufSize = partBefore.length() + fileSize + partAfter.length();
        uint8_t* combinedBuf = (uint8_t*)malloc(totalBufSize);
        if (!combinedBuf) {
            LOG_ERROR("[SleepHQ] Failed to allocate combined buffer");
            file.close();
            return false;
        }
        
        // Build payload in single buffer: headers + file content + trailer
        memcpy(combinedBuf, partBefore.c_str(), partBefore.length());
        size_t bytesRead = file.read(combinedBuf + partBefore.length(), fileSize);
        file.close();
        if (bytesRead != fileSize) {
            LOG_ERRORF("[SleepHQ] Short read: expected %lu bytes, got %u", fileSize, bytesRead);
            free(combinedBuf);
            return false;
        }
        memcpy(combinedBuf + partBefore.length() + fileSize, partAfter.c_str(), partAfter.length());
        
        HTTPClient http;
        http.begin(*tlsClient, url);
        http.setTimeout(30000);
        http.addHeader("Accept", "application/vnd.api+json");
        http.addHeader("Authorization", "Bearer " + accessToken);
        http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        
        httpCode = http.sendRequest("POST", combinedBuf, totalBufSize);
        free(combinedBuf);
        
        if (httpCode > 0) {
            responseBody = http.getString();
            bytesTransferred = fileSize;
        }
        http.end();
        return httpCode > 0;
    }
    
    // For larger files, stream via WiFiClientSecure directly
    file.close();
    
    // Parse host and port from base URL
    String host = config->getCloudBaseUrl();
    host.replace("https://", "");
    host.replace("http://", "");
    // Strip trailing path first so ':' in paths can't confuse the parser
    int pathSep = host.indexOf('/');
    if (pathSep > 0) host = host.substring(0, pathSep);
    int port = 443;
    int portSep = host.indexOf(':');
    if (portSep > 0) {
        port = host.substring(portSep + 1).toInt();
        host = host.substring(0, portSep);
    }
    
    if (!tlsClient->connect(host.c_str(), port)) {
        LOG_ERROR("[SleepHQ] Failed to connect for streaming upload");
        return false;
    }
    
    // Send HTTP POST request headers
    tlsClient->print("POST " + path + " HTTP/1.1\r\n");
    tlsClient->print("Host: " + host + "\r\n");
    tlsClient->print("Authorization: Bearer " + accessToken + "\r\n");
    tlsClient->print("Accept: application/vnd.api+json\r\n");
    tlsClient->print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    tlsClient->print("Content-Length: " + String(totalLength) + "\r\n");
    tlsClient->print("Connection: close\r\n\r\n");
    
    // Send multipart preamble
    tlsClient->print(partBefore);
    
    // Re-open and stream file
    file = sd.open(filePath, FILE_READ);
    if (!file) {
        LOG_ERROR("[SleepHQ] Cannot re-open file for streaming");
        tlsClient->stop();
        return false;
    }
    
    uint8_t buffer[CLOUD_UPLOAD_BUFFER_SIZE];
    unsigned long totalSent = 0;
    // Read exactly fileSize bytes (locked to match the content hash)
    // Do NOT use file.available() which can grow if CPAP appends data
    while (totalSent < fileSize) {
        size_t toRead = sizeof(buffer);
        if (fileSize - totalSent < toRead) {
            toRead = fileSize - totalSent;
        }
        size_t bytesRead = file.read(buffer, toRead);
        if (bytesRead == 0) break;  // EOF or read error
        size_t written = tlsClient->write(buffer, bytesRead);
        if (written != bytesRead) {
            LOG_ERROR("[SleepHQ] Write error during streaming upload");
            file.close();
            tlsClient->stop();
            return false;
        }
        totalSent += written;
    }
    file.close();
    
    // Send closing boundary
    tlsClient->print(partAfter);
    tlsClient->flush();
    
    // Read response status line
    unsigned long timeout = millis() + 30000;
    while (!tlsClient->available() && millis() < timeout) {
        delay(10);
    }
    
    String statusLine = tlsClient->readStringUntil('\n');
    // Parse HTTP status code
    int spaceIdx = statusLine.indexOf(' ');
    if (spaceIdx > 0) {
        httpCode = statusLine.substring(spaceIdx + 1, spaceIdx + 4).toInt();
    } else {
        httpCode = -1;
    }
    
    // Skip headers
    while (tlsClient->available()) {
        String headerLine = tlsClient->readStringUntil('\n');
        if (headerLine == "\r" || headerLine.length() <= 1) break;
    }
    
    // Read response body (bulk read to avoid O(n²) char-by-char concatenation)
    responseBody = tlsClient->readString();
    
    tlsClient->stop();
    bytesTransferred = totalSent;
    return httpCode > 0;
}

#endif // ENABLE_SLEEPHQ_UPLOAD
