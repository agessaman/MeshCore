#pragma once
#include <cstdint>
#include <string>
class IPAddress {
  uint8_t bytes[4]{};
public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : bytes{a,b,c,d} {}
  bool operator==(const IPAddress& other) const {
    for (int i = 0; i < 4; ++i) if (bytes[i] != other.bytes[i]) return false;
    return true;
  }
  bool operator!=(const IPAddress& other) const { return !(*this == other); }
  std::string toString() const {
    return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
           std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
  }
};
