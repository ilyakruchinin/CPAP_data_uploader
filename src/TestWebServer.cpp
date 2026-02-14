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
    server->on("/status", [this]() { this->handleStatus(); });
    server->on("/reset-state", [this]() { this->handleResetState(); });
    server->on("/config", [this]() { this->handleConfig(); });
    server->on("/logs", [this]() { this->handleLogs(); });
    server->on("/monitor", [this]() { this->handleMonitorPage(); });
    server->on("/api/monitor-start", [this]() { this->handleMonitorStart(); });
    server->on("/api/monitor-stop", [this]() { this->handleMonitorStop(); });
    server->on("/api/sd-activity", [this]() { this->handleSdActivity(); });
    
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
    server->send(200, "text/html", html);

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
    html += "<a href='/status' class='btn btn-secondary' target='_blank' rel='noopener noreferrer'>JSON Status</a>";
    html += "<a href='/config' class='btn btn-secondary' target='_blank' rel='noopener noreferrer'>Config</a>";
    html += "<a href='/logs' class='btn btn-secondary' target='_blank' rel='noopener noreferrer'>Logs</a>";
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

// GET /status - JSON status information
void TestWebServer::handleStatus() {
    // Add CORS headers
    addCorsHeaders(server);
    
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    
    auto sendChunk = [this](const String& s) {
        server->sendContent(s);
    };
    
    String json = "{";
    json += "\"uptime_seconds\":" + String(millis() / 1000) + ",";
    json += "\"current_time\":\"" + getCurrentTimeString() + "\",";
    json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    
    // Send initial chunk with headers
    server->send(200, "application/json", json);
    
    json = "";
    // WiFi information
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
    // Upload progress
    if (stateManager) {
        int completedFolders = stateManager->getCompletedFoldersCount();
        int incompleteFolders = stateManager->getIncompleteFoldersCount();
        int pendingFolders = stateManager->getPendingFoldersCount();
        int totalFolders = completedFolders + incompleteFolders;
        
        json += "\"completed_folders\":" + String(completedFolders) + ",";
        json += "\"incomplete_folders\":" + String(incompleteFolders) + ",";
        json += "\"pending_data_folders\":" + String(incompleteFolders) + ",";
        json += "\"pending_folders\":" + String(pendingFolders) + ",";
        json += "\"total_folders\":" + String(totalFolders) + ",";
        json += "\"upload_state_initialized\":" + String((totalFolders + pendingFolders) > 0 ? "true" : "false");
    } else {
        json += "\"completed_folders\":0,";
        json += "\"incomplete_folders\":0,";
        json += "\"pending_data_folders\":0,";
        json += "\"pending_folders\":0,";
        json += "\"total_folders\":0,";
        json += "\"upload_state_initialized\":false";
    }
    sendChunk(json);
    
    json = "";
    if (config) {
        json += ",\"endpoint_type\":\"" + config->getEndpointType() + "\"";
        json += ",\"boot_delay_seconds\":" + String(config->getBootDelaySeconds());
        json += ",\"upload_mode\":\"" + config->getUploadMode() + "\"";
        json += ",\"upload_start_hour\":" + String(config->getUploadStartHour());
        json += ",\"upload_end_hour\":" + String(config->getUploadEndHour());
        json += ",\"inactivity_seconds\":" + String(config->getInactivitySeconds());
        json += ",\"exclusive_access_minutes\":" + String(config->getExclusiveAccessMinutes());
        json += ",\"cooldown_minutes\":" + String(config->getCooldownMinutes());
        
        // Cloud upload status
        if (config->hasCloudEndpoint()) {
            json += ",\"cloud_configured\":true";
            if (config->getMaxDays() > 0) {
                json += ",\"max_days\":" + String(config->getMaxDays());
            }
            json += ",\"cloud_insecure_tls\":" + String(config->getCloudInsecureTls() ? "true" : "false");
        } else {
            json += ",\"cloud_configured\":false";
        }
    }
    sendChunk(json);
    
    json = "";
    // Add CPAP monitor data (without usage percentage)
    if (cpapMonitor) {
        json += ",\"cpap_monitor\":{";
        json += "\"interval_minutes\":10";
        json += ",\"data_points\":144";
        // Usage data might be large, so we handle it carefully if it was a large string
        // But getUsageDataJSON likely returns a string. 
        // Ideally we would stream that too, but for now just append.
        json += ",\"usage_data\":" + cpapMonitor->getUsageDataJSON();
        json += "}";
    }

    json += "}";
    sendChunk(json);
    
    // Terminate chunked transmission
    server->sendContent("");
}

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

