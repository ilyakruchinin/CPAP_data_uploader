#include "TestWebServer.h"
#include "Logger.h"
#include "UploadFSM.h"
#include "version.h"
#include <time.h>

// Global trigger flags
volatile bool g_triggerUploadFlag = false;
volatile bool g_resetStateFlag = false;

// Monitoring trigger flags
volatile bool g_monitorActivityFlag = false;
volatile bool g_stopMonitorFlag = false;

// External FSM state (defined in main.cpp)
extern UploadState currentState;
extern unsigned long stateEnteredAt;
extern unsigned long cooldownStartedAt;

// Constructor
TestWebServer::TestWebServer(Config* cfg, UploadStateManager* state,
                             ScheduleManager* schedule, 
                             WiFiManager* wifi, CPAPMonitor* monitor)
    : server(nullptr),
      config(cfg),
      stateManager(state),
      scheduleManager(schedule),
      wifiManager(wifi),
      cpapMonitor(monitor),
      trafficMonitor(nullptr)
#ifdef ENABLE_OTA_UPDATES
      , otaManager(nullptr)
#endif
{
}

// Destructor
TestWebServer::~TestWebServer() {
    if (server) {
        server->stop();
        delete server;
    }
}

// Initialize and start the web server
bool TestWebServer::begin() {
    LOG("[TestWebServer] Initializing web server on port 80...");
    
    server = new WebServer(80);
    
    // Register request handlers
    server->on("/", [this]() { this->handleRoot(); });
    server->on("/trigger-upload", [this]() { this->handleTriggerUpload(); });
    
    // HTML Views
    server->on("/status", [this]() { this->handleStatusPage(); });
    server->on("/config", [this]() { this->handleConfigPage(); });
    server->on("/logs", [this]() { this->handleLogs(); });
    server->on("/monitor", [this]() { this->handleMonitorPage(); });
    
    // APIs
    server->on("/api/status", [this]() { this->handleApiStatus(); });
    server->on("/api/config", [this]() { this->handleApiConfig(); });
    server->on("/api/logs", [this]() { this->handleApiLogs(); });
    server->on("/api/monitor-start", [this]() { this->handleMonitorStart(); });
    server->on("/api/monitor-stop", [this]() { this->handleMonitorStop(); });
    server->on("/api/sd-activity", [this]() { this->handleSdActivity(); });
    server->on("/reset-state", [this]() { this->handleResetState(); });
    
#ifdef ENABLE_OTA_UPDATES
    // OTA handlers
    server->on("/ota", [this]() { this->handleOTAPage(); });
    server->on("/ota-upload", HTTP_POST, 
               [this]() { this->handleOTAUploadComplete(); },
               [this]() { this->handleOTAUpload(); });
    server->on("/ota-url", HTTP_POST, [this]() { this->handleOTAURL(); });
#endif
    
    // Handle common browser requests silently
    server->on("/favicon.ico", [this]() { 
        // Return empty 204 No Content with proper content type
        server->send(204, "text/plain", ""); 
    });
    
    server->onNotFound([this]() { this->handleNotFound(); });
    
    // Start the server
    server->begin();
    
    LOG("[TestWebServer] Web server started successfully");
    LOG("[TestWebServer] Available endpoints:");
    LOG("[TestWebServer]   GET /              - Status page (HTML)");
    LOG("[TestWebServer]   GET /trigger-upload - Force immediate upload");
    LOG("[TestWebServer]   GET /status        - Status information (JSON)");
    LOG("[TestWebServer]   GET /reset-state   - Clear upload state");
    LOG("[TestWebServer]   GET /config        - Display configuration");
    LOG("[TestWebServer]   GET /logs          - Retrieve system logs (JSON)");
    LOG("[TestWebServer]   GET /monitor       - SD Activity Monitor (live)");
    
    return true;
}

// Process incoming HTTP requests
void TestWebServer::handleClient() {
    if (server) {
        server->handleClient();
    }
}

