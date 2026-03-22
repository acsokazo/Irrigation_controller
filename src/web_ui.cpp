/*
 * web_ui.cpp — Web dashboard, REST API and OTA update handler
 *
 * Dashboard served from PROGMEM (see src/web_pages.cpp)
 * OTA firmware update via /update (password protected)
 *
 * REST API:
 *   GET /api/state              → full system state JSON
 *   GET /api/log?since=N        → log lines JSON
 *   GET /api/log/clear
 *   POST /api/zone/N/start      → start zone N (optional ?duration=min)
 *   POST /api/zone/N/stop       → stop (any running program)
 *   POST /api/program/start     → start full cycle
 *   POST /api/program/stop      → stop running program
 *   GET  /ap?action=ON|OFF
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Update.h>
#include "config.h"
#include "web_ui.h"
#include "web_pages.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Externals from main.cpp
// ─────────────────────────────────────────────────────────────────────────────

#define NUM_ZONES 8

// Relay / zone
extern uint8_t   relayBits;
extern bool      getRelay(int idx);
extern void      setRelay(int idx, bool on, uint32_t pulse);
extern void      allRelaysOff();
extern void      publishRelayState(int idx);
extern void      publishAllZoneStates();
extern uint16_t  zoneDuration[NUM_ZONES];
extern String    zoneName[NUM_ZONES];

// Sequencer
extern bool      programRunning;
extern uint8_t   currentStep;
extern uint32_t  stepStartMs;
extern uint32_t  stepDurationMs;
extern bool      delayActive;
extern uint32_t  delayStartMs;
extern uint16_t  sequenceDelaySec;

struct IrrigProgram {
    uint8_t  zones[NUM_ZONES];
    uint8_t  count;
    uint16_t durations[NUM_ZONES];
};
extern IrrigProgram activeProgram;
extern void startZone(int idx, uint16_t durationMin);
extern void stopProgram();
extern void startProgram(const IrrigProgram& prog);
extern IrrigProgram savedProgram;
extern void publishSavedSequence();

// AP
extern bool apActive;
extern void apStart();
extern void apStop();

// Inputs
extern bool         inputState[8];
extern const int8_t INPUT_PINS[8];

// Log ring buffer
extern String logLines[];
extern int    logHead;
extern int    logCount;
extern void   logf(const char* fmt, ...);

// MQTT
extern PubSubClient mqtt;

// WiFi runtime credentials
extern char wifiSsid[64];
extern void wifiCredsSave(const char* ssid, const char* pass);

// Daily schedule
struct DailySchedule {
    bool    enabled;
    bool    days[7];
    uint8_t hour, minute;
    bool    firedToday;
};
extern DailySchedule dailySched;

// ─────────────────────────────────────────────────────────────────────────────
//  Web server instance
// ─────────────────────────────────────────────────────────────────────────────

WebServer webServer(80);

// ─────────────────────────────────────────────────────────────────────────────
//  Route handlers
// ─────────────────────────────────────────────────────────────────────────────

// GET /  — dashboard served gzip compressed (saves ~17KB flash vs plain string)
static void handleRoot() {
    webServer.sendHeader("Content-Encoding", "gzip");
    webServer.send_P(200, "text/html",
        (const char*)PAGE_DASHBOARD_GZ, PAGE_DASHBOARD_LEN);
}

// GET /api/state
static void handleApiState() {
    JsonDocument doc;

    // Zone states + config
    JsonArray zones = doc["zones"].to<JsonArray>();
    for (int i = 0; i < NUM_ZONES; i++) {
        JsonObject z = zones.add<JsonObject>();
        z["id"]       = i + 1;
        z["name"]     = zoneName[i];
        z["state"]    = getRelay(i) ? "ON" : "OFF";
        z["duration"] = zoneDuration[i];
    }

    // Running program status
    doc["program_running"] = programRunning;
    if (programRunning) {
        if (delayActive) {
            uint32_t elapsed   = (millis() - delayStartMs) / 1000;
            uint32_t remaining = sequenceDelaySec > elapsed
                                 ? sequenceDelaySec - elapsed : 0;
            doc["phase"]         = "filling";
            doc["next_zone"]     = (currentStep < activeProgram.count)
                                   ? activeProgram.zones[currentStep] + 1 : 0;
            doc["elapsed_sec"]   = elapsed;
            doc["remaining_sec"] = remaining;
            doc["delay_sec"]     = sequenceDelaySec;
        } else {
            int zone = (currentStep < activeProgram.count)
                       ? activeProgram.zones[currentStep] : 0;
            uint32_t elapsed      = (millis() - stepStartMs) / 1000;
            uint32_t stepTotalSec = stepDurationMs / 1000;
            uint32_t remaining    = stepDurationMs > (millis() - stepStartMs)
                ? (stepDurationMs - (millis() - stepStartMs)) / 1000 : 0;
            doc["phase"]             = "watering";
            doc["active_zone"]       = zone + 1;
            doc["elapsed_sec"]       = elapsed;
            doc["remaining_sec"]     = remaining;
            doc["step_duration_sec"] = stepTotalSec;
        }
    }
    doc["sequence_delay_sec"] = sequenceDelaySec;

    // Saved sequence (for editor sync)
    JsonArray seq = doc["sequence"].to<JsonArray>();
    for (int i = 0; i < savedProgram.count; i++) {
        int z = savedProgram.zones[i];
        JsonObject o = seq.add<JsonObject>();
        o["zone"]     = z + 1;
        o["duration"] = savedProgram.durations[i] > 0
                        ? savedProgram.durations[i]
                        : zoneDuration[z];
        o["name"]     = zoneName[z];
        o["enabled"]  = true;
    }

    // Schedule
    JsonObject sched = doc["schedule"].to<JsonObject>();
    sched["enabled"] = dailySched.enabled;
    char timeStr[6];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", dailySched.hour, dailySched.minute);
    sched["time"] = timeStr;
    String days = "";
    const char dayChars[] = "MTWTFSS";
    for (int d = 0; d < 7; d++) days += dailySched.days[d] ? dayChars[d] : '-';
    sched["days"] = days;

    // Network / system
    doc["mqtt"]    = mqtt.connected();
    doc["ip"]      = WiFi.localIP().toString();
    doc["ssid"]    = String(wifiSsid);
    doc["ap"]      = apActive;
    doc["ap_ip"]   = apActive ? WiFi.softAPIP().toString() : "";
    doc["ap_ssid"] = AP_SSID;
    doc["uptime"]  = millis() / 1000;

    String out;
    serializeJson(doc, out);
    webServer.send(200, "application/json", out);
}

// GET /api/log?since=N
static void handleApiLog() {
    int since = webServer.hasArg("since") ? webServer.arg("since").toInt() : 0;
    JsonDocument doc;
    JsonArray arr = doc["lines"].to<JsonArray>();
    int total = logCount;
    int start = (logCount < LOG_BUFFER_LINES) ? 0 : logHead;
    for (int i = 0; i < total; i++) {
        int globalIdx = (logCount < LOG_BUFFER_LINES)
            ? i : (logCount - LOG_BUFFER_LINES) + i;
        if (globalIdx < since) continue;
        int bufIdx = (start + i) % LOG_BUFFER_LINES;
        arr.add(logLines[bufIdx]);
    }
    doc["total"] = logCount;
    String out;
    serializeJson(doc, out);
    webServer.send(200, "application/json", out);
}

// GET /api/log/clear
static void handleApiLogClear() {
    logHead = 0; logCount = 0;
    webServer.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/zone/start?n=N  (optional ?duration=M)
static void handleZoneStart() {
    if (!webServer.hasArg("n")) {
        webServer.send(400, "application/json", "{\"error\":\"missing zone n\"}");
        return;
    }
    int idx = webServer.arg("n").toInt() - 1;
    if (idx < 0 || idx >= NUM_ZONES) {
        webServer.send(400, "application/json", "{\"error\":\"zone out of range\"}");
        return;
    }
    uint16_t dur = webServer.hasArg("duration") ? webServer.arg("duration").toInt() : 0;
    startZone(idx, dur);
    webServer.send(200, "application/json",
        "{\"ok\":true,\"zone\":" + String(idx+1) + "}");
}

// POST /api/zone/stop
static void handleZoneStop() {
    stopProgram();
    webServer.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/program/start — run saved sequence
static void handleProgramStart() {
    if (savedProgram.count > 0) {
        startProgram(savedProgram);
        webServer.send(200, "application/json",
            "{\"ok\":true,\"steps\":" + String(savedProgram.count) + "}");
    } else {
        webServer.send(400, "application/json",
            "{\"error\":\"no sequence configured\"}");
    }
}

// POST /api/program/stop
static void handleProgramStop() {
    stopProgram();
    webServer.send(200, "application/json", "{\"ok\":true}");
}

// POST /api/program/set  — custom sequence from web editor
// Body: [{"zone":1,"duration":5},{"zone":3,"duration":10}]
static void handleProgramSet() {
    JsonDocument doc;
    if (deserializeJson(doc, webServer.arg("plain"))) {
        webServer.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }
    IrrigProgram p;
    p.count = 0;
    for (JsonObject e : doc.as<JsonArray>()) {
        if (p.count >= NUM_ZONES) break;
        int z = (int)e["zone"] - 1;
        if (z < 0 || z >= NUM_ZONES) continue;
        p.zones[p.count]     = z;
        p.durations[p.count] = e["duration"] | 0;
        p.count++;
    }
    if (p.count > 0) {
        startProgram(p);
        webServer.send(200, "application/json", "{\"ok\":true,\"steps\":" + String(p.count) + "}");
    } else {
        webServer.send(400, "application/json", "{\"error\":\"empty sequence\"}");
    }
}

// GET /ap?action=ON|OFF
static void handleAp() {
    String action = webServer.arg("action");
    if      (action == "ON"  && !apActive) apStart();
    else if (action == "OFF" &&  apActive) apStop();
    String resp = "{\"active\":"  + String(apActive ? "true" : "false") +
                  ",\"ip\":\"192.168.4.1\""
                  ",\"ssid\":\"" + String(AP_SSID) + "\"}";
    webServer.send(200, "application/json", resp);
}

// GET /wifi — WiFi settings page
static void handleWifiGet() {
    if (strlen(OTA_PASSWORD) > 0 &&
        !webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
        return webServer.requestAuthentication(BASIC_AUTH, "WiFi Settings", "Unauthorized");
    }
    webServer.send_P(200, "text/html", PAGE_WIFI);
}

// POST /wifi — save new credentials and reboot
static void handleWifiPost() {
    if (strlen(OTA_PASSWORD) > 0 &&
        !webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
        return webServer.requestAuthentication(BASIC_AUTH, "WiFi Settings", "Unauthorized");
    }
    String ssid = webServer.arg("ssid");
    String pass = webServer.arg("pass");
    ssid.trim(); pass.trim();
    if (ssid.length() == 0) {
        webServer.send(400, "text/html",
            "<html><body style='background:#0a0e14;color:#ff3860;font-family:monospace;padding:40px'>"
            "<h2>&#10007; SSID cannot be empty</h2>"
            "<p><a href='/wifi' style='color:#00d4ff'>Back</a></p></body></html>");
        return;
    }
    wifiCredsSave(ssid.c_str(), pass.c_str());
    webServer.send(200, "text/html",
        "<html><body style='background:#0a0e14;color:#00ff88;font-family:monospace;padding:40px'>"
        "<h2>&#10003; WiFi credentials saved</h2>"
        "<p style='color:#c0cfe0'>Board is rebooting and will connect to <b>" + ssid + "</b>&hellip;</p>"
        "<p style='margin-top:20px;color:#6a8aaa'>If connection fails, the board will restart in AP mode.<br>"
        "AP SSID: <b>" + String(AP_SSID) + "</b> &mdash; then visit <b>192.168.4.1/wifi</b></p>"
        "</body></html>");
    delay(1000);
    ESP.restart();
}
static void handleUpdateGet() {
    if (strlen(OTA_PASSWORD) > 0 &&
        !webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
        return webServer.requestAuthentication(BASIC_AUTH, "OTA Update", "Unauthorized");
    }
    webServer.send_P(200, "text/html", PAGE_OTA_GET);
}

// POST /update
static void handleUpdatePost() {
    if (strlen(OTA_PASSWORD) > 0 &&
        !webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
        return webServer.requestAuthentication(BASIC_AUTH, "OTA Update", "Unauthorized");
    }
    bool ok = !Update.hasError();
    webServer.send_P(200, "text/html", ok ? PAGE_OTA_OK : PAGE_OTA_FAIL);
    if (ok) { delay(500); ESP.restart(); }
}

static void handleUpdateUpload() {
    HTTPUpload& upload = webServer.upload();
    if (upload.status == UPLOAD_FILE_START) {
        logf("[OTA] Web upload: %s", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
            logf("[OTA] Begin failed: %s", Update.errorString());
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
            logf("[OTA] Write error: %s", Update.errorString());
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true))
            logf("[OTA] Upload complete: %u bytes", upload.totalSize);
        else
            logf("[OTA] End failed: %s", Update.errorString());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

void setupWebServer() {
    webServer.on("/",                    HTTP_GET,  handleRoot);
    webServer.on("/api/state",           HTTP_GET,  handleApiState);
    webServer.on("/api/log",             HTTP_GET,  handleApiLog);
    webServer.on("/api/log/clear",       HTTP_GET,  handleApiLogClear);
    webServer.on("/api/program/start",   HTTP_POST, handleProgramStart);
    webServer.on("/api/program/stop",    HTTP_POST, handleProgramStop);
    webServer.on("/api/program/set",     HTTP_POST, handleProgramSet);
    webServer.on("/api/zone/start",      HTTP_POST, handleZoneStart);
    webServer.on("/api/zone/stop",       HTTP_POST, handleZoneStop);
    webServer.on("/wifi",                HTTP_GET,  handleWifiGet);
    webServer.on("/wifi",                HTTP_POST, handleWifiPost);
    webServer.on("/ap",                  HTTP_GET,  handleAp);
    webServer.on("/update",              HTTP_GET,  handleUpdateGet);
    webServer.on("/update",              HTTP_POST, handleUpdatePost, handleUpdateUpload);
    webServer.begin();
    logf("[WEB] Server started at http://%s/", WiFi.localIP().toString().c_str());
}

void setupOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) {
        ArduinoOTA.setPassword(OTA_PASSWORD);
        logf("[OTA] Password protection enabled");
    } else {
        logf("[OTA] WARNING: No OTA password set!");
    }
    ArduinoOTA.onStart([]() {
        logf("[OTA] Starting: %s",
             ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
    });
    ArduinoOTA.onEnd([]()   { logf("[OTA] Done — rebooting"); });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        static int last = -1;
        int pct = (p * 100) / t;
        if (pct != last && pct % 10 == 0) { last = pct; logf("[OTA] %d%%", pct); }
    });
    ArduinoOTA.onError([](ota_error_t e) {
        logf("[OTA] Error[%u]: %s", e,
             e==OTA_AUTH_ERROR    ? "Auth failed"    :
             e==OTA_BEGIN_ERROR   ? "Begin failed"   :
             e==OTA_CONNECT_ERROR ? "Connect failed" :
             e==OTA_RECEIVE_ERROR ? "Receive failed" :
             e==OTA_END_ERROR     ? "End failed"     : "Unknown");
    });
    ArduinoOTA.begin();
    logf("[OTA] ArduinoOTA ready — %s.local", OTA_HOSTNAME);
}

void webServerLoop() {
    ArduinoOTA.handle();
    webServer.handleClient();
}
