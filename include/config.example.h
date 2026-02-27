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

// OTA (Over-The-Air) update credentials
// Used for both ArduinoOTA (PlatformIO WiFi upload) and web /update page
// Set OTA_PASSWORD to "" to disable password protection (not recommended)
#define OTA_HOSTNAME  "relay-board-1"     // mDNS hostname: relay-board-1.local
#define OTA_USERNAME  "admin"             // web /update page only (ArduinoOTA has no username)
#define OTA_PASSWORD  "your_ota_password" // shared by both ArduinoOTA and web upload

// Access Point (AP) mode
// Toggle via MQTT: home/relayboard/ap/set  → "ON" | "OFF"
// State published: home/relayboard/ap/state → "ON" | "OFF"
// AP IP will always be 192.168.4.1
#define AP_SSID      "RelayBoard"        // WiFi network name broadcast by the board
#define AP_PASSWORD  "your_ap_password"  // min 8 characters, leave "" for open network
#define AP_CHANNEL   1                   // WiFi channel (1-13)
#define AP_DEFAULT   false               // true = AP on at boot, false = off at boot

// NTP timezone offset in seconds from UTC
// UTC+1 (Hungary winter) = 3600
// UTC+2 (Hungary summer) = 7200
#define NTP_UTC_OFFSET   3600