// GET / - HTML status page (modern dark dashboard)
void TestWebServer::handleRoot() {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);

    auto sendChunk = [this](const String& s) {
        server->sendContent(s);
    };

    String html = "<!DOCTYPE html><html><head>";
    html += "<title>CPAP Data Uploader</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='5'>";
    html += "<style>";
    html += "*{box-sizing:border-box;margin:0;padding:0}";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;";
    html += "background:#0f1923;color:#c7d5e0;min-height:100vh;padding:20px}";
    html += ".wrap{max-width:900px;margin:0 auto}";
    html += "h1{font-size:1.6em;color:#fff;margin-bottom:4px}";
    html += ".subtitle{color:#66c0f4;font-size:0.9em;margin-bottom:20px}";
    html += ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:16px;margin-bottom:20px}";
    html += ".card{background:#1b2838;border:1px solid #2a475e;border-radius:10px;padding:18px}";
    html += ".card h2{font-size:0.85em;text-transform:uppercase;letter-spacing:1px;color:#66c0f4;margin-bottom:12px;border-bottom:1px solid #2a475e;padding-bottom:8px}";
    html += ".row{display:flex;justify-content:space-between;padding:5px 0;font-size:0.88em}";
    html += ".row .k{color:#8f98a0}.row .v{color:#c7d5e0;font-weight:500;text-align:right}";
    // FSM state badge
    html += ".fsm{display:inline-block;padding:4px 12px;border-radius:20px;font-weight:700;font-size:0.8em;letter-spacing:0.5px}";
    html += ".fsm-idle{background:#2a475e;color:#8f98a0}";
    html += ".fsm-listening{background:#1a3a1a;color:#44ff44;animation:pulse 2s infinite}";
    html += ".fsm-acquiring,.fsm-uploading{background:#1a2a4a;color:#66c0f4}";
    html += ".fsm-cooldown{background:#3a2a1a;color:#ffaa44}";
    html += ".fsm-complete{background:#1a3a1a;color:#44ff44}";
    html += ".fsm-monitoring{background:#2a1a3a;color:#cc66ff}";
    html += ".fsm-releasing{background:#3a2a1a;color:#ffaa44}";
    html += "@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.6}}";
    // Progress bar
    html += ".prog-bar{background:#2a475e;border-radius:6px;height:10px;margin-top:6px;overflow:hidden}";
    html += ".prog-fill{background:linear-gradient(90deg,#66c0f4,#44aaff);height:100%;border-radius:6px;transition:width 0.5s}";
    // Buttons
    html += ".actions{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}";
    html += ".btn{display:inline-flex;align-items:center;gap:6px;padding:10px 18px;border-radius:6px;font-size:0.85em;font-weight:600;text-decoration:none;border:none;cursor:pointer;transition:all 0.2s}";
    html += ".btn-primary{background:#66c0f4;color:#0f1923}.btn-primary:hover{background:#88d0ff}";
    html += ".btn-secondary{background:#2a475e;color:#c7d5e0}.btn-secondary:hover{background:#3a5a7e}";
    html += ".btn-accent{background:#9b59b6;color:#fff}.btn-accent:hover{background:#b06ed0}";
    html += ".btn-danger{background:#c0392b;color:#fff}.btn-danger:hover{background:#e04030}";
    html += ".btn-loading{background:#3f6d88;color:#dbe7ef;cursor:progress;opacity:0.92}";
    // WiFi signal colors
    html += ".sig-exc{color:#44ff44}.sig-good{color:#88dd44}.sig-fair{color:#ddcc44}.sig-weak{color:#dd8844}.sig-vweak{color:#dd4444}";
    // Alert
    html += ".alert{background:#3a2a1a;border:1px solid #aa6622;border-radius:8px;padding:14px;margin-bottom:16px}";
    html += ".alert h3{color:#ffaa44;font-size:0.9em;margin-bottom:6px}";
    html += ".alert p{font-size:0.85em;color:#c7d5e0;margin:3px 0}";
    // Toast notification (for inline actions without navigation)
    html += ".toast{position:fixed;right:16px;bottom:16px;max-width:340px;background:#1b2838;border:1px solid #2a475e;color:#c7d5e0;padding:10px 12px;border-radius:8px;font-size:0.85em;box-shadow:0 6px 24px rgba(0,0,0,0.35);opacity:0;transform:translateY(8px);transition:opacity 0.2s,transform 0.2s;pointer-events:none;z-index:9999}";
    html += ".toast.show{opacity:1;transform:translateY(0)}";
    html += ".toast.ok{border-color:#2f8f57}";
    html += ".toast.err{border-color:#c0392b}";
    html += "</style></head><body><div class='wrap'>";
    
    // Send initial chunk with headers
    server->send(200, "text/html; charset=utf-8", html);

    // ── Header ──
    html = "<h1>CPAP Data Uploader</h1>";
    html += "<p class='subtitle'>Firmware " + String(FIRMWARE_VERSION) + " &middot; " + getUptimeString() + " uptime</p>";
    sendChunk(html);

    // ── FSM State + System card (side by side) ──
    html = "<div class='cards'>";
    
    // FSM State card
    html += "<div class='card'>";
    html += "<h2>Upload Engine</h2>";
    String stName = String(getStateName(currentState));
    stName.toLowerCase();
    html += "<div class='row'><span class='k'>State</span><span class='fsm fsm-" + stName + "'>" + String(getStateName(currentState)) + "</span></div>";
    
    // State duration
    unsigned long inStateSec = (millis() - stateEnteredAt) / 1000;
    String inStateStr;
    if (inStateSec >= 3600) inStateStr = String(inStateSec / 3600) + "h " + String((inStateSec % 3600) / 60) + "m";
    else if (inStateSec >= 60) inStateStr = String(inStateSec / 60) + "m " + String(inStateSec % 60) + "s";
    else inStateStr = String(inStateSec) + "s";
    html += "<div class='row'><span class='k'>In state for</span><span class='v'>" + inStateStr + "</span></div>";
    
    // Mode and window from ScheduleManager
    if (scheduleManager) {
        String mode = scheduleManager->isSmartMode() ? "Smart" : "Scheduled";
        html += "<div class='row'><span class='k'>Mode</span><span class='v'>" + mode + "</span></div>";
        html += "<div class='row'><span class='k'>Time synced</span><span class='v'>" + String(scheduleManager->isTimeSynced() ? "Yes" : "No") + "</span></div>";
    }
    if (config) {
        html += "<div class='row'><span class='k'>Upload window</span><span class='v'>" + String(config->getUploadStartHour()) + ":00 - " + String(config->getUploadEndHour()) + ":00</span></div>";
        html += "<div class='row'><span class='k'>Inactivity threshold</span><span class='v'>" + String(config->getInactivitySeconds()) + "s</span></div>";
        html += "<div class='row'><span class='k'>Exclusive access</span><span class='v'>" + String(config->getExclusiveAccessMinutes()) + " min</span></div>";
        html += "<div class='row'><span class='k'>Cooldown</span><span class='v'>" + String(config->getCooldownMinutes()) + " min</span></div>";
    }
    html += "</div>";
    sendChunk(html);

    // System card
    html = "<div class='card'>";
    html += "<h2>System</h2>";
    html += "<div class='row'><span class='k'>Time</span><span class='v'>" + getCurrentTimeString() + "</span></div>";
    html += "<div class='row'><span class='k'>Free heap</span><span class='v'>" + String(ESP.getFreeHeap() / 1024) + " KB</span></div>";
    
    if (wifiManager && wifiManager->isConnected()) {
        int rssi = wifiManager->getSignalStrength();
        String quality = wifiManager->getSignalQuality();
        String sigClass = "sig-fair";
        if (quality == "Excellent") sigClass = "sig-exc";
        else if (quality == "Good") sigClass = "sig-good";
        else if (quality == "Fair") sigClass = "sig-fair";
        else if (quality == "Weak") sigClass = "sig-weak";
        else sigClass = "sig-vweak";
        html += "<div class='row'><span class='k'>WiFi</span><span class='v " + sigClass + "'>" + quality + " (" + String(rssi) + " dBm)</span></div>";
        html += "<div class='row'><span class='k'>IP</span><span class='v'>" + wifiManager->getIPAddress() + "</span></div>";
    }
    
    if (config) {
        html += "<div class='row'><span class='k'>Endpoint</span><span class='v'>" + config->getEndpointType() + "</span></div>";
        html += "<div class='row'><span class='k'>GMT offset</span><span class='v'>+" + String(config->getGmtOffsetHours()) + "</span></div>";
    }
    html += "</div>";
    html += "</div>"; // end .cards row 1
    sendChunk(html);

    // ── Upload Progress card (full width) ──
    html = "<div class='cards'>";
    html += "<div class='card' style='grid-column:1/-1'>";
    html += "<h2>Upload Progress</h2>";
    
    if (stateManager) {
        int completed = stateManager->getCompletedFoldersCount();
        int incomplete = stateManager->getIncompleteFoldersCount();
        int pending = stateManager->getPendingFoldersCount();
        int dataTotal = completed + incomplete;
        
        if (dataTotal == 0 && pending == 0) {
            html += "<div class='row'><span class='k'>Status</span><span class='v'>Waiting for first scan</span></div>";
        } else {
            if (scheduleManager && config && config->getMaxDays() > 0 && !scheduleManager->isTimeSynced()) {
                html += "<div class='row' style='margin-top:6px'><span class='k'>Warning</span><span class='v' style='color:#ffaa44'>Time not synced: MAX_DAYS cutoff is inactive, so all DATALOG folders may be processed this cycle</span></div>";
            }

            if (dataTotal > 0) {
                int pct = (completed * 100 / dataTotal);
                html += "<div class='row'><span class='k'>Data folders</span><span class='v'>" + String(completed) + " / " + String(dataTotal) + " uploaded</span></div>";
                html += "<div class='prog-bar'><div class='prog-fill' style='width:" + String(pct) + "%'></div></div>";

                if (incomplete > 0) {
                    html += "<div class='row' style='margin-top:6px'><span class='k'>Pending data folders</span><span class='v' style='color:#ffaa44'>" + String(incomplete) + " (will continue in next cycle)</span></div>";
                } else {
                    html += "<div class='row' style='margin-top:6px'><span class='k'>Data upload status</span><span class='v' style='color:#66dd88'>Complete</span></div>";
                }
            }

            if (pending > 0) {
                html += "<div class='row' style='margin-top:6px'><span class='k'>Empty folders</span><span class='v'>" + String(pending) + " pending (auto-complete after 7 days)</span></div>";
                if (incomplete == 0) {
                    html += "<div class='row'><span class='k'>Status</span><span class='v'>Waiting on empty-folder aging</span></div>";
                }
            }
        }
    }
    html += "</div></div>"; // end progress card + cards
    sendChunk(html);

    // ── Actions ──
    html = "<div class='card' style='margin-bottom:20px'>";
    html += "<h2>Actions</h2>";
    html += "<div class='actions'>";
    html += "<button id='trigger-upload-btn' class='btn btn-primary' onclick='triggerUpload()'>&#9650; Trigger Upload</button>";
    html += "<span id='trigger-upload-status' style='font-size:0.82em;color:#8f98a0;align-self:center'></span>";
    html += "<a href='/monitor' class='btn btn-accent'>&#128200; SD Activity Monitor</a>";
    html += "</div>";
    
    html += "<div class='actions' style='margin-top:10px'>";
    html += "<a href='/status' class='btn btn-secondary'>Status</a>";
    html += "<a href='/config' class='btn btn-secondary'>Config</a>";
    html += "<a href='/logs' class='btn btn-secondary'>Logs</a>";
