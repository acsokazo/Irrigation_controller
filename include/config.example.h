#pragma once
// ─────────────────────────────────────────────
//  COPY THIS FILE TO config.h AND FILL IN YOUR VALUES
//  config.h is git-ignored and will never be committed.
//
//  cp include/config.example.h include/config.h
// ─────────────────────────────────────────────

// WiFi
#define WIFI_SSID        "your_wifi_name"
#define WIFI_PASSWORD    "your_wifi_password"

// MQTT Broker
#define MQTT_BROKER      "192.168.x.x"
#define MQTT_PORT        1883
#define MQTT_USER        "your_mqtt_user"      // leave "" if not required
#define MQTT_PASSWORD    "your_mqtt_password"  // leave "" if not required
#define MQTT_CLIENT_ID   "relay_board_1"
#define MQTT_ROOT        "home/relayboard"

// NTP timezone offset in seconds from UTC
// UTC+1 (Hungary winter) = 3600
// UTC+2 (Hungary summer) = 7200
#define NTP_UTC_OFFSET   3600