// GET /config - Display current configuration
void TestWebServer::handleConfig() {
    // Add CORS headers
    addCorsHeaders(server);
    
    String json = "{";
    
    if (config) {
        // Check if credentials are stored in secure mode
        bool credentialsSecured = config->areCredentialsInFlash();
        
        json += "\"wifi_ssid\":\"" + config->getWifiSSID() + "\",";
        
        // Always censor WiFi password (never expose via HTTP)
        json += "\"wifi_password\":\"***HIDDEN***\",";
        
        json += "\"endpoint\":\"" + config->getEndpoint() + "\",";
        json += "\"endpoint_type\":\"" + config->getEndpointType() + "\",";
        json += "\"endpoint_user\":\"" + config->getEndpointUser() + "\",";
        
        // Always censor endpoint password (never expose via HTTP)
        json += "\"endpoint_password\":\"***HIDDEN***\",";
        
        json += "\"gmt_offset_hours\":" + String(config->getGmtOffsetHours()) + ",";
        json += "\"upload_mode\":\"" + config->getUploadMode() + "\",";
        json += "\"upload_start_hour\":" + String(config->getUploadStartHour()) + ",";
        json += "\"upload_end_hour\":" + String(config->getUploadEndHour()) + ",";
        json += "\"inactivity_seconds\":" + String(config->getInactivitySeconds()) + ",";
        json += "\"exclusive_access_minutes\":" + String(config->getExclusiveAccessMinutes()) + ",";
        json += "\"cooldown_minutes\":" + String(config->getCooldownMinutes()) + ",";
        
        // Cloud upload config
        if (config->hasCloudEndpoint()) {
            json += "\"cloud_client_id\":\"" + config->getCloudClientId() + "\",";
            // Always censor cloud client secret (never expose via HTTP)
            json += "\"cloud_client_secret\":\"***HIDDEN***\",";
            json += "\"cloud_device_id\":" + String(config->getCloudDeviceId()) + ",";
            json += "\"cloud_base_url\":\"" + config->getCloudBaseUrl() + "\",";
            if (!config->getCloudTeamId().isEmpty()) {
                json += "\"cloud_team_id\":\"" + config->getCloudTeamId() + "\",";
            }
            json += "\"cloud_insecure_tls\":" + String(config->getCloudInsecureTls() ? "true" : "false") + ",";
        }
        if (config->getMaxDays() > 0) {
            json += "\"max_days\":" + String(config->getMaxDays()) + ",";
        }
        // Add credentials_secured field to indicate storage mode
        json += "\"credentials_secured\":" + String(credentialsSecured ? "true" : "false");
    }
    
    json += "}";
    
    server->send(200, "application/json", json);
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
    
    // Set headers for streaming
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    // Send initial chunk with headers to avoid "content length is zero" warning
    server->send(200, "text/plain", "--- System Logs ---\n");
    
    // Stream logs directly using ChunkedPrint adapter
    ChunkedPrint chunkedOutput(server);
    Logger::getInstance().printLogs(chunkedOutput);
    
    // Terminate chunked transmission
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
    String html = "<!DOCTYPE html><html><head>";
    html += "<title>Firmware Update - CPAP Auto-Uploader</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }";
    html += "h1 { color: #333; }";
    html += "h2 { color: #666; margin-top: 30px; }";
    html += ".container { background: white; padding: 20px; border-radius: 8px; max-width: 800px; }";
    html += ".info { margin: 10px 0; }";
    html += ".label { font-weight: bold; display: inline-block; width: 200px; }";
    html += ".value { color: #0066cc; }";
    html += ".button { display: inline-block; padding: 10px 20px; margin: 10px 5px; ";
    html += "background: #0066cc; color: white; text-decoration: none; border-radius: 4px; border: none; cursor: pointer; }";
    html += ".button:hover { background: #0052a3; }";
    html += ".button.danger { background: #cc0000; }";
    html += ".button.danger:hover { background: #a30000; }";
    html += ".form-group { margin: 15px 0; }";
    html += ".form-group label { display: block; margin-bottom: 5px; font-weight: bold; }";
    html += ".form-group input { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; }";
    html += ".warning { background: #fff3cd; border: 1px solid #ffc107; padding: 15px; border-radius: 4px; margin: 15px 0; }";
    html += ".error { background: #f8d7da; border: 1px solid #f5c6cb; padding: 15px; border-radius: 4px; margin: 15px 0; }";
    html += ".success { background: #d4edda; border: 1px solid #c3e6cb; padding: 15px; border-radius: 4px; margin: 15px 0; }";
    html += "</style>";
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<h1>Firmware Update</h1>";
    
    // Current version info
    html += "<h2>Current Firmware</h2>";
    if (otaManager) {
        html += "<div class='info'><span class='label'>Version:</span><span class='value'>" + otaManager->getCurrentVersion() + "</span></div>";
    } else {
        html += "<div class='info'><span class='label'>Version:</span><span class='value'>Unknown</span></div>";
    }
    
    // Check if update is in progress
    if (otaManager && otaManager->isUpdateInProgress()) {
        html += "<div class='warning'>";
        html += "<h3>Update in Progress</h3>";
        html += "<p>A firmware update is currently in progress. Please wait for it to complete.</p>";
        html += "</div>";
    } else {
        // Warning message
        html += "<div class='warning'>";
        html += "<h3>WARNING: Important Safety Information</h3>";
        html += "<ul>";
        html += "<li><strong>Do not power off</strong> the device during update</li>";
        html += "<li><strong>Ensure stable WiFi</strong> connection before starting</li>";
        html += "<li><strong>Do NOT remove SD card</strong> from CPAP machine during update</li>";
        html += "<li>Update process takes 1-2 minutes</li>";
        html += "<li>Device will restart automatically when complete</li>";
        html += "</ul>";
        html += "</div>";
        
        // File upload method
        html += "<h2>Method 1: Upload Firmware File</h2>";
        html += "<form id='uploadForm' enctype='multipart/form-data'>";
        html += "<div class='form-group'>";
        html += "<label for='firmwareFile'>Select firmware file (.bin):</label>";
        html += "<input type='file' id='firmwareFile' name='firmware' accept='.bin' required>";
        html += "</div>";
        html += "<button type='submit' class='button'>Upload & Install</button>";
        html += "<span id='uploadStatus' style='margin-left: 10px; color: #0066cc;'></span>";
        html += "</form>";
        
        // URL download method
        html += "<h2>Method 2: Download from URL</h2>";
        html += "<form id='urlForm'>";
        html += "<div class='form-group'>";
        html += "<label for='firmwareURL'>Firmware URL:</label>";
        html += "<input type='url' id='firmwareURL' name='url' placeholder='https://github.com/amanuense/CPAP_data_uploader/releases/download/latest/firmware-ota-upgrade.bin' required>";
        html += "</div>";
        html += "<button type='submit' class='button'>Download & Install</button>";
        html += "<span id='downloadStatus' style='margin-left: 10px; color: #0066cc;'></span>";
        html += "</form>";
        
        // Result display
        html += "<div id='resultContainer'></div>";
    }
    
    html += "<h2>Navigation</h2>";
    html += "<a href='/' class='button'>Back to Status</a>";
    
    html += "</div>";
    
    // JavaScript for handling uploads
    html += "<script>";
    html += "let updateInProgress = false;";
    
    // File upload handler
    html += "document.getElementById('uploadForm').addEventListener('submit', function(e) {";
    html += "  e.preventDefault();";
    html += "  if (updateInProgress) return;";
    html += "  const fileInput = document.getElementById('firmwareFile');";
    html += "  if (!fileInput.files[0]) { alert('Please select a file'); return; }";
    html += "  uploadFirmware(fileInput.files[0]);";
    html += "});";
    
    // URL download handler
    html += "document.getElementById('urlForm').addEventListener('submit', function(e) {";
    html += "  e.preventDefault();";
    html += "  if (updateInProgress) return;";
    html += "  const url = document.getElementById('firmwareURL').value;";
    html += "  if (!url) { alert('Please enter a URL'); return; }";
    html += "  downloadFirmware(url);";
    html += "});";
    
    // Upload function
    html += "function uploadFirmware(file) {";
    html += "  updateInProgress = true;";
    html += "  document.getElementById('uploadStatus').textContent = 'Uploading firmware...';";
    html += "  const formData = new FormData();";
    html += "  formData.append('firmware', file);";
    html += "  fetch('/ota-upload', { method: 'POST', body: formData })";
    html += "    .then(response => response.json())";
    html += "    .then(data => handleResult(data))";
    html += "    .catch(error => handleError('Upload failed: ' + error));";
    html += "}";
    
    // Download function
    html += "function downloadFirmware(url) {";
    html += "  updateInProgress = true;";
    html += "  document.getElementById('downloadStatus').textContent = 'Downloading firmware...';";
    html += "  const formData = new FormData();";
    html += "  formData.append('url', url);";
    html += "  fetch('/ota-url', { method: 'POST', body: formData })";
    html += "    .then(response => response.json())";
    html += "    .then(data => handleResult(data))";
    html += "    .catch(error => handleError('Download failed: ' + error));";
    html += "}";
    
    // Result handlers
    html += "function handleResult(data) {";
    html += "  updateInProgress = false;";
    html += "  document.getElementById('uploadStatus').textContent = '';";
    html += "  document.getElementById('downloadStatus').textContent = '';";
    html += "  const container = document.getElementById('resultContainer');";
    html += "  if (data.success) {";
    html += "    container.innerHTML = '<div class=\"success\"><h3>Update Successful!</h3><p>' + data.message + '</p><p>Redirecting to status page in 60 seconds...</p></div>';";
    html += "    setTimeout(() => { window.location.href = '/'; }, 60000);";
    html += "  } else {";
    html += "    container.innerHTML = '<div class=\"error\"><h3>Update Failed</h3><p>' + data.message + '</p></div>';";
    html += "  }";
    html += "}";
    
    html += "function handleError(message) {";
    html += "  updateInProgress = false;";
    html += "  document.getElementById('uploadStatus').textContent = '';";
    html += "  document.getElementById('downloadStatus').textContent = '';";
    html += "  document.getElementById('resultContainer').innerHTML = '<div class=\"error\"><h3>Error</h3><p>' + message + '</p></div>';";
    html += "}";
    
    html += "</script>";
    html += "</body></html>";
    
    server->send(200, "text/html", html);
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
    String html = "<!DOCTYPE html><html><head>";
    html += "<title>SD Activity Monitor - CPAP Auto-Uploader</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>";
    html += "body{font-family:monospace;background:#1a1a2e;color:#e0e0e0;margin:20px}";
    html += "h1{color:#00d4ff}";
    html += ".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px;margin:20px 0}";
    html += ".stat{background:#16213e;padding:15px;border-radius:8px;text-align:center}";
    html += ".stat .value{font-size:2em;color:#00d4ff;display:block;margin:5px 0}";
    html += ".stat .label{color:#888;font-size:0.9em}";
    html += ".bar-chart{background:#16213e;padding:15px;border-radius:8px;margin:20px 0;overflow-x:auto}";
    html += ".bar-row{display:flex;align-items:center;height:20px;margin:2px 0}";
    html += ".bar-label{width:60px;text-align:right;padding-right:8px;font-size:0.8em;color:#888}";
    html += ".bar{height:16px;border-radius:2px;min-width:1px;transition:width 0.3s}";
    html += ".bar.active{background:#ff4444}";
    html += ".bar.idle{background:#16213e}";
    html += ".indicator{display:inline-block;width:20px;height:20px;border-radius:50%;margin-left:10px;vertical-align:middle}";
    html += ".indicator.busy{background:#ff4444;box-shadow:0 0 10px #ff4444}";
    html += ".indicator.idle{background:#44ff44;box-shadow:0 0 10px #44ff44}";
    html += "button{background:#00d4ff;color:#000;border:none;padding:12px 24px;border-radius:6px;";
    html += "font-size:1em;cursor:pointer;margin:5px}";
    html += "button:hover{background:#00b4d8}";
    html += "button.stop{background:#ff4444}button.stop:hover{background:#cc3333}";
    html += "a{color:#00d4ff}";
    html += "</style></head><body>";
    
    html += "<h1>SD Activity Monitor <span id='indicator' class='indicator idle'></span></h1>";
    html += "<p><a href='/'>← Back to Status</a></p>";
    
    html += "<div style='margin:20px 0'>";
    html += "<button onclick='startMonitor()'>Start Monitor</button>";
    html += "<button class='stop' onclick='stopMonitor()'>Stop Monitor</button>";
    html += "</div>";
    
    html += "<div class='stats'>";
    html += "<div class='stat'><span class='label'>Last Pulse Count</span><span class='value' id='pulses'>--</span></div>";
    html += "<div class='stat'><span class='label'>Consecutive Idle</span><span class='value' id='idle'>--</span></div>";
    html += "<div class='stat'><span class='label'>Longest Idle</span><span class='value' id='longest'>--</span></div>";
    html += "<div class='stat'><span class='label'>Active / Idle Samples</span><span class='value' id='ratio'>--</span></div>";
    html += "</div>";
    
    html += "<h2>Activity Timeline (1 bar = 1 second)</h2>";
    html += "<div class='bar-chart' id='chart'><em>Waiting for data...</em></div>";
    
    html += "<script>";
    html += "let polling=null;";
    html += "function startMonitor(){fetch('/api/monitor-start').then(()=>{if(!polling)polling=setInterval(fetchData,1000);})}";
    html += "function stopMonitor(){fetch('/api/monitor-stop');if(polling){clearInterval(polling);polling=null;}}";
    html += "function fetchData(){fetch('/api/sd-activity').then(r=>r.json()).then(d=>{";
    html += "document.getElementById('pulses').textContent=d.last_pulse_count;";
    html += "document.getElementById('idle').textContent=(d.consecutive_idle_ms/1000).toFixed(1)+'s';";
    html += "document.getElementById('longest').textContent=(d.longest_idle_ms/1000).toFixed(1)+'s';";
    html += "document.getElementById('ratio').textContent=d.total_active_samples+' / '+d.total_idle_samples;";
    html += "let ind=document.getElementById('indicator');";
    html += "ind.className='indicator '+(d.is_busy?'busy':'idle');";
    html += "let chart=document.getElementById('chart');";
    html += "if(d.samples&&d.samples.length>0){";
    html += "let html='';";
    html += "d.samples.forEach(s=>{";
    html += "let w=Math.min(Math.max(s.p/10,1),300);";
    html += "let cls=s.a?'active':'idle';";
    html += "let sec=s.t%3600;let m=Math.floor(sec/60);let ss=sec%60;";
    html += "let lbl=String(m).padStart(2,'0')+':'+String(ss).padStart(2,'0');";
    html += "html+='<div class=\"bar-row\"><span class=\"bar-label\">'+lbl+'</span>';";
    html += "html+='<div class=\"bar '+cls+'\" style=\"width:'+w+'px\" title=\"'+s.p+' pulses\"></div>';";
    html += "html+=' <small>'+s.p+'</small></div>';";
    html += "});chart.innerHTML=html;}";
    html += "}).catch(e=>console.error('Fetch error:',e));}";
    html += "startMonitor();";  // Auto-start on page load
    html += "</script>";
    
    html += "</body></html>";
    
    server->send(200, "text/html", html);
}