/*
 * ESP32_Relay_X8_Modbus  –  Irrigation Controller Firmware
 * Board model : 303E32DC812
 * Framework   : Arduino / PlatformIO
 *
 * CONFIRMED HARDWARE (from pin scanner):
 *   74HC595 shift register:
 *     LATCH = GPIO25, CLOCK = GPIO26, DATA = GPIO33, OE = GPIO13
 *
 * 8 irrigation zones, sequential (only one zone active at a time).
 *
 * Web dashboard : http://<board-ip>/
 * OTA update    : http://<board-ip>/update  or via PlatformIO upload_port
 *
 * Setup: copy include/config.example.h to include/config.h and fill in values.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "config.h"
#include "web_ui.h"

// ─────────────────────────────────────────────
//  Hardware
// ─────────────────────────────────────────────
#define PIN_LATCH    25
#define PIN_CLOCK    26
#define PIN_DATA     33
#define PIN_OE       13

extern const int8_t INPUT_PINS[8] = {36, 39, 34, 35, 4, 16, 17, 5};
#define INPUTS_ENABLED true

// ─────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────
#define NUM_ZONES         8
#define NTP_SERVER        "pool.ntp.org"
#define DEFAULT_DURATION  10    // minutes per zone if not configured

// ─────────────────────────────────────────────────────────────────────────────
//  Serial log ring buffer
// ─────────────────────────────────────────────────────────────────────────────

String logLines[LOG_BUFFER_LINES];
int    logHead  = 0;
int    logCount = 0;

void logPush(const String& line) {
    logLines[logHead] = line;
    logHead = (logHead + 1) % LOG_BUFFER_LINES;
    if (logCount < LOG_BUFFER_LINES) logCount++;
    Serial.println(line);
}

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

// ── Shift register ────────────────────────────────────────────────────────────
uint8_t relayBits = 0x00;

// ── Zone configuration ────────────────────────────────────────────────────────
uint16_t zoneDuration[NUM_ZONES];    // minutes per zone
String   zoneName[NUM_ZONES];        // display name

// ── Irrigation sequencer ──────────────────────────────────────────────────────
struct IrrigProgram {
    uint8_t  zones[NUM_ZONES];
    uint8_t  count;
    uint16_t durations[NUM_ZONES];   // 0 = use zoneDuration[]
};

IrrigProgram activeProgram;
IrrigProgram savedProgram;   // persists the last sequence saved from web editor
bool     programRunning  = false;
uint8_t  currentStep     = 0;
uint32_t stepStartMs     = 0;
uint32_t stepDurationMs  = 0;

// ── Daily schedule ────────────────────────────────────────────────────────────
struct DailySchedule {
    bool    enabled;
    bool    days[7];          // Mon=0 … Sun=6
    uint8_t hour, minute;
    bool    firedToday;
};
DailySchedule dailySched = {false, {true,true,true,true,true,true,true}, 6, 0, false};
int lastSchedMinute = -1;

// ── Digital inputs ────────────────────────────────────────────────────────────
bool     inputState[8]    = {false};
bool     inputLastRaw[8]  = {false};
uint32_t inputDebounce[8] = {0};
#define  DEBOUNCE_MS 20

// ── Network ───────────────────────────────────────────────────────────────────
uint32_t lastWifiCheck = 0;
uint32_t lastMqttCheck = 0;
#define  WIFI_RETRY_MS 5000
#define  MQTT_RETRY_MS 3000

// Runtime WiFi credentials — loaded from Preferences, fallback to config.h
char wifiSsid[64]     = WIFI_SSID;
char wifiPassword[64] = WIFI_PASSWORD;

Preferences prefs;

void wifiCredsSave(const char* ssid, const char* pass) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    strlcpy(wifiSsid,     ssid, sizeof(wifiSsid));
    strlcpy(wifiPassword, pass, sizeof(wifiPassword));
    logf("[WiFi] Credentials saved: %s", ssid);
}

void wifiCredsLoad() {
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    prefs.end();
    if (ssid.length() > 0) {
        strlcpy(wifiSsid,     ssid.c_str(), sizeof(wifiSsid));
        strlcpy(wifiPassword, pass.c_str(), sizeof(wifiPassword));
        logf("[WiFi] Loaded credentials from storage: %s", wifiSsid);
    } else {
        logf("[WiFi] No stored credentials, using config.h defaults: %s", wifiSsid);
    }
}

bool apActive = AP_DEFAULT;

// ─────────────────────────────────────────────────────────────────────────────
//  Access Point
// ─────────────────────────────────────────────────────────────────────────────

void apStart() {
    if (WiFi.getMode() != WIFI_AP_STA) WiFi.mode(WIFI_AP_STA);
    bool ok = (strlen(AP_PASSWORD) >= 8)
        ? WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL)
        : WiFi.softAP(AP_SSID);
    if (ok) {
        apActive = true;
        logf("[AP] Started — SSID: %s  IP: %s", AP_SSID, WiFi.softAPIP().toString().c_str());
    } else {
        logf("[AP] Failed to start!");
    }
}

void apStop() {
    WiFi.softAPdisconnect(true);
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
//  Zone / relay helpers
// ─────────────────────────────────────────────────────────────────────────────

String zoneTopic(int idx, const char* suffix) {
    return String(MQTT_ROOT) + "/zone/" + (idx + 1) + "/" + suffix;
}
String inputTopic(int idx) {
    return String(MQTT_ROOT) + "/input/" + (idx + 1) + "/state";
}
bool getRelay(int idx) { return (relayBits >> idx) & 0x01; }

void setRelay(int idx, bool on, uint32_t pulse = 0) {
    if (idx < 0 || idx > 7) return;
    if (on) relayBits |=  (1 << idx);
    else    relayBits &= ~(1 << idx);
    writeRelays();
}

void allRelaysOff() {
    relayBits = 0x00;
    writeRelays();
}

void publishRelayState(int idx) {
    if (!mqtt.connected()) return;
    mqtt.publish(zoneTopic(idx, "state").c_str(), getRelay(idx) ? "ON" : "OFF", true);
}

void publishAllZoneStates() {
    for (int i = 0; i < NUM_ZONES; i++) publishRelayState(i);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Irrigation sequencer
// ─────────────────────────────────────────────────────────────────────────────

void publishProgramState();   // forward declaration
void publishSavedSequence();  // forward declaration

void stopProgram() {
    if (!programRunning) return;
    allRelaysOff();
    publishAllZoneStates();
    programRunning = false;
    logf("[IRR] Program stopped");
    publishProgramState();
}

void nextStep() {
    // Turn off the zone that just finished
    if (programRunning && currentStep < activeProgram.count) {
        int zone = activeProgram.zones[currentStep];
        setRelay(zone, false);
        publishRelayState(zone);
        logf("[IRR] Zone %d finished", zone + 1);
    }

    currentStep++;

    if (currentStep >= activeProgram.count) {
        programRunning = false;
        logf("[IRR] Program complete");
        publishProgramState();
        return;
    }

    int zone = activeProgram.zones[currentStep];
    uint32_t dur = (activeProgram.durations[currentStep] > 0)
        ? activeProgram.durations[currentStep]
        : zoneDuration[zone];
    stepDurationMs = (uint32_t)dur * 60000UL;
    stepStartMs    = millis();
    setRelay(zone, true);
    publishRelayState(zone);
    logf("[IRR] Zone %d (%s) started — %u min", zone + 1, zoneName[zone].c_str(), dur);
    publishProgramState();
}

void startProgram(const IrrigProgram& prog) {
    if (programRunning) stopProgram();
    if (prog.count == 0) return;
    activeProgram  = prog;
    currentStep    = 255;   // nextStep() increments to 0
    programRunning = true;
    nextStep();
}

void startZone(int idx, uint16_t durationMin = 0) {
    if (idx < 0 || idx >= NUM_ZONES) return;
    IrrigProgram p;
    p.count        = 1;
    p.zones[0]     = idx;
    p.durations[0] = durationMin;
    startProgram(p);
}

void startFullCycle() {
    IrrigProgram p;
    p.count = NUM_ZONES;
    for (int i = 0; i < NUM_ZONES; i++) {
        p.zones[i]     = i;
        p.durations[i] = 0;
    }
    startProgram(p);
    logf("[IRR] Full cycle started");
}

void publishProgramState() {
    if (!mqtt.connected()) return;
    String base = String(MQTT_ROOT) + "/program/";
    mqtt.publish((base + "running").c_str(), programRunning ? "ON" : "OFF", true);
    if (programRunning) {
        int zone = activeProgram.zones[currentStep];
        uint32_t elapsed   = (millis() - stepStartMs) / 1000;
        uint32_t remaining = stepDurationMs > (millis() - stepStartMs)
            ? (stepDurationMs - (millis() - stepStartMs)) / 1000 : 0;
        JsonDocument doc;
        doc["active_zone"]   = zone + 1;
        doc["step"]          = currentStep + 1;
        doc["total_steps"]   = activeProgram.count;
        doc["elapsed_sec"]   = elapsed;
        doc["remaining_sec"] = remaining;
        String out;
        serializeJson(doc, out);
        mqtt.publish((base + "status").c_str(), out.c_str(), false);
    }
}

void publishSavedSequence() {
    if (!mqtt.connected()) return;
    // Publish full sequence as JSON array to sequence/state (retained)
    // Format: [{"zone":1,"duration":10,"name":"Zone 1"}, ...]
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < savedProgram.count; i++) {
        int z = savedProgram.zones[i];
        JsonObject o = arr.add<JsonObject>();
        o["zone"]     = z + 1;
        o["duration"] = savedProgram.durations[i] > 0
                        ? savedProgram.durations[i]
                        : zoneDuration[z];
        o["name"]     = zoneName[z];
    }
    String out;
    serializeJson(doc, out);
    mqtt.publish((String(MQTT_ROOT) + "/sequence/state").c_str(), out.c_str(), true);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String t(topic);
    String msg;
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    msg.trim();
    logf("[MQTT IN] %s → %s", topic, msg.c_str());

    // ap/set
    if (t == String(MQTT_ROOT) + "/ap/set") {
        if      (msg == "ON"  && !apActive) { apStart();  publishApState(); }
        else if (msg == "OFF" &&  apActive) { apStop();   publishApState(); }
        return;
    }

    // program/stop
    if (t == String(MQTT_ROOT) + "/program/stop") {
        stopProgram(); return;
    }

    // program/start — full cycle
    if (t == String(MQTT_ROOT) + "/program/start") {
        startFullCycle(); return;
    }

    // sequence/set — save custom sequence from MQTT
    // Payload: [{"zone":1,"duration":8},{"zone":3,"duration":12}]
    if (t == String(MQTT_ROOT) + "/sequence/set") {
        JsonDocument doc;
        if (deserializeJson(doc, msg)) { logf("[SEQ] Invalid JSON"); return; }
        savedProgram.count = 0;
        for (JsonObject e : doc.as<JsonArray>()) {
            if (savedProgram.count >= NUM_ZONES) break;
            int z = (int)e["zone"] - 1;
            if (z < 0 || z >= NUM_ZONES) continue;
            savedProgram.zones[savedProgram.count]     = z;
            savedProgram.durations[savedProgram.count] = e["duration"] | 0;
            savedProgram.count++;
        }
        logf("[SEQ] Saved via MQTT (%d steps)", savedProgram.count);
        publishSavedSequence();
        return;
    }

    // program/set — run custom sequence immediately (does not save)
    // Payload: [{"zone":1,"duration":5},{"zone":3,"duration":10}]
    if (t == String(MQTT_ROOT) + "/program/set") {
        JsonDocument doc;
        if (deserializeJson(doc, msg)) { logf("[IRR] Invalid JSON"); return; }
        IrrigProgram p;
        p.count = 0;
        for (JsonObject e : doc.as<JsonArray>()) {
            if (p.count >= NUM_ZONES) break;
            p.zones[p.count]     = (int)e["zone"] - 1;
            p.durations[p.count] = e["duration"] | 0;
            p.count++;
        }
        if (p.count > 0) startProgram(p);
        return;
    }

    // schedule/set
    // Payload: {"enabled":true,"days":"MTWTFSS","time":"06:00"}
    if (t == String(MQTT_ROOT) + "/schedule/set") {
        JsonDocument doc;
        if (deserializeJson(doc, msg)) return;
        dailySched.enabled    = doc["enabled"] | false;
        String ts             = doc["time"] | "06:00";
        dailySched.hour       = ts.substring(0, 2).toInt();
        dailySched.minute     = ts.substring(3, 5).toInt();
        String ds             = doc["days"] | "MTWTFSS";
        for (int d = 0; d < 7; d++) dailySched.days[d] = (ds[d] != '-');
        dailySched.firedToday = false;
        logf("[SCHED] Set: %s enabled=%s", ts.c_str(), dailySched.enabled ? "yes" : "no");
        return;
    }

    // zone/N/config — set zone name and duration
    // Payload: {"duration":10,"name":"Front lawn"}
    for (int i = 0; i < NUM_ZONES; i++) {
        if (t == zoneTopic(i, "config")) {
            JsonDocument doc;
            if (!deserializeJson(doc, msg)) {
                if (doc["duration"].is<int>()) zoneDuration[i] = doc["duration"];
                if (doc["name"].is<const char*>()) zoneName[i] = doc["name"].as<String>();
                logf("[ZONE] %d config: \"%s\" %d min", i+1, zoneName[i].c_str(), zoneDuration[i]);
            }
            return;
        }
    }

    // zone/N/set — manual start / stop
    // Payload: ON | OFF | {"state":"ON","duration":5}
    for (int i = 0; i < NUM_ZONES; i++) {
        if (t != zoneTopic(i, "set")) continue;
        if (msg == "OFF") {
            stopProgram();
        } else if (msg == "ON") {
            startZone(i);
        } else if (msg.startsWith("{")) {
            JsonDocument doc;
            if (!deserializeJson(doc, msg)) {
                String state = doc["state"] | "ON";
                uint16_t dur = doc["duration"] | 0;
                if (state == "ON") startZone(i, dur);
                else               stopProgram();
            }
        }
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi & MQTT
// ─────────────────────────────────────────────────────────────────────────────

void connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return;
    logf("[WiFi] Connecting to %s…", wifiSsid);
    WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(wifiSsid, wifiPassword);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
        delay(250); Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
        logf("[WiFi] Connected. IP: %s", WiFi.localIP().toString().c_str());
        ntp.begin();
        setupOTA();
        setupWebServer();
        if (apActive) { logf("[AP] Restoring…"); apStart(); }
    } else {
        logf("[WiFi] Failed – retry in %ds", WIFI_RETRY_MS / 1000);
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
        mqtt.subscribe((String(MQTT_ROOT) + "/zone/+/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/zone/+/config").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/program/start").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/program/stop").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/program/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/sequence/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/schedule/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/ap/set").c_str());
        publishAllZoneStates();
        publishApState();
        publishProgramState();
        publishSavedSequence();
    } else {
        logf("[MQTT] FAILED rc=%d", mqtt.state());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Daily schedule engine
// ─────────────────────────────────────────────────────────────────────────────

void runSchedule() {
    if (!ntp.isTimeSet() || !dailySched.enabled) return;
    time_t epoch = ntp.getEpochTime();
    struct tm* t = localtime(&epoch);
    int wday = (t->tm_wday + 6) % 7;
    int h = t->tm_hour, m = t->tm_min;
    int cur = h * 60 + m;
    if (cur != lastSchedMinute) {
        lastSchedMinute = cur;
        dailySched.firedToday = false;
    }
    if (!dailySched.firedToday
        && dailySched.days[wday]
        && h == dailySched.hour
        && m == dailySched.minute) {
        dailySched.firedToday = true;
        logf("[SCHED] Daily trigger — starting full cycle");
        startFullCycle();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup & Loop
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    logf("[BOOT] ESP32 Irrigation Controller starting");
    logf("[BOOT] Board: 303E32DC812");

    pinMode(PIN_LATCH, OUTPUT);
    pinMode(PIN_CLOCK, OUTPUT);
    pinMode(PIN_DATA,  OUTPUT);
    pinMode(PIN_OE,    OUTPUT);
    digitalWrite(PIN_LATCH, HIGH);
    digitalWrite(PIN_CLOCK, LOW);
    digitalWrite(PIN_OE,    LOW);
    allRelaysOff();
    logf("[OK] All zones OFF");

    for (int i = 0; i < NUM_ZONES; i++) {
        zoneDuration[i] = DEFAULT_DURATION;
        zoneName[i]     = "Zone " + String(i + 1);
    }
    logf("[OK] Zone defaults: %d min each", DEFAULT_DURATION);

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

    wifiCredsLoad();   // load stored WiFi credentials (overrides config.h if set)
    connectWifi();
    connectMqtt();

    if (AP_DEFAULT) apStart();
}

void loop() {
    uint32_t now = millis();

    if (now - lastWifiCheck > WIFI_RETRY_MS) { lastWifiCheck = now; connectWifi(); }
    if (WiFi.status() == WL_CONNECTED && now - lastMqttCheck > MQTT_RETRY_MS) {
        lastMqttCheck = now; connectMqtt();
    }

    mqtt.loop();
    ntp.update();
    webServerLoop();

    // Sequencer tick
    if (programRunning) {
        if (millis() - stepStartMs >= stepDurationMs) {
            nextStep();
        } else {
            static uint32_t lastStatusMs = 0;
            if (now - lastStatusMs >= 10000) { lastStatusMs = now; publishProgramState(); }
        }
    }

    // Digital inputs
    if (INPUTS_ENABLED) {
        for (int i = 0; i < 8; i++) {
            if (INPUT_PINS[i] < 0) continue;
            bool raw = !digitalRead(INPUT_PINS[i]);
            if (raw != inputLastRaw[i]) { inputLastRaw[i] = raw; inputDebounce[i] = now; }
            if (now - inputDebounce[i] > DEBOUNCE_MS && raw != inputState[i]) {
                inputState[i] = raw;
                if (mqtt.connected())
                    mqtt.publish(inputTopic(i).c_str(), raw ? "ON" : "OFF", false);
                logf("[IN] Input %d → %s", i + 1, raw ? "ON" : "OFF");
            }
        }
    }

    // Schedule engine
    static uint32_t lastSchedCheck = 0;
    if (now - lastSchedCheck >= 1000) { lastSchedCheck = now; runSchedule(); }
}