#ifdef ENABLE_OTA_UPDATES
    html += "<a href='/ota' class='btn btn-secondary'>&#128190; Firmware Update</a>";
#endif
    html += "<a href='/reset-state' class='btn btn-danger' onclick='return confirm(\"Reset all upload state? This cannot be undone.\")'>Reset State</a>";
    html += "</div></div>";

    html += "<div id='toast' class='toast'></div>";
    html += "<script>";
    html += "function showToast(msg,ok){var t=document.getElementById('toast');if(!t)return;t.textContent=msg;t.className='toast '+(ok?'ok':'err')+' show';setTimeout(function(){t.className='toast';},2800);}";
    html += "function setTriggerStatus(msg){var s=document.getElementById('trigger-upload-status');if(s){s.textContent=msg||'';}}";
    html += "function triggerUpload(){var b=document.getElementById('trigger-upload-btn');if(!b)return;if(b.dataset.loading==='1')return;var label=b.innerHTML;b.dataset.loading='1';b.classList.add('btn-loading');b.innerHTML='&#8987; Triggering...';setTriggerStatus('Sending upload request...');showToast('Sending upload request...',true);fetch('/trigger-upload',{method:'GET',cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.text();}).then(function(raw){var d={};try{d=JSON.parse(raw);}catch(e){}var m=(d&&d.message)?d.message:'Upload triggered.';showToast(m,true);setTriggerStatus('Request accepted.');}).catch(function(){showToast('Failed to trigger upload.',false);setTriggerStatus('Request failed. Try again.');}).finally(function(){setTimeout(function(){b.dataset.loading='0';b.classList.remove('btn-loading');b.innerHTML=label;},700);});}";
    html += "</script>";
    html += "</div></body></html>";
    sendChunk(html);
    
    // Terminate chunked transmission
    server->sendContent("");
}

// GET /trigger-upload - Force immediate upload
void TestWebServer::handleTriggerUpload() {
    LOG("[TestWebServer] Upload trigger requested via web interface");
    
    // Set global trigger flag
    g_triggerUploadFlag = true;
    
    // Add CORS headers
    addCorsHeaders(server);
    
    String response = "{\"status\":\"success\",\"message\":\"Upload triggered. Check serial output for progress.\"}";
    server->send(200, "application/json", response);
}

// GET /status - JSON status information (Legacy - Removed, use handleApiStatus)


// GET /reset-state - Clear upload state
void TestWebServer::handleResetState() {
    LOG("[TestWebServer] State reset requested via web interface");
    
    // Set global reset flag
    g_resetStateFlag = true;
    
    // Add CORS headers
    addCorsHeaders(server);
    
    String response = "{\"status\":\"success\",\"message\":\"Upload state will be reset. Check serial output for confirmation.\"}";
    server->send(200, "application/json", response);
}

// GET /api/config - Display current configuration
void TestWebServer::handleApiConfig() {
    // Add CORS headers
    addCorsHeaders(server);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    auto sendChunk = [this](const String& s) { server->sendContent(s); };
    
    String json = "{";
    
    if (config) {
        // Check if credentials are stored in secure mode
        bool credentialsSecured = config->areCredentialsInFlash();
        
        json += "\"wifi_ssid\":\"" + escapeJson(config->getWifiSSID()) + "\",";
        json += "\"wifi_password\":\"***HIDDEN***\",";
        json += "\"hostname\":\"" + escapeJson(config->getHostname()) + "\",";
        
        server->send(200, "application/json", json);
        
        json = "";
        json += "\"endpoint\":\"" + escapeJson(config->getEndpoint()) + "\",";
        json += "\"endpoint_type\":\"" + escapeJson(config->getEndpointType()) + "\",";
        json += "\"endpoint_user\":\"" + escapeJson(config->getEndpointUser()) + "\",";
        json += "\"endpoint_password\":\"***HIDDEN***\",";
        sendChunk(json);
        
        json = "";
        json += "\"gmt_offset_hours\":" + String(config->getGmtOffsetHours()) + ",";
        json += "\"upload_mode\":\"" + escapeJson(config->getUploadMode()) + "\",";
        json += "\"upload_start_hour\":" + String(config->getUploadStartHour()) + ",";
        json += "\"upload_end_hour\":" + String(config->getUploadEndHour()) + ",";
        json += "\"inactivity_seconds\":" + String(config->getInactivitySeconds()) + ",";
        json += "\"exclusive_access_minutes\":" + String(config->getExclusiveAccessMinutes()) + ",";
        json += "\"cooldown_minutes\":" + String(config->getCooldownMinutes()) + ",";
        sendChunk(json);
        
        // Cloud upload config
        json = "";
        if (config->hasCloudEndpoint()) {
            json += "\"cloud_client_id\":\"" + escapeJson(config->getCloudClientId()) + "\",";
            json += "\"cloud_client_secret\":\"***HIDDEN***\",";
            json += "\"cloud_device_id\":" + String(config->getCloudDeviceId()) + ",";
            json += "\"cloud_base_url\":\"" + escapeJson(config->getCloudBaseUrl()) + "\",";
            if (!config->getCloudTeamId().isEmpty()) {
                json += "\"cloud_team_id\":\"" + escapeJson(config->getCloudTeamId()) + "\",";
            }
            json += "\"cloud_insecure_tls\":" + String(config->getCloudInsecureTls() ? "true" : "false") + ",";
        }
        if (config->getMaxDays() > 0) {
            json += "\"max_days\":" + String(config->getMaxDays()) + ",";
        }
        // Add credentials_secured field to indicate storage mode
        json += "\"credentials_secured\":" + String(credentialsSecured ? "true" : "false");
    } else {
        // If config is null, send minimal JSON
        server->send(200, "application/json", json);
    }
    
    json = "}";
    sendChunk(json);
    server->sendContent("");
}

// Handle 404 errors
void TestWebServer::handleNotFound() {
    String uri = server->uri();
    
    // Silently handle common browser requests that we don't care about
    if (uri == "/favicon.ico" || uri == "/apple-touch-icon.png" || 
        uri == "/apple-touch-icon-precomposed.png" || uri == "/robots.txt") {
        server->send(404, "text/plain", "Not found");
        return;
    }
    
    // Log unexpected 404s
    LOG_DEBUGF("[TestWebServer] 404 Not Found: %s", uri.c_str());
    
    String message = "{\"status\":\"error\",\"message\":\"Endpoint not found\",\"path\":\"" + uri + "\"}";
    server->send(404, "application/json", message);
}

