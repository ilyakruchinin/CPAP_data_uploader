#include "SleepHQUploader.h"
#include "Logger.h"

#ifdef ENABLE_SLEEPHQ_UPLOAD

#include <WiFi.h>
#include <ArduinoJson.h>
#include "NetworkRecovery.h"
#include <esp_rom_md5.h>
#include <esp_task_wdt.h>

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

// Upload buffer size for streaming files — adaptive based on heap pressure.
// Smaller buffers reduce peak current from concurrent SD read + TLS encrypt + WiFi TX,
// at the cost of more SD read calls per file. Network throughput is the bottleneck,
// not SD read speed, so smaller buffers have negligible transfer time impact.
#define CLOUD_UPLOAD_BUFFER_SIZE_MAX  4096
#define CLOUD_UPLOAD_BUFFER_SIZE_MID  2048
#define CLOUD_UPLOAD_BUFFER_SIZE_MIN  1024

static size_t getAdaptiveBufferSize() {
    uint32_t ma = ESP.getMaxAllocHeap();
    if (ma > 50000) return CLOUD_UPLOAD_BUFFER_SIZE_MAX;
    if (ma > 35000) return CLOUD_UPLOAD_BUFFER_SIZE_MID;
    return CLOUD_UPLOAD_BUFFER_SIZE_MIN;
}

SleepHQUploader::SleepHQUploader(Config* cfg)
    : config(cfg),
      tokenObtainedAt(0),
      tokenExpiresIn(0),
      connected(false),
      lowMemoryKeepAliveWarned(false),
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
        if (!tlsClient) {
            LOG_ERROR("[SleepHQ] Failed to allocate WiFiClientSecure - OOM!");
            return;
        }
    }
    configureTLS();
}

void SleepHQUploader::resetTLS() {
    if (tlsClient) {
        tlsClient->stop();
        delay(100);  // Let lwIP process the FIN/RST before freeing
        delete tlsClient;
        tlsClient = nullptr;
    }
    // Give lwIP TCP stack time to release socket FDs and clean up TIME_WAIT
    delay(500);
    setupTLS();
}

void SleepHQUploader::configureTLS() {
    if (!tlsClient) return;
    
    if (config->getCloudInsecureTls()) {
        LOG_WARN("[SleepHQ] TLS certificate validation DISABLED (insecure mode)");
        tlsClient->setInsecure();
    } else {
        LOG_DEBUG("[SleepHQ] Using GTS Root R4 CA certificate for TLS validation");
        tlsClient->setCACert(GTS_ROOT_R4_CA);
    }
    
    // Set reasonable timeout for ESP32 - increased to 60s for slow networks
    tlsClient->setTimeout(60);  // 60 seconds
}

void SleepHQUploader::resetConnection() {
    resetTLS();
}

void SleepHQUploader::parseHostPort(char* host, size_t hostLen, int& port) {
    port = 443;
    const char* rawUrl = config->getCloudBaseUrl().c_str();
    const char* hostStart = rawUrl;
    if (strncmp(hostStart, "https://", 8) == 0) hostStart += 8;
    else if (strncmp(hostStart, "http://", 7) == 0) { hostStart += 7; port = 80; }
    const char* hostEnd = strchr(hostStart, '/');
    size_t len = hostEnd ? (size_t)(hostEnd - hostStart) : strlen(hostStart);
    if (len >= hostLen) len = hostLen - 1;
    memcpy(host, hostStart, len);
    host[len] = '\0';
    char* colonPos = strchr(host, ':');
    if (colonPos) {
        port = atoi(colonPos + 1);
        *colonPos = '\0';
    }
}

