#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  web_ui.h — Web server & OTA declarations
//  Implemented in src/web_ui.cpp
// ─────────────────────────────────────────────────────────────────────────────

// Shared constant — log ring buffer size used in main.cpp and web_ui.cpp
#define LOG_BUFFER_LINES 80

void setupWebServer();
void setupOTA();
void webServerLoop();   // call from loop() — handles client + OTA

// WiFi credential storage — implemented in main.cpp
extern char wifiSsid[64];
extern char wifiPassword[64];
extern void wifiCredsSave(const char* ssid, const char* pass);
