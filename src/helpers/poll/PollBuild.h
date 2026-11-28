#pragma once

// Build gate for the repeater poller. Derived like WITH_MQTT_NEIGHBORS: it reads
// the neighbor table, and its RAM is budgeted for PSRAM boards. Non-PSRAM boards
// opt in with REPEATER_POLL_WITHOUT_PSRAM; any build opts out with
// REPEATER_POLL_DISABLE.
#if defined(ESP32) && defined(WITH_MQTT_BRIDGE) && defined(MAX_NEIGHBOURS) && MAX_NEIGHBOURS > 0 && \
    (defined(BOARD_HAS_PSRAM) || defined(REPEATER_POLL_WITHOUT_PSRAM)) && !defined(REPEATER_POLL_DISABLE)
#define WITH_REPEATER_POLL 1
#endif