// Static helper: Add CORS headers to response
void TestWebServer::addCorsHeaders(WebServer* server) {
    server->sendHeader("Access-Control-Allow-Origin", "*");
    server->sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
    server->sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// Helper: Get uptime as formatted string
String TestWebServer::getUptimeString() {
    unsigned long seconds = millis() / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;
    
    String uptime = "";
    if (days > 0) {
        uptime += String(days) + "d ";
    }
    uptime += String(hours % 24) + "h ";
    uptime += String(minutes % 60) + "m ";
    uptime += String(seconds % 60) + "s";
    
    return uptime;
}

// Helper: Get current time as formatted string
String TestWebServer::getCurrentTimeString() {
    time_t now;
    time(&now);
    
    if (now < 1000000000) {
        return "Not synchronized";
    }
    
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    
    char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    return String(buffer);
}

// Helper: Get count of pending files (estimate)
int TestWebServer::getPendingFilesCount() {
    if (!stateManager) {
        return 0;
    }
    
    // Count files in incomplete DATALOG folders
    // Note: This requires SD card access, which we don't have here
    // Return -1 to indicate "unknown" rather than misleading 0
    return -1;
}

// Helper: Get count of pending DATALOG folders
int TestWebServer::getPendingFoldersCount() {
    if (!stateManager) {
        return 0;
    }
    
    return stateManager->getPendingFoldersCount();
}

// Helper class to adapt WebServer for chunked streaming via Print interface
class ChunkedPrint : public Print {
    WebServer* _server;
public:
    ChunkedPrint(WebServer* server) : _server(server) {}
    
    size_t write(uint8_t c) override {
        return write(&c, 1);
    }
    
    size_t write(const uint8_t *buffer, size_t size) override {
        if (size == 0) return 0;
        
        // Manually write chunk header (size in hex + CRLF)
        WiFiClient client = _server->client();
        client.print(String(size, HEX));
        client.write("\r\n", 2);
        
        // Write chunk data
        size_t written = client.write(buffer, size);
        
        // Write chunk footer (CRLF)
        client.write("\r\n", 2);
        
        return written;
    }
};

// GET /logs - Retrieve system logs from circular buffer
void TestWebServer::handleLogs() {
    // Add CORS headers for cross-origin access
    addCorsHeaders(server);
    
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    
    auto sendChunk = [this](const String& s) {
        server->sendContent(s);
    };

    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='utf-8'>";
    html += "<title>System Logs - CPAP Data Uploader</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "*{box-sizing:border-box;margin:0;padding:0}";
    html += "body{font-family:Consolas,Monaco,'Andale Mono','Ubuntu Mono',monospace;background:#0f1923;color:#c7d5e0;padding:20px;font-size:13px;line-height:1.4;height:100vh;display:flex;flex-direction:column}";
    html += ".header{display:flex;align-items:center;gap:20px;margin-bottom:15px;flex-shrink:0}";
    html += "h1{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;color:#fff;font-size:1.4em;margin:0}";
    html += ".log-container{background:#1b2838;border:1px solid #2a475e;border-radius:6px;padding:15px;white-space:pre-wrap;word-wrap:break-word;overflow-x:hidden;overflow-y:auto;flex-grow:1;font-size:13px}";
    html += ".btn{display:inline-flex;align-items:center;gap:6px;padding:8px 16px;border-radius:6px;background:#2a475e;color:#c7d5e0;text-decoration:none;font-family:-apple-system,sans-serif;font-size:0.9em;border:none;cursor:pointer;transition:background 0.2s}";
    html += ".btn:hover{background:#3a5a7e}";
    html += ".status{margin-left:auto;font-family:-apple-system,sans-serif;color:#8f98a0;font-size:0.9em}";
    html += "</style></head><body>";
    
    html += "<div class='header'>";
    html += "<a href='/' class='btn'>&larr; Dashboard</a>";
    html += "<h1>System Logs</h1>";
    html += "<span id='status' class='status'>Connecting...</span>";
    html += "</div>";
    
    html += "<div id='logs' class='log-container'>Loading...</div>";
    
    html += "<script>";
    html += "const logDiv = document.getElementById('logs');";
    html += "const statusSpan = document.getElementById('status');";
    html += "let isScrolledToBottom = true;";
    html += "logDiv.addEventListener('scroll', () => {";
    html += "  isScrolledToBottom = (logDiv.scrollHeight - logDiv.scrollTop - logDiv.clientHeight) < 50;";
    html += "});";
    html += "function fetchLogs() {";
    html += "  fetch('/api/logs').then(r => r.text()).then(text => {";
    html += "    if (logDiv.innerText !== text) {";
    html += "      logDiv.innerText = text;";
    html += "      if (isScrolledToBottom) logDiv.scrollTop = logDiv.scrollHeight;";
    html += "      statusSpan.textContent = 'Live';";
    html += "      statusSpan.style.color = '#44ff44';";
    html += "    }";
    html += "  }).catch(e => {";
    html += "    statusSpan.textContent = 'Disconnected';";
    html += "    statusSpan.style.color = '#ff4444';";
    html += "  });";
    html += "}";
    html += "fetchLogs();";
    html += "setInterval(fetchLogs, 2000);"; // Refresh every 2s
    html += "</script>";
    
    html += "</body></html>";
    
    server->send(200, "text/html; charset=utf-8", html);
    server->sendContent("");
}

// GET /api/logs - Raw logs for AJAX
void TestWebServer::handleApiLogs() {
    // Add CORS headers
    addCorsHeaders(server);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    // Send headers with correct content type
    server->send(200, "text/plain; charset=utf-8", "");
    // Stream logs directly
    ChunkedPrint chunkedOutput(server);
    Logger::getInstance().printLogs(chunkedOutput);
    server->sendContent("");
}

// GET /config - Display current configuration (HTML View)
void TestWebServer::handleConfigPage() {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    auto sendChunk = [this](const String& s) { server->sendContent(s); };

    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Configuration - CPAP Data Uploader</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "*{box-sizing:border-box;margin:0;padding:0}";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;";
    html += "background:#0f1923;color:#c7d5e0;min-height:100vh;padding:20px}";
    html += ".wrap{max-width:800px;margin:0 auto}";
    html += "h1{font-size:1.6em;color:#fff;margin-bottom:20px}";
    html += ".json-box{background:#1b2838;border:1px solid #2a475e;border-radius:10px;padding:20px;font-family:monospace;white-space:pre-wrap;color:#aaddff;overflow-x:auto}";
    html += ".btn{display:inline-flex;align-items:center;padding:10px 18px;border-radius:6px;background:#2a475e;color:#c7d5e0;text-decoration:none;font-weight:600;border:none;cursor:pointer;transition:background 0.2s}";
    html += ".btn:hover{background:#3a5a7e}";
    html += "</style></head><body><div class='wrap'>";
    
    server->send(200, "text/html; charset=utf-8", html);
    
    html = "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:20px'>";
    html += "<h1>Configuration (JSON)</h1>";
    html += "<a href='/' class='btn'>&larr; Dashboard</a>";
    html += "</div>";
    
    html += "<div id='json' class='json-box'>Loading...</div>";
    
    html += "<script>";
    html += "fetch('/api/config').then(r=>r.json()).then(d=>{";
    html += "document.getElementById('json').textContent = JSON.stringify(d, null, 2);";
    html += "});";
    html += "</script>";

    html += "</div></body></html>";
    sendChunk(html);
    server->sendContent("");
}

// GET /status - Display system status (HTML View)
void TestWebServer::handleStatusPage() {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    auto sendChunk = [this](const String& s) { server->sendContent(s); };

    String html = "<!DOCTYPE html><html><head>";
    html += "<title>System Status - CPAP Data Uploader</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "*{box-sizing:border-box;margin:0;padding:0}";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;";
    html += "background:#0f1923;color:#c7d5e0;min-height:100vh;padding:20px}";
    html += ".wrap{max-width:800px;margin:0 auto}";
    html += "h1{font-size:1.6em;color:#fff;margin-bottom:20px}";
    html += ".json-box{background:#1b2838;border:1px solid #2a475e;border-radius:10px;padding:20px;font-family:monospace;white-space:pre-wrap;color:#aaddff;overflow-x:auto}";
    html += ".btn{display:inline-flex;align-items:center;padding:10px 18px;border-radius:6px;background:#2a475e;color:#c7d5e0;text-decoration:none;font-weight:600;border:none;cursor:pointer;transition:background 0.2s}";
    html += ".btn:hover{background:#3a5a7e}";
    html += "</style></head><body><div class='wrap'>";

    server->send(200, "text/html; charset=utf-8", html);

    html = "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:20px'>";
    html += "<h1>System Status (JSON)</h1>";
    html += "<a href='/' class='btn'>&larr; Dashboard</a>";
    html += "</div>";

    html += "<div id='json' class='json-box'>Loading...</div>";

    html += "<script>";
    html += "fetch('/api/status').then(r=>r.json()).then(d=>{";
    html += "document.getElementById('json').textContent = JSON.stringify(d, null, 2);";
    html += "});";
    html += "</script>";

    html += "</div></body></html>";
    sendChunk(html);
    server->sendContent("");
}

// GET /api/status - JSON status information
void TestWebServer::handleApiStatus() {
    // Add CORS headers
    addCorsHeaders(server);
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    // Send headers with correct content type
    server->send(200, "application/json", "");
    
    auto sendChunk = [this](const String& s) { server->sendContent(s); };
    
    String json = "{";
    json += "\"uptime_seconds\":" + String(millis() / 1000) + ",";
    json += "\"current_time\":\"" + getCurrentTimeString() + "\",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    sendChunk(json);
    
    json = "";
    if (wifiManager && wifiManager->isConnected()) {
        json += "\"wifi_connected\":true,";
        json += "\"wifi_rssi\":" + String(wifiManager->getSignalStrength()) + ",";
        json += "\"wifi_quality\":\"" + wifiManager->getSignalQuality() + "\",";
    } else {
        json += "\"wifi_connected\":false,";
    }
    
    if (scheduleManager) {
        json += "\"next_upload_seconds\":" + String(scheduleManager->getSecondsUntilNextUpload()) + ",";
        json += "\"time_synced\":" + String(scheduleManager->isTimeSynced() ? "true" : "false") + ",";
    }
    sendChunk(json);
    
    json = "";
    if (stateManager) {
        int completed = stateManager->getCompletedFoldersCount();
        int incomplete = stateManager->getIncompleteFoldersCount();
        int pending = stateManager->getPendingFoldersCount();
        json += "\"completed_folders\":" + String(completed) + ",";
        json += "\"incomplete_folders\":" + String(incomplete) + ",";
        json += "\"pending_data_folders\":" + String(incomplete) + ",";
        json += "\"pending_folders\":" + String(pending) + ",";
        json += "\"total_folders\":" + String(completed + incomplete) + ",";
        json += "\"upload_state_initialized\":" + String((completed + incomplete + pending) > 0 ? "true" : "false");
    } else {
        json += "\"completed_folders\":0,\"incomplete_folders\":0,\"total_folders\":0,\"upload_state_initialized\":false";
    }
    sendChunk(json);

    json = "";
    if (config) {
        json += ",\"endpoint_type\":\"" + config->getEndpointType() + "\"";
        json += ",\"boot_delay_seconds\":" + String(config->getBootDelaySeconds());
        json += ",\"upload_mode\":\"" + config->getUploadMode() + "\"";
        json += ",\"cloud_configured\":" + String(config->hasCloudEndpoint() ? "true" : "false");
    }
    sendChunk(json);

    json = "}";
    sendChunk(json);
    server->sendContent("");
}

// Update manager references (needed after uploader recreation)
void TestWebServer::updateManagers(UploadStateManager* state, ScheduleManager* schedule) {
    stateManager = state;
    scheduleManager = schedule;
}

// Set WiFi manager reference
void TestWebServer::setWiFiManager(WiFiManager* wifi) {
    wifiManager = wifi;
}

// Set TrafficMonitor reference
void TestWebServer::setTrafficMonitor(TrafficMonitor* tm) {
    trafficMonitor = tm;
}

// Helper: Escape special characters for JSON string
String TestWebServer::escapeJson(const String& str) {
    String escaped = "";
    escaped.reserve(str.length() + 20);  // Reserve extra space for escape sequences
    
    for (size_t i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        switch (c) {
            case '"':
                escaped += "\\\"";
                break;
            case '\\':
                escaped += "\\\\";
                break;
            case '\b':
                escaped += "\\b";
                break;
            case '\f':
                escaped += "\\f";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                // Handle control characters (0x00-0x1F)
                if (c >= 0 && c < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    escaped += buf;
                } else {
                    escaped += c;
                }
                break;
        }
    }
    
    return escaped;
}

#ifdef ENABLE_OTA_UPDATES
// Set OTA manager reference
void TestWebServer::setOTAManager(OTAManager* ota) {
    otaManager = ota;
}

// GET /ota - OTA update page
void TestWebServer::handleOTAPage() {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);

    auto sendChunk = [this](const String& s) {
        server->sendContent(s);
    };

    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Firmware Update - CPAP Auto-Uploader</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "*{box-sizing:border-box;margin:0;padding:0}";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;";
    html += "background:#0f1923;color:#c7d5e0;min-height:100vh;padding:20px}";
    html += ".wrap{max-width:900px;margin:0 auto}";
    html += "h1{font-size:1.6em;color:#fff;margin-bottom:4px}";
    html += ".subtitle{color:#66c0f4;font-size:0.9em;margin-bottom:20px}";
    html += ".cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:16px;margin-bottom:20px}";
    html += ".card{background:#1b2838;border:1px solid #2a475e;border-radius:10px;padding:18px}";
    html += ".card h2{font-size:0.85em;text-transform:uppercase;letter-spacing:1px;color:#66c0f4;margin-bottom:12px;border-bottom:1px solid #2a475e;padding-bottom:8px}";
    html += ".row{display:flex;justify-content:space-between;padding:5px 0;font-size:0.88em}";
    html += ".row .k{color:#8f98a0}.row .v{color:#c7d5e0;font-weight:500;text-align:right}";
    html += ".btn{display:inline-flex;align-items:center;justify-content:center;gap:6px;padding:10px 18px;border-radius:6px;font-size:0.85em;font-weight:600;text-decoration:none;border:none;cursor:pointer;transition:all 0.2s;width:100%}";
    html += ".btn-primary{background:#66c0f4;color:#0f1923}.btn-primary:hover{background:#88d0ff}";
    html += ".btn-secondary{background:#2a475e;color:#c7d5e0;width:auto}.btn-secondary:hover{background:#3a5a7e}";
    html += ".btn-danger{background:#c0392b;color:#fff}.btn-danger:hover{background:#e04030}";
    html += ".alert{background:#3a2a1a;border:1px solid #aa6622;border-radius:8px;padding:14px;margin-bottom:16px}";
    html += ".alert h3{color:#ffaa44;font-size:0.9em;margin-bottom:6px}";
    html += ".alert ul{padding-left:20px;color:#c7d5e0;font-size:0.85em}";
    html += ".alert li{margin-bottom:4px}";
    html += ".form-group{margin-bottom:15px}";
    html += ".form-group label{display:block;margin-bottom:6px;color:#8f98a0;font-size:0.9em}";
    html += ".form-group input{width:100%;padding:10px;background:#0f1923;border:1px solid #2a475e;color:#fff;border-radius:6px;font-size:0.9em}";
    html += ".form-group input:focus{outline:none;border-color:#66c0f4}";
    html += ".status-msg{margin-top:10px;font-size:0.9em;min-height:1.2em}";
    html += ".status-msg.success{color:#44ff44}";
    html += ".status-msg.error{color:#ff4444}";
    html += ".status-msg.info{color:#66c0f4}";
    html += ".actions{margin-top:20px}";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='wrap'>";
    
    server->send(200, "text/html; charset=utf-8", html);

    html = "<h1>Firmware Update</h1>";
    html += "<p class='subtitle'>System Maintenance</p>";
    sendChunk(html);
    
    html = "<div class='cards'>";
    
    // Current version info
    html += "<div class='card'>";
    html += "<h2>Current Status</h2>";
    if (otaManager) {
        html += "<div class='row'><span class='k'>Current Version</span><span class='v'>" + otaManager->getCurrentVersion() + "</span></div>";
    } else {
        html += "<div class='row'><span class='k'>Current Version</span><span class='v'>Unknown</span></div>";
    }
    html += "</div>";
    
    // Warning message
    html += "<div class='card' style='grid-column:1/-1'>";
    html += "<h2>Important Safety Information</h2>";
    html += "<div class='alert'>";
    html += "<h3>WARNING</h3>";
    html += "<ul>";
    html += "<li><strong>Do not power off</strong> the device during update</li>";
    html += "<li><strong>Ensure stable WiFi</strong> connection before starting</li>";
    html += "<li><strong>Do NOT remove SD card</strong> from CPAP machine during update</li>";
    html += "<li>Update process takes 1-2 minutes</li>";
    html += "<li>Device will restart automatically when complete</li>";
    html += "</ul>";
    html += "</div>";
    html += "</div>";
    
    // Check if update is in progress
    if (otaManager && otaManager->isUpdateInProgress()) {
        html += "<div class='card' style='grid-column:1/-1'>";
        html += "<h2>Update In Progress</h2>";
        html += "<p style='color:#ffaa44'>A firmware update is currently in progress. Please wait for it to complete.</p>";
        html += "</div>";
    } else {
        // File upload method
        html += "<div class='card'>";
        html += "<h2>Method 1: File Upload</h2>";
        html += "<form id='uploadForm' enctype='multipart/form-data'>";
        html += "<div class='form-group'>";
        html += "<label for='firmwareFile'>Select firmware file (.bin):</label>";
        html += "<input type='file' id='firmwareFile' name='firmware' accept='.bin' required>";
        html += "</div>";
        html += "<button type='submit' class='btn btn-primary'>Upload & Install</button>";
        html += "<div id='uploadStatus' class='status-msg'></div>";
        html += "</form>";
        html += "</div>";
        
        // URL download method
        html += "<div class='card'>";
        html += "<h2>Method 2: URL Download</h2>";
        html += "<form id='urlForm'>";
        html += "<div class='form-group'>";
        html += "<label for='firmwareURL'>Firmware URL:</label>";
        html += "<input type='url' id='firmwareURL' name='url' placeholder='https://github.com/.../firmware.bin' required>";
        html += "</div>";
        html += "<button type='submit' class='btn btn-primary'>Download & Install</button>";
        html += "<div id='downloadStatus' class='status-msg'></div>";
        html += "</form>";
        html += "</div>";
    }
    
    html += "</div>"; // end cards
    
    html += "<div class='actions'>";
    html += "<a href='/' class='btn btn-secondary'>&larr; Back to Status</a>";
    html += "</div>";
    
    // JavaScript for handling uploads
    html += "<script>";
    html += "let updateInProgress = false;";
    
    // File upload handler
    html += "document.getElementById('uploadForm')?.addEventListener('submit', function(e) {";
    html += "  e.preventDefault();";
    html += "  if (updateInProgress) return;";
    html += "  const fileInput = document.getElementById('firmwareFile');";
    html += "  if (!fileInput.files[0]) { alert('Please select a file'); return; }";
    html += "  uploadFirmware(fileInput.files[0]);";
    html += "});";
    
    // URL download handler
    html += "document.getElementById('urlForm')?.addEventListener('submit', function(e) {";
    html += "  e.preventDefault();";
    html += "  if (updateInProgress) return;";
    html += "  const url = document.getElementById('firmwareURL').value;";
    html += "  if (!url) { alert('Please enter a URL'); return; }";
    html += "  downloadFirmware(url);";
    html += "});";
    
    // Upload function
    html += "function uploadFirmware(file) {";
    html += "  updateInProgress = true;";
    html += "  setStatus('uploadStatus', 'info', 'Uploading firmware... 0%');";
    html += "  const formData = new FormData();";
    html += "  formData.append('firmware', file);";
    html += "  const xhr = new XMLHttpRequest();";
    html += "  xhr.upload.addEventListener('progress', function(e) {";
    html += "    if (e.lengthComputable) {";
    html += "      const percent = Math.round((e.loaded / e.total) * 100);";
    html += "      setStatus('uploadStatus', 'info', 'Uploading firmware... ' + percent + '%');";
    html += "    }";
    html += "  });";
    html += "  xhr.addEventListener('load', function() {";
    html += "    try {";
    html += "      const data = JSON.parse(xhr.responseText);";
    html += "      handleResult(data, 'uploadStatus');";
    html += "    } catch(e) { handleResult({success:false, message:'Invalid response'}, 'uploadStatus'); }";
    html += "  });";
    html += "  xhr.addEventListener('error', function() { handleResult({success:false, message:'Network error'}, 'uploadStatus'); });";
    html += "  xhr.open('POST', '/ota-upload');";
    html += "  xhr.send(formData);";
    html += "}";
    
    // Download function
    html += "function downloadFirmware(url) {";
    html += "  updateInProgress = true;";
    html += "  setStatus('downloadStatus', 'info', 'Downloading firmware... (this may take a minute)');";
    html += "  const formData = new FormData();";
    html += "  formData.append('url', url);";
    html += "  fetch('/ota-url', { method: 'POST', body: formData })";
    html += "    .then(response => response.json())";
    html += "    .then(data => handleResult(data, 'downloadStatus'))";
    html += "    .catch(error => handleResult({success:false, message:String(error)}, 'downloadStatus'));";
    html += "}";
    
    // Result handlers
    html += "function setStatus(id, type, msg) {";
    html += "  const el = document.getElementById(id);";
    html += "  if(el) { el.className = 'status-msg ' + type; el.textContent = msg; }";
    html += "}";
    
    html += "function handleResult(data, statusId) {";
    html += "  updateInProgress = false;";
    html += "  if (data.success) {";
    html += "    setStatus(statusId, 'success', 'Success! ' + data.message);";
    html += "    let timeLeft = 30;";
    html += "    setInterval(() => {";
    html += "      timeLeft--;";
    html += "      if (timeLeft <= 0) window.location.href = '/';";
    html += "      else setStatus(statusId, 'success', 'Success! Redirecting to dashboard in ' + timeLeft + 's...');";
    html += "    }, 1000);";
    html += "  } else {";
    html += "    setStatus(statusId, 'error', 'Failed: ' + data.message);";
    html += "  }";
    html += "}";
    
    html += "</script>";
    html += "</div></body></html>";
    sendChunk(html);
    
    server->sendContent("");
}

