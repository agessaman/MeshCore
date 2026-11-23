#pragma once

#include <stdint.h>

// Fork-added diagnostic commands shared by every host firmware (repeater, room
// server, companion). Kept out of the upstream-tracked CommonCLI.cpp so each
// host delegates with one call.
class NodeDiagHost {
public:
  virtual ~NodeDiagHost() {}

  // Reported as `Queue` by the memory command (the MQTT bridge queue, if any).
  virtual int getDiagQueueSize() { return 0; }
  // Fill `reply` for stats-radio-diag.
  virtual void formatDiagRadio(char* reply);
};

namespace NodeDiagCLI {

// Handles `memory` and `stats-radio-diag`. Returns true when `command` was one
// of them. `reply` must hold at least 161 bytes. stats-radio-diag answers only
// local consoles (sender_timestamp == 0), as the other stats-* commands do.
bool handleCommand(uint32_t sender_timestamp, const char* command, char* reply, NodeDiagHost& host);

}  // namespace NodeDiagCLI
