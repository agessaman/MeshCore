#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "MQTTPacketFilter.h"

// Append complete slot objects to an existing JSON prefix ending in '['.
// Names/states come from firmware tables, never unescaped user strings.
template<class Slot>
void appendObserverSlotStatsJson(char* buf, size_t size, int pos, const Slot* slots, size_t count) {
  if (!buf || !size) return;
  if (pos < 0 || static_cast<size_t>(pos) + 3 > size) {
    snprintf(buf, size, "{}");
    return;
  }
  bool first = true;
  for (size_t i = 0; i < count; ++i) {
    const Slot& slot = slots[i];
    if (!slot.configured) continue;
    char filter[24] = "";
    if (slot.filter_mask != MQTTPacketFilter::kAllPacketTypes) {
      snprintf(filter, sizeof(filter), ",\"filt\":%u", (unsigned)slot.filter_mask);
    }
    char entry[192];
    const int n = snprintf(entry, sizeof(entry),
        "%s{\"n\":%u,\"name\":\"%s\",\"state\":\"%s\",\"ok\":%lu,\"err\":%lu%s}",
        first ? "" : ",", (unsigned)i + 1, slot.name, slot.state,
        slot.publish_ok, slot.publish_err, filter);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(entry) ||
        static_cast<size_t>(pos) + static_cast<size_t>(n) + 3 > size) break;
    memcpy(buf + pos, entry, n);
    pos += n;
    first = false;
  }
  snprintf(buf + pos, size - pos, "]}");
}