// POST /ota-upload - Handle firmware file upload
void TestWebServer::handleOTAUpload() {
    static bool uploadError = false;
    static bool successResponseSent = false;
    static unsigned long lastUploadAttempt = 0;
    
    LOG_DEBUG("[OTA] handleOTAUpload() called");
    
    if (!otaManager) {
        LOG_ERROR("[OTA] OTA manager not initialized");
        server->send(500, "application/json", "{\"success\":false,\"message\":\"OTA manager not initialized\"}");
        return;
    }
    
    HTTPUpload& upload = server->upload();
    LOG_DEBUGF("[OTA] Upload status: %d", upload.status);
    
    if (upload.status == UPLOAD_FILE_START) {
        LOG_DEBUGF("[OTA] UPLOAD_FILE_START - filename: %s, totalSize: %u", upload.filename.c_str(), upload.totalSize);
        
        // Only check for "already in progress" during START phase
        if (otaManager->isUpdateInProgress()) {
            LOG_ERROR("[OTA] Update already in progress");
            server->send(400, "application/json", "{\"success\":false,\"message\":\"Update already in progress\"}");
            return;
        }
        
        // Prevent rapid repeated calls (debounce)
        unsigned long now = millis();
        if (now - lastUploadAttempt < 1000) {  // 1 second debounce
            LOG_WARN("[OTA] Upload attempt too soon, ignoring");
            server->send(429, "application/json", "{\"success\":false,\"message\":\"Too many requests\"}");
            return;
        }
        lastUploadAttempt = now;
        
        uploadError = false;
        successResponseSent = false;
        
        if (upload.totalSize == 0) {
            LOG_WARN("[OTA] Total size is 0, using chunked upload mode");
        }
        
        if (!otaManager->startUpdate(upload.totalSize)) {
            LOG_ERROR("[OTA] Failed to start update");
            uploadError = true;
            return;
        }
        
        LOG_DEBUG("[OTA] Update started successfully");
        
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        LOG_DEBUGF("[OTA] UPLOAD_FILE_WRITE - currentSize: %u, uploadError: %s", 
                   upload.currentSize, uploadError ? "true" : "false");
        
        if (!uploadError && !otaManager->writeChunk(upload.buf, upload.currentSize)) {
            LOG_ERROR("[OTA] Failed to write chunk");
            uploadError = true;
            return;
        }
        
    } else if (upload.status == UPLOAD_FILE_END) {
        LOG_DEBUGF("[OTA] UPLOAD_FILE_END - totalSize: %u, uploadError: %s", 
                   upload.totalSize, uploadError ? "true" : "false");
        
        if (uploadError) {
            LOG_ERROR("[OTA] Upload failed due to previous errors");
            otaManager->abortUpdate();
            // Don't send response here, handleOTAUploadComplete will handle it
            return;
        }
        
        if (otaManager->finishUpdate()) {
            LOG("[OTA] Update completed successfully, restarting...");
            // Send success response before restarting
            server->send(200, "application/json", "{\"success\":true,\"message\":\"Update completed! Device will restart in 3 seconds.\"}");
            successResponseSent = true;
            
            // Restart after a short delay
            delay(3000);
            ESP.restart();
        } else {
            LOG_ERROR("[OTA] Failed to finish update");
            // Don't send response here, handleOTAUploadComplete will handle it
        }
        
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        LOG_WARN("[OTA] UPLOAD_FILE_ABORTED");
        otaManager->abortUpdate();
        uploadError = true;
        // Don't send response here, handleOTAUploadComplete will handle it
    } else {
        LOG_DEBUGF("[OTA] Unknown upload status: %d", upload.status);
    }
}

