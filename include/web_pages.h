#pragma once
#include <Arduino.h>
#include <pgmspace.h>

// ─────────────────────────────────────────────────────────────────────────────
//  web_pages.h — HTML page content declarations
//  Defined in src/web_pages.cpp
//
//  PAGE_DASHBOARD is gzip compressed — serve with Content-Encoding: gzip
//  PAGE_OTA_* are plain strings — serve normally
// ─────────────────────────────────────────────────────────────────────────────

// Main dashboard — gzip compressed, use send_P with Content-Encoding: gzip
extern const uint8_t  PAGE_DASHBOARD_GZ[];
extern const uint32_t PAGE_DASHBOARD_LEN;

// OTA pages — plain PROGMEM strings
extern const char PAGE_OTA_GET[];
extern const char PAGE_OTA_OK[];
extern const char PAGE_OTA_FAIL[];

// WiFi settings page — served at GET /wifi
extern const char PAGE_WIFI[];
