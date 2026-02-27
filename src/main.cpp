/*
 * ESP32_Relay_X8_Modbus  –  WiFi / MQTT Firmware
 * Board model : 303E32DC812
 * Framework   : Arduino / PlatformIO
 *
 * CONFIRMED HARDWARE (from pin scanner):
 *   74HC595 shift register:
 *     LATCH = GPIO25, CLOCK = GPIO26, DATA = GPIO33, OE = GPIO13
 *   Bit order: bit0=relay1 … bit6=relay7, bit1=relay8 (shared with relay2)
 *
 * Setup: copy include/config.example.h to include/config.h and fill in values.
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "config.h"   // ← git-ignored, never committed

// ─────────────────────────────────────────────
//  Hardware — CONFIRMED by pin scanner
// ─────────────────────────────────────────────
#define PIN_LATCH    25
#define PIN_CLOCK    26
#define PIN_DATA     33
#define PIN_OE       13

// Digital inputs (IN1–IN8, active-LOW, direct GPIO)
const int8_t INPUT_PINS[8] = {36, 39, 34, 35, 4, 16, 17, 5};
#define INPUTS_ENABLED true

// ─────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────
#define MAX_SCHEDULES    16
#define NTP_SERVER       "pool.ntp.org"
#define DEFAULT_PULSE_MS 0

// ─────────────────────────────────────────────────────────────────────────────
//  Globals
// ─────────────────────────────────────────────────────────────────────────────

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WiFiUDP      ntpUDP;
NTPClient    ntp(ntpUDP, NTP_SERVER, NTP_UTC_OFFSET, 60000);

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

// ─────────────────────────────────────────────────────────────────────────────
//  Shift register
// ─────────────────────────────────────────────────────────────────────────────

void writeRelays() {
    digitalWrite(PIN_LATCH, LOW);
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, ~relayBits);
    digitalWrite(PIN_LATCH, HIGH);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
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
    Serial.printf("[RELAY] %d → %s  (reg=0x%02X)\n", idx+1, on?"ON":"OFF", relayBits);
    if (on && pulse > 0) {
        pulseMs[idx] = pulse; pulseStart[idx] = millis(); pulseActive[idx] = true;
    } else if (!on) {
        pulseActive[idx] = false;
    }
}

void toggleRelay(int idx, uint32_t pulse = 0) { setRelay(idx, !getRelay(idx), pulse); }

// ─────────────────────────────────────────────────────────────────────────────
//  MQTT callback
// ─────────────────────────────────────────────────────────────────────────────

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String t(topic);
    String msg;
    for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
    msg.trim();
    Serial.printf("[MQTT IN] %s → %s\n", topic, msg.c_str());

    if (t == String(MQTT_ROOT) + "/schedule/clear") {
        scheduleCount = 0; return;
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
        Serial.printf("[SCHED] Loaded %d entries\n", scheduleCount);
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
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
        delay(250); Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
        ntp.begin();
    } else {
        Serial.println("\n[WiFi] Failed – retrying");
    }
}

void connectMqtt() {
    if (mqtt.connected()) return;
    Serial.printf("[MQTT] Connecting to %s:%d … ", MQTT_BROKER, MQTT_PORT);
    String lwt = String(MQTT_ROOT) + "/status";
    bool ok = (strlen(MQTT_USER) > 0)
        ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD, lwt.c_str(), 0, true, "offline")
        : mqtt.connect(MQTT_CLIENT_ID, nullptr, nullptr, lwt.c_str(), 0, true, "offline");
    if (ok) {
        Serial.println("OK");
        mqtt.publish(lwt.c_str(), "online", true);
        mqtt.subscribe((String(MQTT_ROOT) + "/relay/+/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/relay/all/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/schedule/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/schedule/clear").c_str());
        for (int i = 0; i < 8; i++) publishRelayState(i);
    } else {
        Serial.printf("FAIL rc=%d\n", mqtt.state());
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
        Serial.printf("[SCHED] Fired entry %d → relay %d\n", i, s.relay+1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup & Loop
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    Serial.println("\n[BOOT] ESP32_Relay_X8 firmware starting");

    pinMode(PIN_LATCH, OUTPUT);
    pinMode(PIN_CLOCK, OUTPUT);
    pinMode(PIN_DATA,  OUTPUT);
    pinMode(PIN_OE,    OUTPUT);
    digitalWrite(PIN_LATCH, HIGH);
    digitalWrite(PIN_CLOCK, LOW);
    digitalWrite(PIN_OE,    LOW);
    relayBits = 0x00;
    writeRelays();
    Serial.println("[OK] All relays OFF");

    if (INPUTS_ENABLED) {
        for (int i = 0; i < 8; i++) {
            if (INPUT_PINS[i] >= 0) {
                pinMode(INPUT_PINS[i], INPUT_PULLUP);
                inputLastRaw[i] = !digitalRead(INPUT_PINS[i]);
                inputState[i]   = inputLastRaw[i];
            }
        }
    }

    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(1024);
    connectWifi();
    connectMqtt();
}

void loop() {
    uint32_t now = millis();

    if (now - lastWifiCheck > WIFI_RETRY_MS) { lastWifiCheck = now; connectWifi(); }
    if (WiFi.status() == WL_CONNECTED && now - lastMqttCheck > MQTT_RETRY_MS) {
        lastMqttCheck = now; connectMqtt();
    }
    mqtt.loop();
    ntp.update();

    for (int i = 0; i < 8; i++) {
        if (pulseActive[i] && (now - pulseStart[i] >= pulseMs[i])) {
            setRelay(i, false); pulseActive[i] = false;
        }
    }

    if (INPUTS_ENABLED) {
        for (int i = 0; i < 8; i++) {
            if (INPUT_PINS[i] < 0) continue;
            bool raw = !digitalRead(INPUT_PINS[i]);
            if (raw != inputLastRaw[i]) { inputLastRaw[i] = raw; inputDebounce[i] = now; }
            if (now - inputDebounce[i] > DEBOUNCE_MS && raw != inputState[i]) {
                inputState[i] = raw;
                if (mqtt.connected())
                    mqtt.publish(inputTopic(i).c_str(), raw ? "ON" : "OFF", false);
                Serial.printf("[IN] Input %d → %s\n", i+1, raw?"ON":"OFF");
            }
        }
    }

    static uint32_t lastSchedCheck = 0;
    if (now - lastSchedCheck >= 1000) { lastSchedCheck = now; runSchedules(); }
}
