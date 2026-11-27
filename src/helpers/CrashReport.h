#pragma once

#include <stddef.h>
#include <stdio.h>

// Reads back the panic core dump left on flash. Observer builds only; elsewhere every call is a no-op.
#if defined(ESP_PLATFORM) && defined(WITH_MQTT_BRIDGE)

#include "CrashReportFormat.h"

namespace CrashReport {
// Call once early in setup(): reads the dump summary into a static record.
void captureAtBoot();
// Status `crash` fragment into buf (CrashReportFormat::kJsonMax); 0 when there is none.
size_t formatJson(char* buf, size_t bufsize);
// `get crash` reply.
void formatCli(char* buf, size_t bufsize);
// A status carrying the fragment was published: schedule the flash erase.
void notePublished();
// Runs a scheduled erase; call from the loop task.
void service();
// `clear crash`: erase the flash dump now and drop the record. False if the erase failed.
bool clear();
}  // namespace CrashReport

#else

namespace CrashReport {
inline void captureAtBoot() {}
inline size_t formatJson(char* buf, size_t bufsize) {
  if (buf && bufsize) buf[0] = '\0';
  return 0;
}
inline void formatCli(char* buf, size_t bufsize) {
  if (buf && bufsize) snprintf(buf, bufsize, "> none");
}
inline void notePublished() {}
inline void service() {}
inline bool clear() { return true; }
}  // namespace CrashReport

#endif
