#include "mqtt_manager.h"
#include "config.h"
#include <Arduino.h>
#include <PubSubClient.h>

extern PubSubClient mqtt;
extern void publishProgramState();
extern void publishAllZoneStates();
extern void publishApState();
extern void publishSavedSequence();
extern void publishScheduleState();
extern void logf(const char* fmt, ...);

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

        // Clear any retained messages on command topics — these should never be retained
        // because the broker replays them on every reconnect causing duplicate execution
        const char* clearTopics[] = {
            "/zone/1/set", "/zone/2/set", "/zone/3/set", "/zone/4/set",
            "/zone/5/set", "/zone/6/set", "/zone/7/set", "/zone/8/set",
            "/program/start", "/program/stop", "/program/set",
            "/ap/set"
        };
        for (const char* t : clearTopics) {
            mqtt.publish((String(MQTT_ROOT) + t).c_str(), "", true);  // empty retained = delete
        }

        mqtt.subscribe((String(MQTT_ROOT) + "/reboot").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/zone/+/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/zone/+/config").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/program/start").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/program/stop").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/program/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/sequence/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/sequence/delay/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/schedule/set").c_str());
        mqtt.subscribe((String(MQTT_ROOT) + "/ap/set").c_str());
        publishAllZoneStates();
        publishApState();
        publishProgramState();
        publishSavedSequence();
        publishScheduleState();
        mqtt.publish((String(MQTT_ROOT) + "/sequence/delay/state").c_str(),
                     String(0).c_str(), true);
    } else {
        logf("[MQTT] FAILED rc=%d", mqtt.state());
    }
}
