#include <Arduino.h>
#include <unity.h>
#include "relays.h"

void setUp() {
    relayBits = 0x00;
}

void tearDown() {
}

void test_getRelay_initially_false() {
    relayBits = 0x00;
    TEST_ASSERT_FALSE(getRelay(0));
    TEST_ASSERT_FALSE(getRelay(7));
}

void test_relay_bits_set_and_clear() {
    relayBits = 0x00;
    relayBits |= (1 << 2);
    TEST_ASSERT_TRUE(getRelay(2));
    relayBits &= ~(1 << 2);
    TEST_ASSERT_FALSE(getRelay(2));
}

void test_setRelay_updates_bitmask() {
    relayBits = 0x00;
    setRelay(2, true);
    TEST_ASSERT_TRUE(getRelay(2));
    setRelay(2, false);
    TEST_ASSERT_FALSE(getRelay(2));
}

void test_allRelaysOff_resets_bits() {
    relayBits = 0xFF;
    allRelaysOff();
    TEST_ASSERT_EQUAL_UINT8(0x00, relayBits);
}

void test_zoneTopic_formats_mqtt_topic() {
    String topic = zoneTopic(5, "state");
    TEST_ASSERT_EQUAL_STRING("irrigation/controller/zone/6/state", topic.c_str());
}

void setup() {
    delay(2000);
    UNITY_BEGIN();
    RUN_TEST(test_getRelay_initially_false);
    RUN_TEST(test_relay_bits_set_and_clear);
    RUN_TEST(test_allRelaysOff_resets_bits);
    RUN_TEST(test_zoneTopic_formats_mqtt_topic);
    UNITY_END();
}

void loop() {
}