// POST /ota-upload - Handle completion of firmware file upload
void TestWebServer::handleOTAUploadComplete() {
    LOG_DEBUG("[OTA] handleOTAUploadComplete() called");
    
    if (!otaManager) {
        LOG_ERROR("[OTA] OTA manager not initialized");
        server->send(500, "application/json", "{\"success\":false,\"message\":\"OTA manager not initialized\"}");
        return;
    }
    
    // This is called after the upload is complete
    // The actual upload processing happens in handleOTAUpload()
    // Here we just send the final response if one hasn't been sent already
    
    // Check if there was an error during upload
    String error = otaManager->getLastError();
    if (!error.isEmpty()) {
        LOG_ERROR("[OTA] Upload completed with error");
        server->send(500, "application/json", "{\"success\":false,\"message\":\"Upload failed: " + error + "\"}");
    } else {
        // If we reach here and there's no error, it means the update was successful
        // but the device hasn't restarted yet (which shouldn't normally happen)
        LOG_DEBUG("[OTA] Upload completed successfully");
        server->send(200, "application/json", "{\"success\":true,\"message\":\"Upload completed successfully! Device will restart shortly.\"}");
    }
}

// POST /ota-url - Handle firmware download from URL
void TestWebServer::handleOTAURL() {
    if (!otaManager) {
        server->send(500, "application/json", "{\"success\":false,\"message\":\"OTA manager not initialized\"}");
        return;
    }
    
    if (otaManager->isUpdateInProgress()) {
        server->send(400, "application/json", "{\"success\":false,\"message\":\"Update already in progress\"}");
        return;
    }
    
    if (!server->hasArg("url")) {
        server->send(400, "application/json", "{\"success\":false,\"message\":\"URL parameter required\"}");
        return;
    }
    
    String url = server->arg("url");
    LOG_DEBUGF("[OTA] Starting download from URL: %s", url.c_str());
    
    if (otaManager->updateFromURL(url)) {
        LOG("[OTA] Update completed successfully, restarting...");
        server->send(200, "application/json", "{\"success\":true,\"message\":\"Update completed! Device will restart in 3 seconds.\"}");
        
        // Restart after a short delay
        delay(3000);
        ESP.restart();
    } else {
        LOG_ERROR("[OTA] Failed to update from URL");
        String error = otaManager->getLastError();
        server->send(500, "application/json", "{\"success\":false,\"message\":\"Update failed: " + error + "\"}");
    }
}

