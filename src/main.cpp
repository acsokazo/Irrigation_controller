/*
 * ESP32_Relay_X8_Modbus  –  WiFi / MQTT / OTA / Web Dashboard Firmware
 * Board model : 303E32DC812
 * Framework   : Arduino / PlatformIO
 *
 * CONFIRMED HARDWARE (from pin scanner):
 *   74HC595 shift register:
 *     LATCH = GPIO25, CLOCK = GPIO26, DATA = GPIO33, OE = GPIO13
 *
 * Web dashboard : http://<board-ip>/
 * OTA update    : http://<board-ip>/update  or via PlatformIO upload_port
 *
 * Setup: copy include/config.example.h to include/config.h and fill in values.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include "config.h"

// ─────────────────────────────────────────────
//  Hardware
// ─────────────────────────────────────────────
#define PIN_LATCH    25
#define PIN_CLOCK    26
#define PIN_DATA     33
#define PIN_OE       13

const int8_t INPUT_PINS[8] = {36, 39, 34, 35, 4, 16, 17, 5};
#define INPUTS_ENABLED true

// ─────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────
#define MAX_SCHEDULES     16
#define NTP_SERVER        "pool.ntp.org"
#define DEFAULT_PULSE_MS  0
#define LOG_BUFFER_LINES  80      // circular log shown in web UI

// ─────────────────────────────────────────────────────────────────────────────
//  Serial log ring buffer — captures Serial output for the web UI
// ─────────────────────────────────────────────────────────────────────────────

String logLines[LOG_BUFFER_LINES];
int    logHead = 0;        // next write position
int    logCount = 0;       // how many lines stored so far

void logPush(const String& line) {
    logLines[logHead] = line;
    logHead = (logHead + 1) % LOG_BUFFER_LINES;
    if (logCount < LOG_BUFFER_LINES) logCount++;
    Serial.println(line);
}

// Printf-style helper that also pushes to ring buffer
void logf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    logPush(String(buf));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────────────────────────────────────

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WiFiUDP      ntpUDP;
NTPClient    ntp(ntpUDP, NTP_SERVER, NTP_UTC_OFFSET, 60000);
WebServer    webServer(80);

uint8_t  relayBits      = 0x00;
uint32_t pulseMs[8]     = {DEFAULT_PULSE_MS};
uint32_t pulseStart[8]  = {0};
bool     pulseActive[8] = {false};

bool     inputState[8]    = {false};
bool     inputLastRaw[8]  = {false};
uint32_t inputDebounce[8] = {0};
#define  DEBOUNCE_MS 20

struct Schedule {
    uint8_t  relay;
    bool     days[7];
    uint8_t  hour, minute;
    uint8_t  action;
    uint32_t pulseMs;
    bool     firedToday;
};
Schedule schedules[MAX_SCHEDULES];
uint8_t  scheduleCount   = 0;
int      lastSchedMinute = -1;

uint32_t lastWifiCheck = 0;
uint32_t lastMqttCheck = 0;
#define  WIFI_RETRY_MS 5000
#define  MQTT_RETRY_MS 3000

bool apActive = AP_DEFAULT;

// ─────────────────────────────────────────────────────────────────────────────
//  Access Point control
// ─────────────────────────────────────────────────────────────────────────────

void apStart() {
    // Switch to AP+STA only if not already in that mode
    // Avoids dropping the existing STA connection
    if (WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
    }
    bool ok;
    if (strlen(AP_PASSWORD) >= 8) {
        ok = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL);
    } else {
        ok = WiFi.softAP(AP_SSID);
    }
    if (ok) {
        apActive = true;
        logf("[AP] Started — SSID: %s  IP: %s", AP_SSID, WiFi.softAPIP().toString().c_str());
    } else {
        logf("[AP] Failed to start!");
    }
}

void apStop() {
    WiFi.softAPdisconnect(true);
    // Keep WIFI_STA mode — don't call WiFi.mode(WIFI_STA) as it resets the STA connection
    apActive = false;
    logf("[AP] Stopped");
}

void publishApState() {
    if (mqtt.connected())
        mqtt.publish((String(MQTT_ROOT) + "/ap/state").c_str(),
                     apActive ? "ON" : "OFF", true);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shift register
// ─────────────────────────────────────────────────────────────────────────────

void writeRelays() {
    digitalWrite(PIN_LATCH, LOW);
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, ~relayBits);
    digitalWrite(PIN_LATCH, HIGH);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Relay helpers
// ─────────────────────────────────────────────────────────────────────────────

String relayTopic(int idx, const char* suffix) {
    return String(MQTT_ROOT) + "/relay/" + (idx + 1) + "/" + suffix;
}
String inputTopic(int idx) {
    return String(MQTT_ROOT) + "/input/" + (idx + 1) + "/state";
}
bool getRelay(int idx) { return (relayBits >> idx) & 0x01; }

void publishRelayState(int idx) {
    mqtt.publish(relayTopic(idx, "state").c_str(), getRelay(idx) ? "ON" : "OFF", true);
}

void setRelay(int idx, bool on, uint32_t pulse = 0) {
    if (idx < 0 || idx > 7) return;
    if (on) relayBits |=  (1 << idx);
    else    relayBits &= ~(1 << idx);
    writeRelays();
    if (mqtt.connected()) publishRelayState(idx);
    logf("[RELAY] %d → %s  (reg=0x%02X)", idx+1, on?"ON":"OFF", relayBits);
    if (on && pulse > 0) {
        pulseMs[idx] = pulse; pulseStart[idx] = millis(); pulseActive[idx] = true;
    } else if (!on) {
        pulseActive[idx] = false;
    }
}

void toggleRelay(int idx, uint32_t pulse = 0) { setRelay(idx, !getRelay(idx), pulse); }

// ─────────────────────────────────────────────────────────────────────────────
//  Web dashboard HTML (served from flash, not SPIFFS — keeps it simple)
// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
//  Web server routes
// ─────────────────────────────────────────────────────────────────────────────

// Read current digital input byte
uint8_t readInputByte() {
    uint8_t val = 0;
    for (int i = 0; i < 8; i++) {
        if (INPUT_PINS[i] >= 0 && inputState[i]) val |= (1 << i);
    }
    return val;
}

void setupWebServer() {

    // ── Dashboard — served from LittleFS /index.html ──────────────────────
    webServer.on("/", HTTP_GET, []() {
        File f = LittleFS.open("/index.html", "r");
        if (!f) {
            webServer.send(500, "text/plain",
                "index.html not found.\nRun: pio run -t uploadfs");
            return;
        }
        webServer.streamFile(f, "text/html");
        f.close();
    });

    // ── GET /relay?ch=1&action=ON|OFF|TOGGLE ──────────────────────────────
    webServer.on("/relay", HTTP_GET, []() {
        if (!webServer.hasArg("ch") || !webServer.hasArg("action")) {
            webServer.send(400, "application/json", "{\"error\":\"missing ch or action\"}");
            return;
        }
        int idx = webServer.arg("ch").toInt() - 1;
        String action = webServer.arg("action");
        if (idx < 0 || idx > 7) {
            webServer.send(400, "application/json", "{\"error\":\"ch out of range\"}");
            return;
        }
        if      (action == "ON")     setRelay(idx, true);
        else if (action == "OFF")    setRelay(idx, false);
        else if (action == "TOGGLE") toggleRelay(idx);
        String resp = "{\"relay\":" + String(idx+1) + ",\"state\":\"" + (getRelay(idx)?"ON":"OFF") + "\"}";
        webServer.send(200, "application/json", resp);
    });

    // ── GET /relay/all?action=ON|OFF ──────────────────────────────────────
    webServer.on("/relay/all", HTTP_GET, []() {
        String action = webServer.arg("action");
        bool on = (action == "ON");
        relayBits = on ? 0xFF : 0x00;
        writeRelays();
        if (mqtt.connected()) for (int i = 0; i < 8; i++) publishRelayState(i);
        logf("[WEB] All relays → %s", on ? "ON" : "OFF");
        webServer.send(200, "application/json", "{\"state\":\"" + action + "\"}");
    });

    // ── GET /api/state — returns JSON with relay bits, input bits, mqtt, ip ─
    webServer.on("/api/state", HTTP_GET, []() {
        JsonDocument doc;
        doc["relays"] = relayBits;
        doc["inputs"] = readInputByte();
        doc["mqtt"]   = mqtt.connected();
        doc["ip"]     = WiFi.localIP().toString();
        doc["ap"]     = apActive;
        doc["ap_ip"]  = apActive ? WiFi.softAPIP().toString() : "";
        doc["ap_ssid"]= AP_SSID;
        doc["uptime"] = millis() / 1000;
        String out;
        serializeJson(doc, out);
        webServer.send(200, "application/json", out);
    });

    // ── GET /api/log?since=N — returns new log lines since sequence N ──────
    webServer.on("/api/log", HTTP_GET, []() {
        int since = webServer.hasArg("since") ? webServer.arg("since").toInt() : 0;
        JsonDocument doc;
        JsonArray arr = doc["lines"].to<JsonArray>();

        // Walk ring buffer in chronological order
        int total = logCount;
        int start = (logCount < LOG_BUFFER_LINES) ? 0 : logHead;
        for (int i = 0; i < total; i++) {
            int globalIdx = (logCount < LOG_BUFFER_LINES)
                ? i
                : (logCount - LOG_BUFFER_LINES) + i;
            if (globalIdx < since) continue;
            int bufIdx = (start + i) % LOG_BUFFER_LINES;
            arr.add(logLines[bufIdx]);
        }
        doc["total"] = logCount;

        String out;
        serializeJson(doc, out);
        webServer.send(200, "application/json", out);
    });

    // ── GET /api/log/clear ─────────────────────────────────────────────────
    webServer.on("/api/log/clear", HTTP_GET, []() {
        logHead = 0; logCount = 0;
        webServer.send(200, "application/json", "{\"ok\":true}");
    });

    // ── OTA update page (simple file upload, password protected) ──────────
    webServer.on("/update", HTTP_GET, []() {
        if (strlen(OTA_PASSWORD) > 0 &&
            !webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
            return webServer.requestAuthentication(BASIC_AUTH, "OTA Update", "Unauthorized");
        }
        webServer.send(200, "text/html",
            "<html><body style='background:#0a0e14;color:#c0cfe0;font-family:monospace;padding:40px'>"
            "<h2 style='color:#00d4ff'>OTA Firmware Update</h2>"
            "<form method='POST' action='/update' enctype='multipart/form-data'>"
            "<input type='file' name='firmware' accept='.bin' style='margin:20px 0;display:block'>"
            "<input type='submit' value='Upload & Flash' "
            "style='background:#00d4ff;color:#0a0e14;border:none;padding:10px 24px;"
            "font-family:monospace;font-size:1rem;cursor:pointer;border-radius:4px'>"
            "</form></body></html>"
        );
    });

    webServer.on("/update", HTTP_POST,
        []() {
            if (strlen(OTA_PASSWORD) > 0 &&
                !webServer.authenticate(OTA_USERNAME, OTA_PASSWORD)) {
                return webServer.requestAuthentication(BASIC_AUTH, "OTA Update", "Unauthorized");
            }
            bool ok = !Update.hasError();
            webServer.send(200, "text/html",
                ok ? "<html><body style='background:#0a0e14;color:#00ff88;font-family:monospace;padding:40px'>"
                     "<h2>✓ Update successful — rebooting…</h2></body></html>"
                   : "<html><body style='background:#0a0e14;color:#ff3860;font-family:monospace;padding:40px'>"
                     "<h2>✗ Update FAILED</h2></body></html>"
            );
            if (ok) { delay(500); ESP.restart(); }
        },
        []() {
            HTTPUpload& upload = webServer.upload();
            if (upload.status == UPLOAD_FILE_START) {
                logf("[OTA] Web upload: %s (%u bytes)", upload.filename.c_str(), upload.totalSize);
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    logf("[OTA] Begin failed: %s", Update.errorString());
                }
            } else if (upload.status == UPLOAD_FILE_WRITE) {
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    logf("[OTA] Write error: %s", Update.errorString());
                }
            } else if (upload.status == UPLOAD_FILE_END) {
                if (Update.end(true)) {
                    logf("[OTA] Upload complete: %u bytes — flashing…", upload.totalSize);
                } else {
                    logf("[OTA] End failed: %s", Update.errorString());
                }
            }
        }
    );

    // ── GET /ap?action=ON|OFF ──────────────────────────────────────────────
    webServer.on("/ap", HTTP_GET, []() {
        String action = webServer.arg("action");
        if (action == "ON" && !apActive)       apStart();
        else if (action == "OFF" && apActive)  apStop();
        String resp = "{\"active\":" + String(apActive ? "true" : "false") +
                      ",\"ip\":\"192.168.4.1\",\"ssid\":\"" + String(AP_SSID) + "\"}";
        webServer.send(200, "application/json", resp);
    });

    webServer.begin();
    logf("[WEB] Server started at http://%s/", WiFi.localIP().toString().c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  OTA (ArduinoOTA — for PlatformIO upload_port / IDE upload)
// ─────────────────────────────────────────────────────────────────────────────

void setupOTA() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) {
        ArduinoOTA.setPassword(OTA_PASSWORD);
        logf("[OTA] Password protection enabled");
    } else {
        logf("[OTA] WARNING: No OTA password set — anyone on the network can update!");
    }

    ArduinoOTA.onStart([]() {
        logf("[OTA] Starting update: %s",
             ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem");
    });
    ArduinoOTA.onEnd([]()   { logf("[OTA] Done — rebooting"); });
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        static int lastPct = -1;
        int pct = (p * 100) / t;
        if (pct != lastPct && pct % 10 == 0) { lastPct = pct; logf("[OTA] %d%%", pct); }
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
    logf("[OTA] ArduinoOTA ready — hostname: %s.local", OTA_HOSTNAME);
}

// ─────────────────────────────────────────────────────────────────────────────
//  MQTT callback
// ─────────────────────────────────────────────────────────────────────────────

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String t(topic);
    String msg;
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    msg.trim();
    logf("[MQTT IN] %s → %s", topic, msg.c_str());

    // ap/set
    if (t == String(MQTT_ROOT) + "/ap/set") {
        if (msg == "ON" && !apActive) {
            apStart();
            publishApState();
        } else if (msg == "OFF" && apActive) {
            apStop();
            publishApState();
        }
        return;
    }

    if (t == String(MQTT_ROOT) + "/schedule/clear") {
        scheduleCount = 0;
        logf("[SCHED] Cleared");
        return;
    }

    if (t == String(MQTT_ROOT) + "/schedule/set") {
        JsonDocument doc;
        if (deserializeJson(doc, msg)) return;
        scheduleCount = 0;
        for (JsonObject e : doc.as<JsonArray>()) {
            if (scheduleCount >= MAX_SCHEDULES) break;
            Schedule& s = schedules[scheduleCount];
            s.relay   = (int)e["relay"] - 1;
            String ts = e["time"] | "00:00";
            s.hour    = ts.substring(0, 2).toInt();
            s.minute  = ts.substring(3, 5).toInt();
            String ds = e["days"] | "-------";
            for (int d = 0; d < 7; d++) s.days[d] = (ds[d] != '-');
            String act = e["action"] | "ON";
            s.action   = (act == "OFF") ? 0 : (act == "TOGGLE") ? 2 : 1;
            s.pulseMs  = e["pulse_ms"] | 0;
            s.firedToday = false;
            scheduleCount++;
        }
        logf("[SCHED] Loaded %d entries", scheduleCount);
        return;
    }

    if (t == String(MQTT_ROOT) + "/relay/all/set") {
        relayBits = (msg == "ON") ? 0xFF : 0x00;
        writeRelays();
        for (int i = 0; i < 8; i++) publishRelayState(i);
        return;
    }

    for (int i = 0; i < 8; i++) {
        if (t != relayTopic(i, "set")) continue;
        uint32_t pulse = pulseMs[i];
        bool on = getRelay(i);
        if (msg.startsWith("{")) {
            JsonDocument doc;
            if (!deserializeJson(doc, msg)) {
                String state = doc["state"] | "";
                pulse = doc["pulse_ms"] | (int)pulse;
                if      (state == "ON")     on = true;
                else if (state == "OFF")    on = false;
                else if (state == "TOGGLE") on = !getRelay(i);
            }
        } else {
            if      (msg == "ON")     on = true;
            else if (msg == "OFF")    on = false;
            else if (msg == "TOGGLE") on = !getRelay(i);
        }
        setRelay(i, on, on ? pulse : 0);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi & MQTT
// ─────────────────────────────────────────────────────────────────────────────

void connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    logf("[WiFi] Connecting to %s…", WIFI_SSID);

    // Always use AP_STA if AP is active so it survives the reconnect
    WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
        delay(250); Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
        logf("[WiFi] Connected. IP: %s", WiFi.localIP().toString().c_str());
        ntp.begin();
        setupOTA();
        setupWebServer();

        // Re-raise AP if it was active before the reconnect — WiFi.begin()
        // can silently drop the soft-AP on some SDK versions
        if (apActive) {
            logf("[AP] Restoring AP after WiFi reconnect…");
            apStart();
        }
    } else {
        logf("[WiFi] Failed – will retry in %ds", WIFI_RETRY_MS/1000);
    }
}

void connectMqtt() {
    if (mqtt.connected()) return;
    logf("[MQTT] Connecting to %s:%d…", MQTT_BROKER, MQTT_PORT);
    String lwt = String(MQTT_ROOT) + "/status";
    bool ok = (strlen(MQTT_USER) > 0)
        ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD, lwt.c_str(), 0, true, "offline")
        : mqtt.connect(MQTT_CLIENT_ID, nullptr, nullptr, lwt.c_str(), 0, true, "offline");
    if (ok) {
        logf("[MQTT] Connected OK");
        mqtt.publish(lwt.c_str(), "online", true);
        mqtt.subscribe((String(MQTT_ROOT) + "/relay/+/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/relay/all/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/schedule/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/schedule/clear").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/ap/set").c_str());
        for (int i = 0; i < 8; i++) publishRelayState(i);
        publishApState();
    } else {
        logf("[MQTT] FAILED rc=%d", mqtt.state());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Schedule engine
// ─────────────────────────────────────────────────────────────────────────────

void runSchedules() {
    if (!ntp.isTimeSet()) return;
    time_t epoch = ntp.getEpochTime();
    struct tm* t = localtime(&epoch);
    int wday = (t->tm_wday + 6) % 7;
    int h = t->tm_hour, m = t->tm_min;
    int cur = h * 60 + m;
    if (cur != lastSchedMinute) {
        lastSchedMinute = cur;
        for (int i = 0; i < scheduleCount; i++) schedules[i].firedToday = false;
    }
    for (int i = 0; i < scheduleCount; i++) {
        Schedule& s = schedules[i];
        if (s.firedToday || !s.days[wday] || s.hour != h || s.minute != m) continue;
        if      (s.action == 0) setRelay(s.relay, false);
        else if (s.action == 1) setRelay(s.relay, true, s.pulseMs);
        else                    toggleRelay(s.relay, s.pulseMs);
        s.firedToday = true;
        logf("[SCHED] Fired entry %d → relay %d", i, s.relay+1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup & Loop
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    logf("[BOOT] ESP32_Relay_X8 firmware starting");
    logf("[BOOT] Board: 303E32DC812");

    if (!LittleFS.begin(true)) {
        logf("[ERROR] LittleFS mount failed!");
    } else {
        logf("[OK] LittleFS mounted");
        File f = LittleFS.open("/index.html", "r");
        if (f) { logf("[OK] index.html found (%d bytes)", f.size()); f.close(); }
        else    { logf("[WARN] index.html not found — run: pio run -t uploadfs"); }
    }

    pinMode(PIN_LATCH, OUTPUT);
    pinMode(PIN_CLOCK, OUTPUT);
    pinMode(PIN_DATA,  OUTPUT);
    pinMode(PIN_OE,    OUTPUT);
    digitalWrite(PIN_LATCH, HIGH);
    digitalWrite(PIN_CLOCK, LOW);
    digitalWrite(PIN_OE,    LOW);
    relayBits = 0x00;
    writeRelays();
    logf("[OK] Shift register init — all relays OFF");

    if (INPUTS_ENABLED) {
        for (int i = 0; i < 8; i++) {
            if (INPUT_PINS[i] >= 0) {
                pinMode(INPUT_PINS[i], INPUT_PULLUP);
                inputLastRaw[i] = !digitalRead(INPUT_PINS[i]);
                inputState[i]   = inputLastRaw[i];
            }
        }
        logf("[OK] Digital inputs configured");
    }

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(1024);

    connectWifi();
    connectMqtt();

    if (AP_DEFAULT) {
        apStart();
    }
}

void loop() {
    uint32_t now = millis();

    ArduinoOTA.handle();
    webServer.handleClient();

    if (now - lastWifiCheck > WIFI_RETRY_MS) {
        lastWifiCheck = now;
        connectWifi();
    }
    if (WiFi.status() == WL_CONNECTED && now - lastMqttCheck > MQTT_RETRY_MS) {
        lastMqttCheck = now;
        connectMqtt();
    }
    mqtt.loop();
    ntp.update();

    // Pulse / auto-off
    for (int i = 0; i < 8; i++) {
        if (pulseActive[i] && (now - pulseStart[i] >= pulseMs[i])) {
            setRelay(i, false);
            pulseActive[i] = false;
        }
    }

    // Digital inputs (debounced)
    if (INPUTS_ENABLED) {
        for (int i = 0; i < 8; i++) {
            if (INPUT_PINS[i] < 0) continue;
            bool raw = !digitalRead(INPUT_PINS[i]);
            if (raw != inputLastRaw[i]) { inputLastRaw[i] = raw; inputDebounce[i] = now; }
            if (now - inputDebounce[i] > DEBOUNCE_MS && raw != inputState[i]) {
                inputState[i] = raw;
                if (mqtt.connected())
                    mqtt.publish(inputTopic(i).c_str(), raw ? "ON" : "OFF", false);
                logf("[IN] Input %d → %s", i+1, raw?"ON":"OFF");
            }
        }
    }

    // Schedule engine
    static uint32_t lastSchedCheck = 0;
    if (now - lastSchedCheck >= 1000) {
        lastSchedCheck = now;
        runSchedules();
    }
}