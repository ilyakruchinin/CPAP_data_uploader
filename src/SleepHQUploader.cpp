#include "SleepHQUploader.h"
#include "SDCardManager.h"
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

// Minimum contiguous heap required for TLS handshake (~40KB buffers + headroom)
#define MIN_HEAP_FOR_TLS 55000

SleepHQUploader::SleepHQUploader(Config* cfg)
    : config(cfg),
      tokenObtainedAt(0),
      tokenExpiresIn(0),
      deviceId(0),
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
    
    // Discover user's machine info (name, model) from /teams/{id}/machines
    discoverMachineInfo();
    
    // Resolve device_id: use config value, or auto-discover from /devices
    deviceId = config->getCloudDeviceId();
    if (deviceId > 0) {
        LOGF("[SleepHQ] Using configured device ID: %d", deviceId);
    } else {
        // Try to auto-discover device_id from API
        if (!discoverDeviceId()) {
            LOG_WARN("[SleepHQ] Could not auto-discover device ID - imports will not be linked to a device");
            LOG_WARN("[SleepHQ] Set CLOUD_DEVICE_ID in config to fix this");
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

bool SleepHQUploader::discoverDeviceId() {
    if (!ensureAccessToken()) {
        return false;
    }
    
    LOG("[SleepHQ] Attempting to auto-discover device ID...");
    
    // The /api/v1/devices/ endpoint returns a static list of supported device TYPES
    // (not per-user devices). Each "device" is a CPAP model family, e.g.:
    //   id=17 "Series 11 (Elite, AutoSet, etc)" ResMed
    //   id=16 "Series 10 (AirSense, AirCurve etc)" ResMed
    //   id=18 "Series 9 (S9 AutoSet, Lumis, etc)" ResMed
    String responseBody;
    int httpCode;
    
    if (!httpRequest("GET", "/api/v1/devices/", "", "", responseBody, httpCode)) {
        LOG_WARN("[SleepHQ] Failed to request /api/v1/devices/");
        return false;
    }
    
    LOGF("[SleepHQ] Devices endpoint HTTP %d", httpCode);
    
    if (httpCode != 200) {
        String truncated = responseBody.substring(0, 200);
        LOGF("[SleepHQ] Devices response: %s", truncated.c_str());
        return false;
    }
    
    // Parse response - JSON:API format: { "data": [ { "id": "17", "type": "device", "attributes": { ... } } ] }
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, responseBody);
    if (error) {
        LOG_ERRORF("[SleepHQ] Failed to parse devices response: %s", error.c_str());
        return false;
    }
    
    if (!doc.containsKey("data") || !doc["data"].is<JsonArray>()) {
        LOG_WARN("[SleepHQ] Unexpected devices response format");
        LOGF("[SleepHQ] Response: %s", responseBody.substring(0, 300).c_str());
        return false;
    }
    
    JsonArray devices = doc["data"].as<JsonArray>();
    int count = devices.size();
    LOGF("[SleepHQ] Found %d supported device type(s)", count);
    
    // Log all device types and try to auto-match
    int matchedId = 0;
    String matchedName = "";
    
    for (int i = 0; i < count; i++) {
        JsonObject dev = devices[i];
        int devId = dev["attributes"]["id"] | dev["id"].as<String>().toInt();
        String devName = dev["attributes"]["name"] | "";
        String devBrand = dev["attributes"]["brand"] | "";
        
        LOGF("[SleepHQ] Device type: id=%d brand=%s name=%s", 
             devId, devBrand.c_str(), devName.c_str());
        
        // Auto-match: look for ResMed Series 11 (most common target for this project)
        // The name contains "Series 11" for AirSense 11 / AirCurve 11 devices
        if (devBrand == "ResMed" && devName.indexOf("Series 11") >= 0) {
            matchedId = devId;
            matchedName = devName;
        }
    }
    
    if (matchedId > 0) {
        deviceId = matchedId;
        LOGF("[SleepHQ] Auto-matched device: id=%d (%s)", deviceId, matchedName.c_str());
        return true;
    }
    
    LOG_WARN("[SleepHQ] Could not auto-match device type - set CLOUD_DEVICE_ID in config");
    LOG_WARN("[SleepHQ] Common values: 17=ResMed Series 11, 16=Series 10, 18=Series 9");
    return false;
}

bool SleepHQUploader::discoverMachineInfo() {
    if (!ensureAccessToken()) {
        return false;
    }
    
    LOG("[SleepHQ] Discovering machine info...");
    
    // /v1/teams/{team_id}/machines returns the user's actual CPAP machines
    // (distinct from /v1/devices which returns device TYPE categories)
    String path = "/api/v1/teams/" + teamId + "/machines";
    String responseBody;
    int httpCode;
    
    if (!httpRequest("GET", path, "", "", responseBody, httpCode)) {
        LOG_WARN("[SleepHQ] Failed to request machines endpoint");
        return false;
    }
    
    if (httpCode != 200) {
        LOG_WARNF("[SleepHQ] Machines endpoint HTTP %d", httpCode);
        return false;
    }
    
    // Parse response - JSON:API format
    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, responseBody);
    if (error) {
        LOG_WARNF("[SleepHQ] Failed to parse machines response: %s", error.c_str());
        return false;
    }
    
    if (!doc.containsKey("data")) {
        LOG_WARN("[SleepHQ] No data in machines response");
        return false;
    }
    
    // Could be an array or single object
    JsonVariant data = doc["data"];
    
    if (data.is<JsonArray>()) {
        JsonArray machines = data.as<JsonArray>();
        LOGF("[SleepHQ] Found %d machine(s) on account", machines.size());
        
        for (size_t i = 0; i < machines.size(); i++) {
            JsonObject m = machines[i];
            int mId = m["attributes"]["id"] | m["id"].as<String>().toInt();
            String mName = m["attributes"]["name"] | "";
            String mModel = m["attributes"]["model"] | "";
            String mBrand = m["attributes"]["brand"] | "";
            String mSerial = m["attributes"]["serial_number"] | "";
            
            LOGF("[SleepHQ] Machine %d: id=%d name=%s model=%s brand=%s serial=%s",
                 (int)(i + 1), mId, mName.c_str(), mModel.c_str(), mBrand.c_str(), mSerial.c_str());
            
            // Store the first machine's name for import labeling
            if (i == 0 && machineName.isEmpty()) {
                machineName = mName.isEmpty() ? mModel : mName;
            }
        }
    } else if (data.is<JsonObject>()) {
        JsonObject m = data.as<JsonObject>();
        String mName = m["attributes"]["name"] | "";
        String mModel = m["attributes"]["model"] | "";
        LOGF("[SleepHQ] Machine: name=%s model=%s", mName.c_str(), mModel.c_str());
        machineName = mName.isEmpty() ? mModel : mName;
    }
    
    if (!machineName.isEmpty()) {
        LOGF("[SleepHQ] Using machine name: %s", machineName.c_str());
    }
    return true;
}