#endif // ENABLE_OTA_UPDATES

// ============================================================================
// SD Activity Monitor Handlers
// ============================================================================

void TestWebServer::handleMonitorStart() {
    addCorsHeaders(server);
    g_monitorActivityFlag = true;
    server->send(200, "application/json", "{\"success\":true,\"message\":\"Monitoring started\"}");
}

void TestWebServer::handleMonitorStop() {
    addCorsHeaders(server);
    g_stopMonitorFlag = true;
    server->send(200, "application/json", "{\"success\":true,\"message\":\"Monitoring stopped\"}");
}

void TestWebServer::handleSdActivity() {
    addCorsHeaders(server);
    
    if (!trafficMonitor) {
        server->send(500, "application/json", "{\"error\":\"TrafficMonitor not available\"}");
        return;
    }
    
    // Build JSON with current stats and recent samples
    String json = "{";
    json += "\"last_pulse_count\":" + String(trafficMonitor->getLastPulseCount()) + ",";
    json += "\"consecutive_idle_ms\":" + String(trafficMonitor->getConsecutiveIdleMs()) + ",";
    json += "\"longest_idle_ms\":" + String(trafficMonitor->getLongestIdleMs()) + ",";
    json += "\"total_active_samples\":" + String(trafficMonitor->getTotalActiveSamples()) + ",";
    json += "\"total_idle_samples\":" + String(trafficMonitor->getTotalIdleSamples()) + ",";
    json += "\"is_busy\":" + String(trafficMonitor->isBusy() ? "true" : "false") + ",";
    json += "\"sample_count\":" + String(trafficMonitor->getSampleCount()) + ",";
    
    // Include the last N samples from the circular buffer
    json += "\"samples\":[";
    int count = trafficMonitor->getSampleCount();
    int head = trafficMonitor->getSampleHead();
    int maxSamples = TrafficMonitor::MAX_SAMPLES;
    const ActivitySample* buffer = trafficMonitor->getSampleBuffer();
    
    // Output samples oldest-first (from tail to head)
    int start = (count < maxSamples) ? 0 : head;
    int outputCount = min(count, 60);  // Limit to last 60 seconds for web response
    int skip = count - outputCount;
    
    bool first = true;
    for (int i = 0; i < count; i++) {
        if (i < skip) continue;
        int idx = (start + i) % maxSamples;
        if (!first) json += ",";
        json += "{\"t\":" + String(buffer[idx].timestamp);
        json += ",\"p\":" + String(buffer[idx].pulseCount);
        json += ",\"a\":" + String(buffer[idx].active ? 1 : 0) + "}";
        first = false;
    }
    
    json += "]}";
    
    server->send(200, "application/json", json);
}

