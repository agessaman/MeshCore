#include "NodeDiagCLI.h"

#include <Arduino.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#else
#include <malloc.h>  // mallinfo() on nRF52/RP2040/STM32
#endif

void NodeDiagHost::formatDiagRadio(char* reply) {
  strcpy(reply, "Not supported");
}

namespace NodeDiagCLI {

static void formatMemory(char* reply, int queue_size) {
#ifdef ESP_PLATFORM
  snprintf(reply, 161, "Free: %d, Min: %d, Max: %d, Queue: %d, IntFree: %d, IntMax: %d, PSRAM: %d/%d",
           (int)ESP.getFreeHeap(), (int)ESP.getMinFreeHeap(), (int)ESP.getMaxAllocHeap(),
           queue_size,
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#else
  // newlib arena stats — the portable equivalent on nRF52/RP2040. There is
  // no min-ever-free or largest-free-block counterpart, so those fields are
  // left out rather than filled with numbers that mean something different.
  // Frags is the free-chunk count, the closest available fragmentation hint.
  struct mallinfo mi = mallinfo();
  snprintf(reply, 161, "Free: %d, Used: %d, Arena: %d, Frags: %d, Queue: %d",
           (int)mi.fordblks, (int)mi.uordblks, (int)mi.arena, (int)mi.ordblks,
           queue_size);
#endif
}

bool handleCommand(uint32_t sender_timestamp, const char* command, char* reply, NodeDiagHost& host) {
  if (memcmp(command, "memory", 6) == 0) {
    formatMemory(reply, host.getDiagQueueSize());
    return true;
  }
  if (sender_timestamp == 0 && memcmp(command, "stats-radio-diag", 16) == 0 &&
      (command[16] == 0 || command[16] == ' ')) {
    host.formatDiagRadio(reply);
    return true;
  }
  return false;
}

}  // namespace NodeDiagCLI
