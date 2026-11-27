#pragma once

// Fork defaults for upstream compile-time knobs on observer (WITH_MQTT_BRIDGE)
// builds. Included via MQTTBridge.h, ahead of the upstream defaults in
// simple_repeater/MyMesh.h; any -D in platformio build_flags still wins.

#if defined(WITH_MQTT_BRIDGE) && !defined(BOARD_HAS_PSRAM)
  // Repeater history costs 5 * slots * 4 bytes of internal heap. Keep 24 h of
  // coverage at 15-minute resolution: 1,920 B instead of upstream's 5,760 B.
  #ifndef REPEATER_HISTORY_SLOTS
    #define REPEATER_HISTORY_SLOTS 96
  #endif
  #ifndef REPEATER_HISTORY_INTERVAL_SECS
    #define REPEATER_HISTORY_INTERVAL_SECS 900
  #endif
#endif