void TestWebServer::handleMonitorPage() {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    
    auto sendChunk = [this](const String& s) {
        server->sendContent(s);
    };

    String html = "<!DOCTYPE html><html><head>";
    html += "<title>SD Activity Monitor - CPAP Auto-Uploader</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "*{box-sizing:border-box;margin:0;padding:0}";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;";
    html += "background:#0f1923;color:#c7d5e0;min-height:100vh;padding:20px}";
    html += ".wrap{max-width:900px;margin:0 auto}";
    html += "h1{font-size:1.6em;color:#fff;margin-bottom:4px}";
    html += ".subtitle{color:#66c0f4;font-size:0.9em;margin-bottom:20px}";
    html += ".card{background:#1b2838;border:1px solid #2a475e;border-radius:10px;padding:18px;margin-bottom:16px}";
    html += ".card h2{font-size:0.85em;text-transform:uppercase;letter-spacing:1px;color:#66c0f4;margin-bottom:12px;border-bottom:1px solid #2a475e;padding-bottom:8px}";
    html += ".btn{display:inline-flex;align-items:center;gap:6px;padding:10px 18px;border-radius:6px;font-size:0.85em;font-weight:600;text-decoration:none;border:none;cursor:pointer;transition:all 0.2s}";
    html += ".btn-primary{background:#66c0f4;color:#0f1923}.btn-primary:hover{background:#88d0ff}";
    html += ".btn-danger{background:#c0392b;color:#fff}.btn-danger:hover{background:#e04030}";
    html += ".btn-secondary{background:#2a475e;color:#c7d5e0}.btn-secondary:hover{background:#3a5a7e}";
    
    // Monitor specific styles
    html += ".stats-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-bottom:20px}";
    html += ".stat-box{background:#16213e;padding:15px;border-radius:8px;text-align:center;border:1px solid #2a475e}";
    html += ".stat-val{font-size:1.8em;color:#66c0f4;display:block;margin:5px 0;font-family:monospace}";
    html += ".stat-lbl{color:#8f98a0;font-size:0.85em}";
    
    html += ".chart-container{background:#16213e;padding:15px;border-radius:8px;border:1px solid #2a475e;overflow-x:auto;min-height:200px}";
    html += ".bar-row{display:flex;align-items:center;height:18px;margin:2px 0;font-family:monospace;font-size:0.8em}";
    html += ".bar-lbl{width:50px;color:#8f98a0;text-align:right;padding-right:8px}";
    html += ".bar-track{flex-grow:1;background:#0f1923;height:100%;border-radius:2px;overflow:hidden}";
    html += ".bar-fill{height:100%;transition:width 0.3s}";
    html += ".bar-fill.active{background:#ff4444}";
    html += ".bar-fill.idle{background:#2a475e}";
    html += ".indicator{display:inline-block;width:12px;height:12px;border-radius:50%;margin-left:10px}";
    html += ".indicator.busy{background:#ff4444;box-shadow:0 0 8px #ff4444}";
    html += ".indicator.idle{background:#44ff44;box-shadow:0 0 8px #44ff44}";
    html += "</style></head><body><div class='wrap'>";
    
    server->send(200, "text/html; charset=utf-8", html);
    
    // Header
    html = "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:20px'>";
    html += "<div><h1>SD Activity Monitor <span id='indicator' class='indicator idle'></span></h1>";
    html += "<p class='subtitle'>Real-time Bus Traffic Analysis</p></div>";
    html += "<button onclick='stopAndExit()' class='btn btn-secondary'>&larr; Back to Status</button>";
    html += "</div>";
    sendChunk(html);
    
    // Explanation Card
    html = "<div class='card'>";
    html += "<h2>About This Tool</h2>";
    html += "<p style='font-size:0.9em;color:#c7d5e0;line-height:1.5'>This tool monitors electrical signals on the SD card bus to detect when the CPAP machine is writing data. <strong>Use this when the CPAP is turned on and the SD card is inserted.</strong></p>";
    html += "<p style='font-size:0.9em;color:#c7d5e0;margin-top:8px'>It helps identify the best 'Idle' times to configure for safe uploads. When the bus is 'Busy' (Red), the CPAP is writing. When 'Idle' (Green), it is safe to take control.</p>";
    html += "</div>";
    sendChunk(html);
    
    // Controls
    html = "<div class='card'>";
    html += "<h2>Controls</h2>";
    html += "<div style='display:flex;gap:10px'>";
    html += "<button id='btn-start' onclick='startMonitor()' class='btn btn-primary'>Start Monitoring</button>";
    html += "<button id='btn-stop' onclick='stopMonitor()' class='btn btn-danger' style='display:none'>Stop Monitoring</button>";
    html += "</div></div>";
    sendChunk(html);
    
    // Stats Grid
    html = "<div class='stats-grid'>";
    html += "<div class='stat-box'><span class='stat-lbl'>Pulse Count (1s)</span><span class='stat-val' id='pulses'>--</span></div>";
    html += "<div class='stat-box'><span class='stat-lbl'>Consecutive Idle</span><span class='stat-val' id='idle'>--</span></div>";
    html += "<div class='stat-box'><span class='stat-lbl'>Longest Idle</span><span class='stat-val' id='longest'>--</span></div>";
    html += "<div class='stat-box'><span class='stat-lbl'>Active/Idle Ratio</span><span class='stat-val' id='ratio'>--</span></div>";
    html += "</div>";
    sendChunk(html);
    
    // Chart
    html = "<div class='card'>";
    html += "<h2>Activity Timeline (Last 60s)</h2>";
    html += "<div class='chart-container' id='chart'><em>Waiting for data...</em></div>";
    html += "</div>";
    sendChunk(html);
    
    // Scripts
    html = "<script>";
    html += "let polling=null;";
    html += "function stopAndExit(){stopMonitor(); setTimeout(()=>{window.location.href='/';}, 500);}";
    html += "function startMonitor(){fetch('/api/monitor-start').then(()=>{";
    html += "  document.getElementById('btn-start').style.display='none';";
    html += "  document.getElementById('btn-stop').style.display='inline-flex';";
    html += "  if(!polling) polling=setInterval(fetchData,1000);";
    html += "});}";
    html += "function stopMonitor(){fetch('/api/monitor-stop');";
    html += "  document.getElementById('btn-start').style.display='inline-flex';";
    html += "  document.getElementById('btn-stop').style.display='none';";
    html += "  if(polling){clearInterval(polling);polling=null;}";
    html += "}";
    html += "function fetchData(){fetch('/api/sd-activity').then(r=>r.json()).then(d=>{";
    html += "  document.getElementById('pulses').textContent=d.last_pulse_count;";
    html += "  document.getElementById('idle').textContent=(d.consecutive_idle_ms/1000).toFixed(1)+'s';";
    html += "  document.getElementById('longest').textContent=(d.longest_idle_ms/1000).toFixed(1)+'s';";
    html += "  document.getElementById('ratio').textContent=d.total_active_samples+' / '+d.total_idle_samples;";
    html += "  let ind=document.getElementById('indicator');";
    html += "  ind.className='indicator '+(d.is_busy?'busy':'idle');";
    html += "  let chart=document.getElementById('chart');";
    html += "  if(d.samples&&d.samples.length>0){";
    html += "    let html='';";
    html += "    d.samples.forEach(s=>{";
    html += "      let w=Math.min(Math.max(s.p/10,1),100);";
    html += "      let cls=s.a?'active':'idle';";
    html += "      let sec=s.t%3600;let m=Math.floor(sec/60);let ss=sec%60;";
    html += "      let lbl=String(m).padStart(2,'0')+':'+String(ss).padStart(2,'0');";
    html += "      html+='<div class=\"bar-row\"><span class=\"bar-lbl\">'+lbl+'</span>';";
    html += "      html+='<div class=\"bar-track\"><div class=\"bar-fill '+cls+'\" style=\"width:'+w+'%\"></div></div>';";
    html += "      html+='</div>';";
    html += "    });";
    html += "    chart.innerHTML=html;";
    html += "  }";
    html += "}).catch(e=>console.error('Fetch error:',e));}";
    html += "startMonitor();"; // Auto-start
    html += "</script>";
    
    html += "</div></body></html>";
    sendChunk(html);
    
    server->sendContent("");
}