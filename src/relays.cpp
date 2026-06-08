#include "relays.h"
#include <Arduino.h>

// externs
extern WiFiClient wifiClient; // declared in main.cpp

// Define relay feedback pins if not provided
#ifdef RELAY_FEEDBACK_PIN_LIST
const int relayFeedbackPins[NUM_ZONES] = RELAY_FEEDBACK_PIN_LIST;
#else
const int relayFeedbackPins[NUM_ZONES] = {-1, -1, -1, -1, -1, -1, -1, -1};
#endif

uint8_t relayBits = 0x00;

static bool hasRelayFeedback() {
    for (int i = 0; i < NUM_ZONES; i++) {
        if (relayFeedbackPins[i] >= 0) return true;
    }
    return false;
}

static bool verifyRelayOutputs() {
    bool ok = true;
    for (int i = 0; i < NUM_ZONES; i++) {
        int pin = relayFeedbackPins[i];
        if (pin < 0) continue;
        bool expected = getRelay(i);
        bool actual = (digitalRead(pin) == (RELAY_FEEDBACK_ACTIVE_HIGH ? HIGH : LOW));
        if (expected != actual) {
            logf("[HW] Relay %d mismatch expected=%s actual=%s",
                 i + 1,
                 expected ? "ON" : "OFF",
                 actual   ? "ON" : "OFF");
            ok = false;
        }
    }
    return ok;
}

static void writeRelaysOnce() {
#ifdef UNIT_TEST
    (void)PIN_LATCH;
    (void)PIN_CLOCK;
    (void)PIN_DATA;
    (void)relayBits;
#else
    digitalWrite(PIN_LATCH, LOW);
    shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, ~relayBits);
    digitalWrite(PIN_LATCH, HIGH);
#endif
}

void writeRelays() {
    writeRelaysOnce();
    if (!hasRelayFeedback()) return;

    if (verifyRelayOutputs()) return;

    logf("[HW] Relay output mismatch detected, retrying");

    const int MAX_ATTEMPTS = 3;
    const unsigned long RETRY_MS = 1000; // 1000 ms between attempts

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
        delay(RETRY_MS);
        writeRelaysOnce();
        if (verifyRelayOutputs()) {
            logf("[HW] Relay output recovered after %d attempt(s)", attempt);
            return;
        } else {
            logf("[HW] Relay output still mismatch after %d attempt(s)", attempt);
        }
    }

    // After retries exhausted, publish fatal status if possible and restart
    logf("[HW] Relay outputs failed after %d attempts — restarting", MAX_ATTEMPTS);
    if (mqtt.connected()) {
        mqtt.publish((String(MQTT_ROOT) + "/status").c_str(), "fatal:relay_stuck", true);
        mqtt.loop();
    }
    delay(200);
    ESP.restart();
}

bool getRelay(int idx) { return (relayBits >> idx) & 0x01; }

void setRelay(int idx, bool on, uint32_t pulse) {
    if (idx < 0 || idx > 7) return;
    if (on) relayBits |=  (1 << idx);
    else    relayBits &= ~(1 << idx);
    writeRelays();
}

void allRelaysOff() {
    relayBits = 0x00;
    writeRelays();
}

String zoneTopic(int idx, const char* suffix) {
    return String(MQTT_ROOT) + "/zone/" + (idx + 1) + "/" + suffix;
}

void publishRelayState(int idx) {
    if (!mqtt.connected()) return;
    bool on = getRelay(idx);
    logf("[PUB] zone/%d/state → %s  (stack hint: check log above)", idx+1, on?"ON":"OFF");
    mqtt.publish(zoneTopic(idx, "state").c_str(), on ? "ON" : "OFF", true);
}

void publishAllZoneStates() {
    logf("[PUB] publishAllZoneStates called");
    for (int i = 0; i < NUM_ZONES; i++) publishRelayState(i);
}

void relaysInit() {
#ifdef UNIT_TEST
    (void)relayFeedbackPins;
#else
    if (hasRelayFeedback()) {
        for (int i = 0; i < NUM_ZONES; i++) {
            if (relayFeedbackPins[i] >= 0) pinMode(relayFeedbackPins[i], INPUT);
        }
        logf("[OK] Relay feedback inputs configured");
    }
#endif
}
