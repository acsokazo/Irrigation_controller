#pragma once
#include <Arduino.h>
#include <PubSubClient.h>
#include "config.h"

// Hardware pins for 74HC595
#define PIN_LATCH    25
#define PIN_CLOCK    26
#define PIN_DATA     33
#define PIN_OE       13

// Relay bits storage
extern uint8_t relayBits;

// Externals from main
extern PubSubClient mqtt;
extern void logf(const char* fmt, ...);

// Optional relay feedback pins
#ifdef RELAY_FEEDBACK_PIN_LIST
extern const int relayFeedbackPins[NUM_ZONES];
#else
extern const int relayFeedbackPins[NUM_ZONES];
#endif

#ifndef RELAY_FEEDBACK_ACTIVE_HIGH
#define RELAY_FEEDBACK_ACTIVE_HIGH 1
#endif

// Initialization for relay subsystem (configure feedback pins)
void relaysInit();

// Low-level write
void writeRelays();

// Relay helpers
bool getRelay(int idx);
void setRelay(int idx, bool on, uint32_t pulse = 0);
void allRelaysOff();

// MQTT/state helpers
String zoneTopic(int idx, const char* suffix);
void publishRelayState(int idx);
void publishAllZoneStates();
