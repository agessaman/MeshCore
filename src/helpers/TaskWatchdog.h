#pragma once

// Reboots the node if the main loop stops running.
//
// The ESP32 task watchdog is already compiled in (CONFIG_ESP_TASK_WDT=y, 5s, panic on
// expiry), but it only watches the core 0 idle task —
// CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 is unset and the Arduino loop task is pinned to
// core 1 (ARDUINO_RUNNING_CORE=1). A loop that never returns is therefore invisible to it,
// which is how a blocking USB CDC write can silence a node indefinitely with no crash and
// no reboot (see SerialPacketLog.h). Subscribing the loop task itself closes that gap: a
// wedge becomes a reboot and a fresh boot banner instead of an open-ended silence.
//
// Opt in per build with -D WITH_TASK_WATCHDOG_SECS=<seconds>. Pick a timeout well above the
// longest legitimate loop stall (MQTT teardown, flash writes, board.sleep()); note that the
// timeout is global to the watchdog, so this also relaxes it for the core 0 idle task.

#if defined(WITH_TASK_WATCHDOG_SECS) && defined(ESP32_PLATFORM)

#include <esp_task_wdt.h>

inline void taskWatchdogBegin() {
  esp_task_wdt_init(WITH_TASK_WATCHDOG_SECS, true);   // panic => reboot
  esp_task_wdt_add(NULL);                             // watch the calling (loop) task
}

inline void taskWatchdogFeed() {
  esp_task_wdt_reset();
}

#else

inline void taskWatchdogBegin() { }
inline void taskWatchdogFeed() { }

#endif
