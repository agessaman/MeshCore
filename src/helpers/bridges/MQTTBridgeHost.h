#pragma once

#include <stdint.h>

// What MQTTBridge needs to know about the node that hosts it. Keeps the bridge
// independent of any one firmware's NodePrefs, so repeater, room server and
// companion builds can all host it. Called from the bridge task (core 0).
class MQTTBridgeHost {
public:
  virtual ~MQTTBridgeHost() {}

  virtual const char* getObserverNodeName() const = 0;
  virtual void getObserverRadioParams(float& freq, float& bw, uint8_t& sf, uint8_t& cr) const = 0;
  // Reported as the status payload's "repeat" field.
  virtual bool isObserverForwarding() const = 0;
  // Reported as the status payload's "role" field: "repeater", "room_server" or "companion".
  virtual const char* getObserverRole() const = 0;
};