bool SleepHQUploader::preWarmTLS() {
    if (tlsClient && tlsClient->connected()) {
        LOG_DEBUG("[SleepHQ] TLS already connected — pre-warm skipped");
        return true;
    }

    LOG("[SleepHQ] Pre-warming TLS connection (before SD mount)...");
    setupTLS();
    if (!tlsClient) {
        LOG_WARN("[SleepHQ] Pre-warm: failed to allocate WiFiClientSecure");
        return false;
    }

    char host[128];
    int port = 443;
    parseHostPort(host, sizeof(host), port);

    uint32_t fh = ESP.getFreeHeap();
    uint32_t ma = ESP.getMaxAllocHeap();
    LOGF("[SleepHQ] Pre-warm: connecting to %s:%d (fh=%u, ma=%u)", host, port, fh, ma);

    if (!tlsClient->connect(host, port)) {
        LOG_WARN("[SleepHQ] Pre-warm: TLS connect failed (non-fatal, will retry during upload)");
        return false;
    }

    LOGF("[SleepHQ] Pre-warm: TLS connected (fh=%u, ma=%u)",
         ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return true;
}

bool SleepHQUploader::begin() {
    LOG("[SleepHQ] Initializing cloud uploader...");
    
    // Setup TLS
    setupTLS();
    if (!tlsClient) {
        LOG_ERROR("[SleepHQ] Failed to initialize TLS client");
        return false;
    }
    
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
    
    // Create the import session while the TLS connection is still alive from
    // the OAuth and team-discovery requests above.  This avoids a second
    // SSL handshake (which would fail at low max_alloc) when ensureCloudImport()
    // calls createImport() later.
    if (!createImport()) {
        LOG_ERROR("[SleepHQ] Failed to create initial import session");
        return false;
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
    char bodyBuf[256];
    snprintf(bodyBuf, sizeof(bodyBuf), "grant_type=password&client_id=%s&client_secret=%s&scope=read+write", 
             config->getCloudClientId().c_str(), config->getCloudClientSecret().c_str());
    String body(bodyBuf);
    
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
    StaticJsonDocument<512> doc;
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
    StaticJsonDocument<2048> doc;
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
    int deviceId = config->getCloudDeviceId();
    
    String body = "";
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
    StaticJsonDocument<1024> doc;
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
    extern volatile unsigned long g_uploadHeartbeat;
    g_uploadHeartbeat = millis();
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
        currentImportId = "";
        return false;
    }
    
    LOGF("[SleepHQ] Import %s submitted for processing", currentImportId.c_str());
    currentImportId = "";
    extern volatile unsigned long g_uploadHeartbeat;
    g_uploadHeartbeat = millis();
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
    
    md5_context_t md5ctx;
    esp_rom_md5_init(&md5ctx);
    
    // Hash exactly snapshotSize bytes (not file.available() which can grow)
    uint8_t buffer[CLOUD_UPLOAD_BUFFER_SIZE_MAX];
    unsigned long totalHashed = 0;
    while (totalHashed < snapshotSize) {
        size_t toRead = sizeof(buffer);
        if (snapshotSize - totalHashed < toRead) {
            toRead = snapshotSize - totalHashed;
        }
        size_t bytesRead = file.read(buffer, toRead);
        if (bytesRead == 0) break;  // EOF or read error
        esp_rom_md5_update(&md5ctx, buffer, bytesRead);
        totalHashed += bytesRead;
    }
    file.close();
    hashedSize = totalHashed;
    
    // Append filename to hash
    esp_rom_md5_update(&md5ctx, (const uint8_t*)fileName.c_str(), fileName.length());
    
    // Finalize
    uint8_t digest[16];
    esp_rom_md5_final(digest, &md5ctx);
    
    // Convert to hex string
    char hashStr[33];
    for (int i = 0; i < 16; i++) {
        sprintf(hashStr + (i * 2), "%02x", digest[i]);
    }
    hashStr[32] = '\0';
    
    return String(hashStr);
}

bool SleepHQUploader::upload(const String& localPath, const String& remotePath,
                              fs::FS &sd, unsigned long& bytesTransferred, String& fileChecksum) {
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
    
    // ── Single-pass upload: content_hash computed on-the-fly ──
    // The SleepHQ API accepts content_hash in the multipart footer (after file
    // data), so we no longer need a separate pre-computation pass.
    // This eliminates one full file read per upload (~50% SD active time reduction).
    // File size is snapshotted at open time to lock the byte count.
    File sizeCheckFile = sd.open(localPath, FILE_READ);
    if (!sizeCheckFile) {
        LOG_ERRORF("[SleepHQ] Cannot open file: %s", localPath.c_str());
        return false;
    }
    unsigned long lockedFileSize = sizeCheckFile.size();
    sizeCheckFile.close();
    
    if (lockedFileSize == 0) {
        LOG_WARNF("[SleepHQ] Skipping empty file: %s", localPath.c_str());
        return true;
    }
    
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    
    LOG_DEBUGF("[SleepHQ] Uploading: %s (%lu bytes, free: %u, max_alloc: %u)",
               localPath.c_str(), lockedFileSize, freeHeap, maxAlloc);
    
    // Prefer one persistent TLS session across the entire import.
    // Reconnecting per large file increases TLS handshake churn and heap fragmentation risk.
    if (g_debugMode) {
        bool lowMemory = (maxAlloc < 50000);
        if (!lowMemory) {
            lowMemoryKeepAliveWarned = false;
        } else if (!lowMemoryKeepAliveWarned) {
            LOGF("[SleepHQ] Low memory (%u bytes contiguous) — TLS keep-alive active", maxAlloc);
            lowMemoryKeepAliveWarned = true;
        }
    }

    const bool useKeepAlive = true;
    
    // Note: WiFi power saving (Modem-sleep) does NOT need to be disabled during uploads.
    // The ESP-IDF WiFi driver automatically holds a PM lock during active TX/RX,
    // ensuring full performance when transmitting. Modem-sleep only engages between
    // packet bursts when the radio would be idle anyway.
    
    // Upload via multipart POST — content_hash computed on-the-fly and sent in footer
    String path = "/api/v1/imports/" + currentImportId + "/files";
    String responseBody;
    int httpCode;
    
    String calculatedFileChecksum;
    // contentHash is empty — httpMultipartUpload computes it on-the-fly
    if (!httpMultipartUpload(path, fileName, localPath, "", lockedFileSize, sd, bytesTransferred, responseBody, httpCode, &calculatedFileChecksum, useKeepAlive)) {
        LOG_ERRORF("[SleepHQ] Upload failed for: %s", localPath.c_str());
        return false;
    }
    
    if (httpCode != 201 && httpCode != 200) {
        LOG_ERRORF("[SleepHQ] Upload returned HTTP %d for: %s", httpCode, localPath.c_str());
        LOG_ERRORF("[SleepHQ] Response: %s", responseBody.c_str());
        return false;
    }
    
    if (fileChecksum.length() == 0 && !calculatedFileChecksum.isEmpty()) {
        fileChecksum = calculatedFileChecksum;
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

bool SleepHQUploader::isTlsAlive() const {
    return tlsClient && tlsClient->connected();
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
    httpCode = -1;
    responseBody = "";

    char host[128];
    int port = 443;
    parseHostPort(host, sizeof(host), port);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!tlsClient) setupTLS();
        if (!tlsClient) {
            LOG_ERROR("[SleepHQ] TLS client not available (OOM)");
            return false;
        }

        // Connect if the keep-alive connection is dead
        if (!tlsClient->connected()) {
            uint32_t maxAlloc = ESP.getMaxAllocHeap();
            if (maxAlloc < 36000) {
                LOG_ERRORF("[SleepHQ] Insufficient contiguous heap for SSL (%u bytes), skipping request", maxAlloc);
                return false;
            }
            LOGF("[SleepHQ] TLS connecting (attempt %d, fh=%u, ma=%u)",
                 attempt + 1, ESP.getFreeHeap(), maxAlloc);
            esp_task_wdt_reset();
            if (!tlsClient->connect(host, port)) {
                LOG_ERRORF("[SleepHQ] TLS connect failed (attempt %d)", attempt + 1);
                if (attempt == 0) {
                    resetTLS();
                    if (WiFi.status() != WL_CONNECTED) {
                        LOG_WARN("[SleepHQ] WiFi disconnected, waiting for reconnection...");
                        unsigned long startWait = millis();
                        while (WiFi.status() != WL_CONNECTED && millis() - startWait < 10000) {
                            esp_task_wdt_reset();
                            delay(100);
                        }
                    } else {
                        tryCoordinatedWifiCycle(true);
                        resetTLS();
                    }
                    continue;
                }
                return false;
            }
            LOGF("[SleepHQ] TLS connected (fh=%u, ma=%u)",
                 ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        }

        // ── Send HTTP request via raw TLS (zero heap allocation) ─────────
        {
            char hdrBuf[256];
            int n;

            n = snprintf(hdrBuf, sizeof(hdrBuf), "%s %s HTTP/1.1\r\n", method.c_str(), path.c_str());
            tlsClient->write((const uint8_t*)hdrBuf, n);

            n = snprintf(hdrBuf, sizeof(hdrBuf), "Host: %s\r\n", host);
            tlsClient->write((const uint8_t*)hdrBuf, n);

            if (!accessToken.isEmpty()) {
                tlsClient->write((const uint8_t*)"Authorization: Bearer ", 22);
                tlsClient->write((const uint8_t*)accessToken.c_str(), accessToken.length());
                tlsClient->write((const uint8_t*)"\r\n", 2);
            }

            tlsClient->write((const uint8_t*)"Accept: application/vnd.api+json\r\n", 34);

            int bodyLen = body.length();
            if (bodyLen > 0 && !contentType.isEmpty()) {
                n = snprintf(hdrBuf, sizeof(hdrBuf), "Content-Type: %s\r\nContent-Length: %d\r\n",
                             contentType.c_str(), bodyLen);
            } else {
                n = snprintf(hdrBuf, sizeof(hdrBuf), "Content-Length: %d\r\n", bodyLen);
            }
            tlsClient->write((const uint8_t*)hdrBuf, n);
            tlsClient->write((const uint8_t*)"Connection: keep-alive\r\n\r\n", 26);

            if (bodyLen > 0) {
                tlsClient->write((const uint8_t*)body.c_str(), bodyLen);
            }
            tlsClient->flush();
        }

        esp_task_wdt_reset();
        extern volatile unsigned long g_uploadHeartbeat;
        g_uploadHeartbeat = millis();

        // ── Wait for response ────────────────────────────────────────────
        unsigned long timeout = millis() + 15000;
        while (!tlsClient->available() && millis() < timeout) {
            delay(10);
        }
        if (!tlsClient->available()) {
            LOG_WARN("[SleepHQ] Response timeout");
            if (attempt == 0) { resetTLS(); continue; }
            return false;
        }

        // ── Read status line ─────────────────────────────────────────────
        char lineBuf[256];
        int lineLen = 0;
        {
            unsigned long ld = millis() + 5000;
            while (millis() < ld) {
                if (!tlsClient->available()) { delay(2); continue; }
                int c = tlsClient->read();
                if (c < 0 || c == '\n') break;
                if (c != '\r' && lineLen < (int)sizeof(lineBuf) - 1) lineBuf[lineLen++] = (char)c;
            }
            lineBuf[lineLen] = '\0';
            const char* sp = strchr(lineBuf, ' ');
            httpCode = sp ? atoi(sp + 1) : -1;
        }

        // ── Parse response headers ───────────────────────────────────────
        long contentLength = -1;
        bool isChunked = false;
        bool connectionClose = false;
        unsigned long headerDl = millis() + 5000;
        while (millis() < headerDl) {
            if (!tlsClient->available()) { delay(2); continue; }
            lineLen = 0;
            while (millis() < headerDl) {
                if (!tlsClient->available()) { delay(2); continue; }
                int c = tlsClient->read();
                if (c < 0 || c == '\n') break;
                if (c != '\r' && lineLen < (int)sizeof(lineBuf) - 1) lineBuf[lineLen++] = (char)c;
            }
            lineBuf[lineLen] = '\0';
            if (lineLen == 0) break;
            if (strncasecmp(lineBuf, "Content-Length:", 15) == 0) {
                contentLength = atol(lineBuf + 15);
            } else if (strncasecmp(lineBuf, "Transfer-Encoding:", 18) == 0) {
                for (int ci = 18; lineBuf[ci]; ci++) lineBuf[ci] = tolower((unsigned char)lineBuf[ci]);
                if (strstr(lineBuf + 18, "chunked")) isChunked = true;
            } else if (strncasecmp(lineBuf, "Connection:", 11) == 0) {
                for (int ci = 11; lineBuf[ci]; ci++) lineBuf[ci] = tolower((unsigned char)lineBuf[ci]);
                if (strstr(lineBuf + 11, "close")) connectionClose = true;
            }
        }

        // ── Read response body into stack buffer ─────────────────────────
        char respBuf[1024];
        int respLen = 0;
        if (isChunked) {
            unsigned long chunkDl = millis() + 5000;
            while (millis() < chunkDl) {
                if (!tlsClient->available()) { delay(2); continue; }
                lineLen = 0;
                while (millis() < chunkDl) {
                    if (!tlsClient->available()) { delay(2); continue; }
                    int c = tlsClient->read();
                    if (c < 0 || c == '\n') break;
                    if (c != '\r' && lineLen < (int)sizeof(lineBuf) - 1) lineBuf[lineLen++] = (char)c;
                }
                lineBuf[lineLen] = '\0';
                long chunkSize = strtol(lineBuf, NULL, 16);
                if (chunkSize <= 0) {
                    unsigned long trailerDl = millis() + 2000;
                    while (tlsClient->available() && millis() < trailerDl) {
                        int c = tlsClient->read();
                        if (c == '\n' || c < 0) break;
                    }
                    break;
                }
                long remaining = chunkSize;
                while (remaining > 0 && millis() < chunkDl) {
                    if (!tlsClient->available()) { delay(2); continue; }
                    int c = tlsClient->read();
                    if (c < 0) break;
                    if (respLen < (int)sizeof(respBuf) - 1) respBuf[respLen++] = (char)c;
                    remaining--;
                }
                while (tlsClient->available()) {
                    int c = tlsClient->read();
                    if (c == '\n' || c < 0) break;
                }
            }
        } else if (contentLength > 0) {
            long remaining = contentLength;
            unsigned long bodyDl = millis() + 5000;
            while (remaining > 0 && millis() < bodyDl) {
                if (!tlsClient->available()) { delay(2); continue; }
                int c = tlsClient->read();
                if (c < 0) break;
                if (respLen < (int)sizeof(respBuf) - 1) respBuf[respLen++] = (char)c;
                remaining--;
            }
        } else {
            unsigned long bodyDl = millis() + 2000;
            while (millis() < bodyDl) {
                if (!tlsClient->available()) { delay(2); if (!tlsClient->connected()) break; continue; }
                int c = tlsClient->read();
                if (c < 0) break;
                if (respLen < (int)sizeof(respBuf) - 1) respBuf[respLen++] = (char)c;
                bodyDl = millis() + 500;
            }
        }
        respBuf[respLen] = '\0';
        responseBody = respBuf;

        // Honour Connection: close — tear down gracefully so the next request
        // reconnects into a fresh TLS session without stale socket state.
        if (connectionClose) {
            tlsClient->stop();
        }

        g_uploadHeartbeat = millis();

        if (httpCode > 0) return true;

        // Negative/zero code on attempt 0 → retry
        if (attempt == 0) {
            LOG_WARNF("[SleepHQ] HTTP %s returned code %d, retrying...", method.c_str(), httpCode);
            resetTLS();
            continue;
        }
        LOG_ERRORF("[SleepHQ] HTTP request failed after retry (code %d)", httpCode);
        return false;
    }
    return false;
}

bool SleepHQUploader::httpMultipartUpload(const String& path, const String& fileName,
                                           const String& filePath, const String& contentHash,
                                           unsigned long lockedFileSize,
                                           fs::FS &sd, unsigned long& bytesTransferred,
                                           String& responseBody, int& httpCode,
                                           String* calculatedChecksum,
                                           bool useKeepAlive) {
    if (!tlsClient) {
        setupTLS();
    }
    
    if (!tlsClient) {
        LOG_ERROR("[SleepHQ] TLS client not available (OOM)");
        return false;
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
    
    // Extract the directory path for the 'path' field (stack-allocated to avoid heap churn)
    char dirPath[128];
    {
        const char* fp = filePath.c_str();
        const char* ls = strrchr(fp, '/');
        if (ls && ls > fp) {
            size_t dirLen = ls - fp;
            if (dirLen >= sizeof(dirPath)) dirLen = sizeof(dirPath) - 1;
            memcpy(dirPath, fp, dirLen);
            dirPath[dirLen] = '\0';
        } else {
            strcpy(dirPath, "/");
        }
    }
    
    // Build multipart boundary on stack (no heap allocation)
    char boundary[32];
    snprintf(boundary, sizeof(boundary), "----ESP32Boundary%lu", millis());
    
    // Calculate exact part lengths using snprintf(NULL,0,...) — zero heap allocation
    const char* fnStr = fileName.c_str();
    int head1Len = snprintf(NULL, 0, "--%s\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\n%s\r\n",
                            boundary, fnStr);
    int head2Len = snprintf(NULL, 0, "--%s\r\nContent-Disposition: form-data; name=\"path\"\r\n\r\n%s\r\n",
                            boundary, dirPath);
    int head3Len = snprintf(NULL, 0, "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                            "Content-Type: application/octet-stream\r\n\r\n",
                            boundary, fnStr);
    int foot1Len = snprintf(NULL, 0, "\r\n--%s\r\nContent-Disposition: form-data; name=\"content_hash\"\r\n\r\n",
                            boundary);
    int foot2Len = snprintf(NULL, 0, "\r\n--%s--\r\n", boundary);
    
    // Calculate total content length
    unsigned long totalLength = head1Len + head2Len + head3Len + 
                                fileSize + 
                                foot1Len + 32 + foot2Len;
    
    // Always stream multipart payloads.
    // This avoids large per-file temporary allocations (and HTTPClient internal allocations)
    // that can fragment heap and trigger TLS allocation failures later in the session.
    file.close();
    
    // Parse host and port from base URL — stack buffer to avoid heap churn
    char host[128];
    int port = 443;
    parseHostPort(host, sizeof(host), port);
    
    // Retry loop for streaming upload (same pattern as in-memory path)
    for (int attempt = 0; attempt < 2; attempt++) {
        // Ensure we have a valid TLS client before attempting
        if (!tlsClient) {
            LOG_ERROR("[SleepHQ] TLS client lost during retry");
            return false;
        }

        // Reuse existing TLS connection if still alive (keep-alive from previous request)
        if (!tlsClient->connected()) {
            LOGF("[SleepHQ] Streaming: establishing TLS connection (attempt %d, free: %u, max_alloc: %u)",
                 attempt + 1, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            if (!tlsClient->connect(host, port)) {
                LOG_ERROR("[SleepHQ] Failed to connect for streaming upload");
                
                if (attempt == 0) {
                    resetTLS(); // Full reset instead of just stop()
                    
                    // Check WiFi status and wait if disconnected
                    if (WiFi.status() != WL_CONNECTED) {
                        LOG_WARN("[SleepHQ] WiFi disconnected during connect, waiting for reconnection...");
                        unsigned long startWait = millis();
                        while (WiFi.status() != WL_CONNECTED && millis() - startWait < 10000) {
                            extern volatile unsigned long g_uploadHeartbeat;
                            g_uploadHeartbeat = millis();  // Feed software watchdog
                            delay(100);
                        }
                        if (WiFi.status() == WL_CONNECTED) {
                            LOG_INFO("[SleepHQ] WiFi reconnected");
                        } else {
                            LOG_ERROR("[SleepHQ] WiFi still disconnected");
                        }
                    } else {
                        // WiFi connected but TLS connect failed — attempt coordinated cycle.
                        // Skips if SMB holds an active connection or cooldown has not elapsed.
                        // Only reset TLS again if the cycle actually ran (WiFi was cycled);
                        // if skipped, the fresh client from the resetTLS() above is still good.
                        if (tryCoordinatedWifiCycle(true)) {
                            resetTLS();
                            if (!tlsClient) {
                                LOG_ERROR("[SleepHQ] TLS client allocation failed after WiFi cycle (OOM)");
                                return false;
                            }
                        }
                    }
                    
                    continue; 
                }
                return false;
            }
        } else {
            LOG_DEBUG("[SleepHQ] Streaming: reusing existing TLS connection (keep-alive)");
        }
        
        // Send HTTP POST request headers — use stack buffer to avoid String heap churn
        {
            char hdrBuf[256];
            int n;
            n = snprintf(hdrBuf, sizeof(hdrBuf), "POST %s HTTP/1.1\r\n", path.c_str());
            tlsClient->write((const uint8_t*)hdrBuf, n);
            n = snprintf(hdrBuf, sizeof(hdrBuf), "Host: %s\r\n", host);
            tlsClient->write((const uint8_t*)hdrBuf, n);
            n = snprintf(hdrBuf, sizeof(hdrBuf), "Authorization: Bearer %s\r\n", accessToken.c_str());
            tlsClient->write((const uint8_t*)hdrBuf, n);
            n = snprintf(hdrBuf, sizeof(hdrBuf), "Accept: application/vnd.api+json\r\n");
            tlsClient->write((const uint8_t*)hdrBuf, n);
            n = snprintf(hdrBuf, sizeof(hdrBuf), "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
            tlsClient->write((const uint8_t*)hdrBuf, n);
            n = snprintf(hdrBuf, sizeof(hdrBuf), "Content-Length: %lu\r\n", totalLength);
            tlsClient->write((const uint8_t*)hdrBuf, n);
            if (useKeepAlive) {
                tlsClient->write((const uint8_t*)"Connection: keep-alive\r\n\r\n", 26);
            } else {
                tlsClient->write((const uint8_t*)"Connection: close\r\n\r\n", 21);
            }
        }
        
        // Send multipart preamble — stack buffer to avoid heap churn
        {
            char partBuf[384];
            int n;
            n = snprintf(partBuf, sizeof(partBuf), "--%s\r\nContent-Disposition: form-data; name=\"name\"\r\n\r\n%s\r\n",
                         boundary, fnStr);
            tlsClient->write((const uint8_t*)partBuf, n);
            n = snprintf(partBuf, sizeof(partBuf), "--%s\r\nContent-Disposition: form-data; name=\"path\"\r\n\r\n%s\r\n",
                         boundary, dirPath);
            tlsClient->write((const uint8_t*)partBuf, n);
            n = snprintf(partBuf, sizeof(partBuf), "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                         "Content-Type: application/octet-stream\r\n\r\n",
                         boundary, fnStr);
            tlsClient->write((const uint8_t*)partBuf, n);
        }
        
        // Re-open and stream file
        file = sd.open(filePath, FILE_READ);
        if (!file) {
            LOG_ERROR("[SleepHQ] Cannot re-open file for streaming");
            return false;
        }
        
        md5_context_t md5ctx;
        esp_rom_md5_init(&md5ctx);
        
        uint8_t buffer[CLOUD_UPLOAD_BUFFER_SIZE_MAX];
        // Adaptive chunk size: smaller reads when heap is constrained
        // to reduce peak current from concurrent SD + TLS + WiFi operations
        const size_t adaptiveChunk = getAdaptiveBufferSize();
        unsigned long totalSent = 0;
        bool writeError = false;
        int readRetries = 0;
        
        while (totalSent < fileSize) {
            size_t toRead = adaptiveChunk;
            if (fileSize - totalSent < toRead) {
                toRead = fileSize - totalSent;
            }
            
            size_t bytesRead = file.read(buffer, toRead);
            if (bytesRead == 0) {
                // Unexpected EOF or read error - try to recover
                if (readRetries < 3) {
                    LOG_WARNF("[SleepHQ] Read returned 0 at %lu/%lu, retrying (%d/3)...", totalSent, fileSize, readRetries + 1);
                    delay(100);
                    readRetries++;
                    continue;
                }
                LOG_ERRORF("[SleepHQ] File read failed at %lu/%lu bytes", totalSent, fileSize);
                break;
            }
            readRetries = 0; // Reset retry counter on success
            
            // Update checksum with file data
            esp_rom_md5_update(&md5ctx, buffer, bytesRead);
            
            // Write to TLS with retry and partial write handling
            size_t remainingToWrite = bytesRead;
            uint8_t* writePtr = buffer;
            int writeRetries = 0;
            
            while (remainingToWrite > 0) {
                size_t written = tlsClient->write(writePtr, remainingToWrite);
                
                if (written > 0) {
                    remainingToWrite -= written;
                    writePtr += written;
                    totalSent += written;
                    writeRetries = 0; // Reset retry counter on success
                } else {
                    // Write failed or returned 0 (buffer full / EAGAIN)
                    if (!tlsClient->connected()) {
                        LOG_ERROR("[SleepHQ] Connection lost during write");
                        writeError = true;
                        break;
                    }
                    
                    if (writeRetries < 10) {
                        LOG_WARNF("[SleepHQ] Write returned 0/fail at %lu/%lu, retrying (%d/10)...", totalSent, fileSize, writeRetries + 1);
                        delay(500); // Wait longer for socket buffer to drain
                        writeRetries++;
                        yield();
                        continue;
                    } else {
                        LOG_ERRORF("[SleepHQ] Write timeout/fail after 10 retries at %lu/%lu", totalSent, fileSize);
                        writeError = true;
                        break;
                    }
                }
            }
            
            if (writeError) {
                break;
            }
            
            // Feed software watchdog during large file streaming
            extern volatile unsigned long g_uploadHeartbeat;
            g_uploadHeartbeat = millis();
            
            // ── POWER: Yield between chunks to allow DFS frequency scaling ──
            // Without yields, the upload loop monopolizes the CPU at max frequency.
            // taskYIELD() lets the IDLE task run briefly, allowing the DFS governor
            // to scale down if no other high-priority work is pending.
            taskYIELD();
        }
        file.close();

        if (totalSent != fileSize) {
            if (writeError) {
                LOG_WARNF("[SleepHQ] Upload interrupted by write error (%lu/%lu bytes), reconnecting...", totalSent, fileSize);
            } else {
                LOG_WARNF("[SleepHQ] Short read from file (%lu/%lu bytes), reconnecting...", totalSent, fileSize);
            }
            resetTLS(); // Full reset to clear FDs
            
            // resetTLS() above already created a fresh WiFiClientSecure.
            // Attempt a coordinated cycle only if SMB is idle and cooldown allows;
            // write errors are usually transient buffer pressure, not dead WiFi.
            tryCoordinatedWifiCycle(true);
            
            if (attempt == 0) {
                continue;
            }
            return false;
        }
        
        if (writeError) {
            resetTLS(); // Full reset to clear FDs
            
            // Attempt a coordinated cycle only if SMB is idle and cooldown allows.
            tryCoordinatedWifiCycle(true);
            
            if (attempt == 0) {
                LOG_WARN("[SleepHQ] Streaming write failed, reconnecting...");
                continue;
            }
            return false;
        }
        
        // Append filename to hash: content_hash = MD5(file_content + filename)
        esp_rom_md5_update(&md5ctx, (const uint8_t*)fnStr, strlen(fnStr));
        
        // Finalize checksum — use stack buffer, no String allocation
        uint8_t digest[16];
        esp_rom_md5_final(digest, &md5ctx);
        char hashStr[33];
        for (int i = 0; i < 16; i++) {
            sprintf(hashStr + (i * 2), "%02x", digest[i]);
        }
        hashStr[32] = '\0';
        
        if (calculatedChecksum) {
            *calculatedChecksum = String(hashStr);
        }
        
        // Send footer with hash — stack buffer to avoid heap churn
        {
            char partBuf[384];
            int n;
            n = snprintf(partBuf, sizeof(partBuf), "\r\n--%s\r\nContent-Disposition: form-data; name=\"content_hash\"\r\n\r\n",
                         boundary);
            tlsClient->write((const uint8_t*)partBuf, n);
            tlsClient->write((const uint8_t*)hashStr, 32);
            n = snprintf(partBuf, sizeof(partBuf), "\r\n--%s--\r\n", boundary);
            tlsClient->write((const uint8_t*)partBuf, n);
        }
        tlsClient->flush();
        
        // Read response status line
        unsigned long timeout = millis() + 30000;
        while (!tlsClient->available() && millis() < timeout) {
            delay(10);
        }

        if (!tlsClient->available()) {
            LOG_WARN("[SleepHQ] Streaming response timeout, reconnecting...");
            resetTLS(); // Full reset to clear FDs
            
            // Check WiFi status
            if (WiFi.status() != WL_CONNECTED) {
                LOG_WARN("[SleepHQ] WiFi disconnected awaiting response, waiting for reconnection...");
                unsigned long startWait = millis();
                while (WiFi.status() != WL_CONNECTED && millis() - startWait < 10000) {
                    extern volatile unsigned long g_uploadHeartbeat;
                    g_uploadHeartbeat = millis();
                    delay(100);
                }
            }
            
            if (attempt == 0) {
                continue;
            }
            return false;
        }
        
        // Parse status line — stack buffer, no heap alloc
        char lineBuf[256];
        int lineLen = 0;
        {
            unsigned long ld = millis() + 5000;
            while (millis() < ld) {
                if (!tlsClient->available()) { delay(2); continue; }
                int c = tlsClient->read();
                if (c < 0 || c == '\n') break;
                if (c != '\r' && lineLen < (int)sizeof(lineBuf) - 1) lineBuf[lineLen++] = (char)c;
            }
            lineBuf[lineLen] = '\0';
        }
        {
            const char* sp = strchr(lineBuf, ' ');
            httpCode = sp ? atoi(sp + 1) : -1;
        }
        
        // Parse response headers — stack buffer, no heap alloc
        long contentLength = -1;
        bool isChunked = false;
        bool connectionClose = false;
        bool headersDone = false;
        unsigned long headerDeadline = millis() + 5000;
        while (millis() < headerDeadline) {
            if (!tlsClient->available()) {
                delay(2);
                continue;
            }
            lineLen = 0;
            while (millis() < headerDeadline) {
                if (!tlsClient->available()) { delay(2); continue; }
                int c = tlsClient->read();
                if (c < 0 || c == '\n') break;
                if (c != '\r' && lineLen < (int)sizeof(lineBuf) - 1) lineBuf[lineLen++] = (char)c;
            }
            lineBuf[lineLen] = '\0';
            if (lineLen == 0) {
                headersDone = true;
                break;
            }
            if (strncasecmp(lineBuf, "Content-Length:", 15) == 0) {
                contentLength = atol(lineBuf + 15);
            } else if (strncasecmp(lineBuf, "Transfer-Encoding:", 18) == 0) {
                for (int ci = 18; lineBuf[ci]; ci++) lineBuf[ci] = tolower((unsigned char)lineBuf[ci]);
                if (strstr(lineBuf + 18, "chunked")) isChunked = true;
            } else if (strncasecmp(lineBuf, "Connection:", 11) == 0) {
                for (int ci = 11; lineBuf[ci]; ci++) lineBuf[ci] = tolower((unsigned char)lineBuf[ci]);
                if (strstr(lineBuf + 11, "close")) connectionClose = true;
            }
        }

        if (!headersDone) {
            LOG_WARN("[SleepHQ] Incomplete response headers, reconnecting...");
            resetTLS(); // Full reset to clear FDs
            
            // Check WiFi status
            if (WiFi.status() != WL_CONNECTED) {
                LOG_WARN("[SleepHQ] WiFi disconnected during headers, waiting for reconnection...");
                unsigned long startWait = millis();
                while (WiFi.status() != WL_CONNECTED && millis() - startWait < 10000) {
                    extern volatile unsigned long g_uploadHeartbeat;
                    g_uploadHeartbeat = millis();
                    delay(100);
                }
            }
            
            if (attempt == 0) {
                continue;
            }
            return false;
        }
        
        // Drain response body — stack buffer to avoid heap fragmentation
        // (responseBody String built char-by-char was the #1 fragmentation source)
        char respBuf[1024];
        int respLen = 0;
        if (isChunked) {
            unsigned long chunkDeadline = millis() + 5000;
            while (millis() < chunkDeadline) {
                if (!tlsClient->available()) {
                    delay(2);
                    continue;
                }
                // Read chunk size line — stack buffer
                lineLen = 0;
                while (millis() < chunkDeadline) {
                    if (!tlsClient->available()) { delay(2); continue; }
                    int c = tlsClient->read();
                    if (c < 0 || c == '\n') break;
                    if (c != '\r' && lineLen < (int)sizeof(lineBuf) - 1) lineBuf[lineLen++] = (char)c;
                }
                lineBuf[lineLen] = '\0';
                long chunkSize = strtol(lineBuf, NULL, 16);
                
                if (chunkSize <= 0) {
                    // End chunk — drain trailers using stack buffer
                    unsigned long trailerDl = millis() + 2000;
                    while (tlsClient->available() && millis() < trailerDl) {
                        lineLen = 0;
                        while (millis() < trailerDl) {
                            if (!tlsClient->available()) { delay(2); continue; }
                            int c = tlsClient->read();
                            if (c < 0 || c == '\n') break;
                            if (c != '\r' && lineLen < (int)sizeof(lineBuf) - 1) lineBuf[lineLen++] = (char)c;
                        }
                        if (lineLen == 0) break;
                    }
                    break;
                }
                
                // Read chunk data
                long remaining = chunkSize;
                while (remaining > 0 && millis() < chunkDeadline) {
                    if (!tlsClient->available()) { delay(2); continue; }
                    uint8_t drainBuf[256];
                    size_t toRead = (remaining < (long)sizeof(drainBuf)) ? remaining : sizeof(drainBuf);
                    size_t r = tlsClient->read(drainBuf, toRead);
                    if (r == 0) break;
                    for (size_t i = 0; i < r && respLen < (int)sizeof(respBuf) - 1; i++) {
                        respBuf[respLen++] = (char)drainBuf[i];
                    }
                    remaining -= r;
                    chunkDeadline = millis() + 5000;
                }
                
                // Read trailing CRLF — byte-by-byte, no heap alloc
                while (tlsClient->available()) {
                    int c = tlsClient->read();
                    if (c == '\n' || c < 0) break;
                }
            }
        } else if (contentLength > 0) {
            long remaining = contentLength;
            unsigned long bodyDeadline = millis() + 5000;
            while (remaining > 0 && millis() < bodyDeadline) {
                if (!tlsClient->available()) {
                    delay(2);
                    continue;
                }
                uint8_t drainBuf[256];
                size_t toRead = (remaining < (long)sizeof(drainBuf)) ? remaining : sizeof(drainBuf);
                size_t r = tlsClient->read(drainBuf, toRead);
                if (r == 0) break;
                for (size_t i = 0; i < r && respLen < (int)sizeof(respBuf) - 1; i++) {
                    respBuf[respLen++] = (char)drainBuf[i];
                }
                remaining -= r;
                bodyDeadline = millis() + 5000;
            }
            if (remaining > 0) {
                LOG_WARNF("[SleepHQ] Response body not fully drained (%ld bytes left), resetting TLS", remaining);
                tlsClient->stop();
            }
        } else {
            // Unknown body length: drain briefly
            unsigned long drainDeadline = millis() + 300;
            while (millis() < drainDeadline) {
                while (tlsClient->available()) {
                    int c = tlsClient->read();
                    if (c >= 0 && respLen < (int)sizeof(respBuf) - 1) {
                        respBuf[respLen++] = (char)c;
                    }
                    drainDeadline = millis() + 300;
                }
                delay(2);
            }
            tlsClient->stop();
        }
        
        // Single heap allocation from stack buffer (only for error logging)
        respBuf[respLen] = '\0';
        responseBody = respBuf;
        
        // Keep TLS alive unless server requested close
        if (connectionClose) {
            tlsClient->stop();
        }

        bytesTransferred = totalSent;
        
        // Feed software watchdog after successful file upload
        {
            extern volatile unsigned long g_uploadHeartbeat;
            g_uploadHeartbeat = millis();
        }
        
        return httpCode > 0;
    }
    return false;
}

#endif // ENABLE_SLEEPHQ_UPLOAD