bool SleepHQUploader::createImport() {
    if (!ensureAccessToken()) {
        return false;
    }
    
    LOG("[SleepHQ] Creating new import session...");
    
    String path = "/api/v1/teams/" + teamId + "/imports";
    
    // Build request body with device_id and name
    String body = "";
    if (deviceId > 0) {
        body = "device_id=" + String(deviceId);
        LOGF("[SleepHQ] Import linked to device ID: %d", deviceId);
    } else {
        LOG_WARN("[SleepHQ] No device_id set - import will not be linked to a specific machine");
    }
    
    // Set import name for SleepHQ UI
    String importName = "CPAP Auto-Upload";
    if (!machineName.isEmpty()) {
        importName = machineName + " Auto-Upload";
    }
    if (!body.isEmpty()) body += "&";
    body += "name=" + importName;
    
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

bool SleepHQUploader::upload(const String& localPath, const String& remotePath,
                              fs::FS &sd, unsigned long& bytesTransferred,
                              SDCardManager* sdManager) {
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
    
    // Get file size (metadata only — no content read yet).
    // Lock the size now so hash and upload use the same byte count,
    // even if the CPAP appends data between now and upload completion.
    File file = sd.open(localPath, FILE_READ);
    if (!file) {
        LOG_ERRORF("[SleepHQ] Cannot open file: %s", localPath.c_str());
        return false;
    }
    unsigned long fileSize = file.size();
    file.close();
    
    if (fileSize == 0) {
        LOG_WARNF("[SleepHQ] Skipping empty file: %s", localPath.c_str());
        return true;
    }
    
    LOG_DEBUGF("[SleepHQ] Uploading: %s (%lu bytes)", localPath.c_str(), fileSize);
    
    String apiPath = "/api/v1/imports/" + currentImportId + "/files";
    
    // Clean TLS state before each upload to prevent stale connection issues
    if (tlsClient) {
        tlsClient->stop();
        delay(10);
    }
    
    String responseBody;
    int httpCode;
    String contentHash;
    
    // Single-read upload: file is read once, hash computed progressively
    if (!httpMultipartUpload(apiPath, fileName, localPath, fileSize, sd, bytesTransferred, responseBody, httpCode, sdManager, &contentHash)) {
        LOG_ERRORF("[SleepHQ] Upload failed for: %s", localPath.c_str());
        return false;
    }
    
    if (httpCode != 201 && httpCode != 200) {
        LOG_ERRORF("[SleepHQ] Upload returned HTTP %d for: %s", httpCode, localPath.c_str());
        LOG_ERRORF("[SleepHQ] Response: %s", responseBody.substring(0, 200).c_str());
        return false;
    }
    
    if (httpCode == 200) {
        LOGF("[SleepHQ] Skipped (already on server): %s", fileName.c_str());
    } else {
        LOGF("[SleepHQ] Uploaded: %s (%lu bytes, hash: %s)", fileName.c_str(), bytesTransferred, contentHash.c_str());
    }
    return true;
}

bool SleepHQUploader::uploadFromBuffer(const uint8_t* fileData, size_t fileSize,
                                        const String& fileName, const String& filePath,
                                        unsigned long& bytesTransferred) {
    bytesTransferred = 0;
    
    if (!ensureAccessToken()) {
        return false;
    }
    
    if (currentImportId.isEmpty()) {
        LOG_ERROR("[SleepHQ] No active import - call createImport() first");
        return false;
    }
    
    if (fileSize == 0) {
        LOG_WARNF("[SleepHQ] Skipping empty file: %s", fileName.c_str());
        return true;
    }
    
    LOG_DEBUGF("[SleepHQ] Uploading from buffer: %s (%u bytes)", fileName.c_str(), fileSize);
    
    String apiPath = "/api/v1/imports/" + currentImportId + "/files";
    
    // Reuse existing TLS connection (HTTP keep-alive) to avoid heap
    // fragmentation from repeated connect/disconnect cycles.  Each TLS
    // handshake allocates ~32KB; with a 48KB batch buffer on the heap,
    // repeated alloc/free fragments memory until no contiguous block
    // remains for the next handshake.
    if (!tlsClient) {
        setupTLS();
    }
    
    // Parse host and port from base URL
    String host = config->getCloudBaseUrl();
    host.replace("https://", "");
    host.replace("http://", "");
    int pathSep = host.indexOf('/');
    if (pathSep > 0) host = host.substring(0, pathSep);
    int port = 443;
    int portSep = host.indexOf(':');
    if (portSep > 0) {
        port = host.substring(portSep + 1).toInt();
        host = host.substring(0, portSep);
    }
    
    // Only connect if not already connected (reuse for batch)
    if (!tlsClient->connected()) {
        if (!tlsClient->connect(host.c_str(), port)) {
            LOG_ERROR("[SleepHQ] Failed to connect for buffer upload");
            return false;
        }
        LOG_DEBUG("[SleepHQ] New TLS connection for buffer upload");
    }
    
    // Extract directory path for 'path' field
    String dirPath = filePath;
    int lastSlash = dirPath.lastIndexOf('/');
    if (lastSlash > 0) {
        dirPath = "." + dirPath.substring(0, lastSlash) + "/";
    } else {
        dirPath = "./";
    }
    
    // Compute hash from buffered data: MD5(file_content + filename)
    struct MD5Context md5ctx;
    MD5Init(&md5ctx);
    MD5Update(&md5ctx, fileData, fileSize);
    MD5Update(&md5ctx, (const uint8_t*)fileName.c_str(), fileName.length());
    uint8_t digest[16];
    MD5Final(digest, &md5ctx);
    char hashStr[33];
    for (int i = 0; i < 16; i++) sprintf(hashStr + (i * 2), "%02x", digest[i]);
    hashStr[32] = '\0';
    
    // Build multipart parts as strings (small — no file data copy)
    String boundary = "----ESP32Boundary" + String(millis());
    
    String partBefore = "";
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"name\"\r\n\r\n";
    partBefore += fileName + "\r\n";
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"path\"\r\n\r\n";
    partBefore += dirPath + "\r\n";
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"file\"; filename=\"" + fileName + "\"\r\n";
    partBefore += "Content-Type: application/octet-stream\r\n\r\n";
    
    String partHashHdr = "\r\n--" + boundary + "\r\n";
    partHashHdr += "Content-Disposition: form-data; name=\"content_hash\"\r\n\r\n";
    
    String partEnd = "\r\n--" + boundary + "--\r\n";
    
    size_t totalLength = partBefore.length() + fileSize + partHashHdr.length() + 32 + partEnd.length();
    
    // HTTP request headers — keep-alive to reuse connection for batch
    tlsClient->print("POST " + apiPath + " HTTP/1.1\r\n");
    tlsClient->print("Host: " + host + "\r\n");
    tlsClient->print("Authorization: Bearer " + accessToken + "\r\n");
    tlsClient->print("Accept: application/vnd.api+json\r\n");
    tlsClient->print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    tlsClient->print("Content-Length: " + String(totalLength) + "\r\n");
    tlsClient->print("Connection: keep-alive\r\n\r\n");
    
    // Multipart body — write parts directly (no intermediate buffer)
    tlsClient->print(partBefore);
    
    // Write file data directly from caller's buffer (zero-copy)
    size_t totalSent = 0;
    while (totalSent < fileSize) {
        size_t chunk = fileSize - totalSent;
        if (chunk > CLOUD_UPLOAD_BUFFER_SIZE) chunk = CLOUD_UPLOAD_BUFFER_SIZE;
        size_t written = tlsClient->write(fileData + totalSent, chunk);
        if (written == 0) {
            LOG_ERROR("[SleepHQ] Write error during buffer upload");
            tlsClient->stop();
            return false;
        }
        totalSent += written;
        yield();
    }
    
    // Hash + closing boundary
    tlsClient->print(partHashHdr);
    tlsClient->write((const uint8_t*)hashStr, 32);
    tlsClient->print(partEnd);
    tlsClient->flush();
    
    // Read response (keep-alive aware — must consume full response body
    // using Content-Length so the connection is ready for the next request)
    unsigned long timeout = millis() + 30000;
    while (!tlsClient->available() && millis() < timeout) {
        delay(10);
    }
    
    // Parse status line
    String statusLine = tlsClient->readStringUntil('\n');
    int spaceIdx = statusLine.indexOf(' ');
    int httpCode = -1;
    if (spaceIdx > 0) {
        httpCode = statusLine.substring(spaceIdx + 1, spaceIdx + 4).toInt();
    }
    
    // Parse response headers — extract Content-Length for keep-alive body read
    int contentLength = -1;
    bool serverClose = false;
    while (millis() < timeout) {
        if (!tlsClient->available()) { delay(1); continue; }
        String headerLine = tlsClient->readStringUntil('\n');
        headerLine.trim();
        if (headerLine.length() == 0) break;
        if (headerLine.startsWith("Content-Length:") || headerLine.startsWith("content-length:")) {
            contentLength = headerLine.substring(headerLine.indexOf(':') + 1).toInt();
        }
        if (headerLine.indexOf("connection: close") >= 0 || headerLine.indexOf("Connection: close") >= 0) {
            serverClose = true;
        }
    }
    
    // Read response body using Content-Length (required for keep-alive)
    String responseBody;
    if (contentLength > 0) {
        int remaining = contentLength;
        while (remaining > 0 && millis() < timeout) {
            if (tlsClient->available()) {
                int toRead = remaining;
                if (toRead > 512) toRead = 512;
                uint8_t rbuf[512];
                int got = tlsClient->read(rbuf, toRead);
                if (got > 0) {
                    if (responseBody.length() < 200) {
                        // Only keep first 200 chars for error reporting
                        responseBody += String((char*)rbuf).substring(0, got);
                    }
                    remaining -= got;
                }
            } else {
                delay(1);
            }
        }
    } else if (contentLength < 0) {
        // No Content-Length — drain what's available
        delay(100);
        while (tlsClient->available()) {
            char c = tlsClient->read();
            if (responseBody.length() < 200) responseBody += c;
        }
    }
    
    // If server sent Connection: close, stop the client
    if (serverClose) {
        tlsClient->stop();
    }
    // Otherwise: connection stays open for next uploadFromBuffer() call
    
    bytesTransferred = totalSent;
    
    if (httpCode != 201 && httpCode != 200) {
        LOG_ERRORF("[SleepHQ] Buffer upload HTTP %d for: %s", httpCode, fileName.c_str());
        if (httpCode > 0) {
            LOG_ERRORF("[SleepHQ] Response: %s", responseBody.substring(0, 200).c_str());
        }
        tlsClient->stop();  // Close on error
        return false;
    }
    
    if (httpCode == 200) {
        LOGF("[SleepHQ] Skipped (already on server): %s", fileName.c_str());
    } else {
        LOGF("[SleepHQ] Uploaded: %s (%u bytes, hash: %s)", fileName.c_str(), fileSize, hashStr);
    }
    return true;
}

void SleepHQUploader::disconnectTls() {
    if (tlsClient) {
        if (tlsClient->connected()) {
            tlsClient->stop();
        }
        delete tlsClient;
        tlsClient = nullptr;
        LOG_DEBUG("[SleepHQ] TLS connection closed (freeing ~32KB heap)");
    }
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
    
    // Check heap before TLS operation - force cleanup if fragmented
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    if (maxBlock < MIN_HEAP_FOR_TLS) {
        LOG_WARNF("[SleepHQ] Low contiguous heap (%u bytes) - forcing TLS cleanup", maxBlock);
        tlsClient->stop();
        delay(10);
        maxBlock = ESP.getMaxAllocHeap();
        if (maxBlock < MIN_HEAP_FOR_TLS) {
            LOG_ERRORF("[SleepHQ] Heap still too low after cleanup (%u bytes), cannot establish TLS", maxBlock);
            return false;
        }
    }
    
    String baseUrl = config->getCloudBaseUrl();
    String url = baseUrl + path;
    
    HTTPClient http;
    http.setReuse(false);  // Force connection close to free TLS buffers after each request
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
    
    LOG_DEBUGF("[SleepHQ] Heap after request: %u free, %u max block", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return true;
}

bool SleepHQUploader::httpMultipartUpload(const String& path, const String& fileName,
                                           const String& filePath, unsigned long fileSize,
                                           fs::FS &sd, unsigned long& bytesTransferred,
                                           String& responseBody, int& httpCode,
                                           SDCardManager* sdManager,
                                           String* outContentHash) {
    if (!tlsClient) {
        setupTLS();
    }
    
    // Extract the directory path for the 'path' field
    // SleepHQ expects "./" prefix and trailing "/" per API docs:
    //   "./" for root files, "./DATALOG/20230924/" for DATALOG files
    String dirPath = filePath;
    int lastSlash = dirPath.lastIndexOf('/');
    if (lastSlash > 0) {
        dirPath = "." + dirPath.substring(0, lastSlash) + "/";
    } else {
        dirPath = "./";
    }
    
    // Build multipart body with content_hash AFTER file data.
    // This enables single-read upload: hash is computed progressively
    // during the file read and sent after the file content.
    String boundary = "----ESP32Boundary" + String(millis());
    
    // Part before file content: name + path + file header
    String partBefore = "";
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"name\"\r\n\r\n";
    partBefore += fileName + "\r\n";
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"path\"\r\n\r\n";
    partBefore += dirPath + "\r\n";
    partBefore += "--" + boundary + "\r\n";
    partBefore += "Content-Disposition: form-data; name=\"file\"; filename=\"" + fileName + "\"\r\n";
    partBefore += "Content-Type: application/octet-stream\r\n\r\n";
    
    // Part between file content and hash: boundary + hash field header
    String partHashHdr = "\r\n--" + boundary + "\r\n";
    partHashHdr += "Content-Disposition: form-data; name=\"content_hash\"\r\n\r\n";
    // <32-char hash will be inserted here>
    
    String partEnd = "\r\n--" + boundary + "--\r\n";
    
    // Content-Length is calculable: hash is always exactly 32 hex chars
    unsigned long totalLength = partBefore.length() + fileSize
                              + partHashHdr.length() + 32 + partEnd.length();
    
    // Check heap before TLS operation - force cleanup if fragmented
    uint32_t maxBlock = ESP.getMaxAllocHeap();
    LOG_DEBUGF("[SleepHQ] Heap before upload: %u free, %u max block", ESP.getFreeHeap(), maxBlock);
    if (maxBlock < MIN_HEAP_FOR_TLS) {
        LOG_WARNF("[SleepHQ] Low contiguous heap (%u bytes) - forcing TLS cleanup", maxBlock);
        tlsClient->stop();
        delay(10);
        maxBlock = ESP.getMaxAllocHeap();
        if (maxBlock < MIN_HEAP_FOR_TLS) {
            LOG_ERRORF("[SleepHQ] Heap too low for TLS (%u bytes), skipping upload", maxBlock);
            return false;
        }
    }
    
    // ─── In-memory path (≤48KB files) ───────────────────────────────────
    // Read file once into buffer, compute hash from buffer data, assemble
    // complete payload, release SD, send via HTTPClient.
    if (fileSize <= 49152) {
        size_t totalBufSize = totalLength;
        
        if (ESP.getMaxAllocHeap() < totalBufSize + MIN_HEAP_FOR_TLS) {
            LOG_WARNF("[SleepHQ] Insufficient heap for in-memory (%u needed) - using streaming",
                      totalBufSize + MIN_HEAP_FOR_TLS);
            goto streaming_upload;
        }
        
        uint8_t* combinedBuf = (uint8_t*)malloc(totalBufSize);
        if (!combinedBuf) {
            LOG_WARN("[SleepHQ] Buffer alloc failed - falling back to streaming");
            goto streaming_upload;
        }
        
        // Copy preamble
        size_t offset = 0;
        memcpy(combinedBuf + offset, partBefore.c_str(), partBefore.length());
        offset += partBefore.length();
        
        // Read file directly into payload buffer (single SD read)
        File file = sd.open(filePath, FILE_READ);
        if (!file) {
            LOG_ERRORF("[SleepHQ] Cannot open file: %s", filePath.c_str());
            free(combinedBuf);
            return false;
        }
        size_t bytesRead = file.read(combinedBuf + offset, fileSize);
        file.close();
        
        if (bytesRead != fileSize) {
            LOG_ERRORF("[SleepHQ] Short read: expected %lu, got %u", fileSize, bytesRead);
            free(combinedBuf);
            return false;
        }
        
        // Compute hash from the buffered file data (no extra SD read!)
        // SleepHQ content_hash = MD5(file_content + filename)
        struct MD5Context md5ctx;
        MD5Init(&md5ctx);
        MD5Update(&md5ctx, combinedBuf + offset, bytesRead);
        MD5Update(&md5ctx, (const uint8_t*)fileName.c_str(), fileName.length());
        uint8_t digest[16];
        MD5Final(digest, &md5ctx);
        char hashStr[33];
        for (int i = 0; i < 16; i++) sprintf(hashStr + (i * 2), "%02x", digest[i]);
        hashStr[32] = '\0';
        if (outContentHash) *outContentHash = String(hashStr);
        
        offset += fileSize;
        
        // Append hash field + closing boundary
        memcpy(combinedBuf + offset, partHashHdr.c_str(), partHashHdr.length());
        offset += partHashHdr.length();
        memcpy(combinedBuf + offset, hashStr, 32);
        offset += 32;
        memcpy(combinedBuf + offset, partEnd.c_str(), partEnd.length());
        
        // Release SD — all file data is in RAM.
        // CPAP gets SD for the entire HTTP POST + response cycle.
        if (sdManager && sdManager->hasControl()) {
            sdManager->releaseControl();
            LOG_DEBUG("[SleepHQ] SD released for CPAP during in-memory upload");
        }
        
        String url = config->getCloudBaseUrl() + path;
        HTTPClient http;
        http.setReuse(false);
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
        LOG_DEBUGF("[SleepHQ] Heap after upload: %u free, %u max block", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        return httpCode > 0;
    }
    
    // ─── Streaming path (>48KB files) ───────────────────────────────────
    // SD_MMC.end() in releaseControl() unmounts the filesystem, which
    // invalidates all open file handles.  Per-chunk SD release is therefore
    // impossible.  Instead:
    //   1. Release SD during TLS handshake (network-only, saves ~2-3s)
    //   2. Retake SD, open file, stream all data + progressive hash
    //   3. Close file, release SD
    //   4. Send content_hash field, wait for response (SD released)
    
streaming_upload:
    {
    // Parse host and port from base URL
    String host = config->getCloudBaseUrl();
    host.replace("https://", "");
    host.replace("http://", "");
    int pathSep = host.indexOf('/');
    if (pathSep > 0) host = host.substring(0, pathSep);
    int port = 443;
    int portSep = host.indexOf(':');
    if (portSep > 0) {
        port = host.substring(portSep + 1).toInt();
        host = host.substring(0, portSep);
    }
    
    // Release SD during TLS handshake — CPAP gets access while we negotiate
    if (sdManager && sdManager->hasControl()) {
        sdManager->releaseControl();
        LOG_DEBUG("[SleepHQ] SD released for CPAP during TLS handshake");
    }
    
    if (!tlsClient->connect(host.c_str(), port)) {
        LOG_ERROR("[SleepHQ] Failed to connect for streaming upload");
        return false;
    }
    
    // Send HTTP POST request headers (still no SD needed)
    tlsClient->print("POST " + path + " HTTP/1.1\r\n");
    tlsClient->print("Host: " + host + "\r\n");
    tlsClient->print("Authorization: Bearer " + accessToken + "\r\n");
    tlsClient->print("Accept: application/vnd.api+json\r\n");
    tlsClient->print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
    tlsClient->print("Content-Length: " + String(totalLength) + "\r\n");
    tlsClient->print("Connection: close\r\n\r\n");
    
    // Send preamble (name + path + file header)
    tlsClient->print(partBefore);
    
    // Retake SD for file read
    if (sdManager && !sdManager->hasControl()) {
        if (!sdManager->takeControl()) {
            LOG_ERROR("[SleepHQ] Failed to retake SD for streaming file read");
            tlsClient->stop();
            return false;
        }
    }
    
    // Open file for the single read
    File file = sd.open(filePath, FILE_READ);
    if (!file) {
        LOG_ERROR("[SleepHQ] Cannot open file for streaming");
        tlsClient->stop();
        return false;
    }
    
    // Progressive hash: MD5(file_content + filename)
    struct MD5Context md5ctx;
    MD5Init(&md5ctx);
    
    uint8_t buffer[CLOUD_UPLOAD_BUFFER_SIZE];
    unsigned long totalSent = 0;
    
    // Stream all file data — SD must stay mounted (file handle requires it)
    while (totalSent < fileSize) {
        size_t toRead = sizeof(buffer);
        if (fileSize - totalSent < toRead) {
            toRead = fileSize - totalSent;
        }
        
        size_t bytesRead = file.read(buffer, toRead);
        if (bytesRead == 0) break;
        
        MD5Update(&md5ctx, buffer, bytesRead);
        
        size_t written = tlsClient->write(buffer, bytesRead);
        if (written != bytesRead) {
            LOG_ERROR("[SleepHQ] Write error during streaming upload");
            file.close();
            tlsClient->stop();
            return false;
        }
        totalSent += written;
        yield();  // Prevent watchdog timeout on large files
    }
    file.close();
    
    // Finalize hash: append filename then digest
    MD5Update(&md5ctx, (const uint8_t*)fileName.c_str(), fileName.length());
    uint8_t digest[16];
    MD5Final(digest, &md5ctx);
    char hashStr[33];
    for (int i = 0; i < 16; i++) sprintf(hashStr + (i * 2), "%02x", digest[i]);
    hashStr[32] = '\0';
    if (outContentHash) *outContentHash = String(hashStr);
    
    // Release SD — file closed, only network I/O remains.
    // CPAP gets SD during hash send + response wait.
    if (sdManager && sdManager->hasControl()) {
        sdManager->releaseControl();
        LOG_DEBUG("[SleepHQ] SD released for CPAP during response wait");
    }
    
    // Send: file part trailer → content_hash field → closing boundary
    tlsClient->print(partHashHdr);
    tlsClient->print(hashStr);
    tlsClient->print(partEnd);
    tlsClient->flush();
    
    // Read response
    unsigned long timeout = millis() + 30000;
    while (!tlsClient->available() && millis() < timeout) {
        delay(10);
    }
    
    String statusLine = tlsClient->readStringUntil('\n');
    int spaceIdx = statusLine.indexOf(' ');
    if (spaceIdx > 0) {
        httpCode = statusLine.substring(spaceIdx + 1, spaceIdx + 4).toInt();
    } else {
        httpCode = -1;
    }
    
    // Skip response headers
    while (tlsClient->available()) {
        String headerLine = tlsClient->readStringUntil('\n');
        if (headerLine == "\r" || headerLine.length() <= 1) break;
    }
    
    // Read response body
    responseBody = tlsClient->readString();
    
    tlsClient->stop();
    bytesTransferred = totalSent;
    return httpCode > 0;
    }
}

#endif // ENABLE_SLEEPHQ_UPLOAD
