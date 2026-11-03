#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../MQTTEffectiveConfig.h"

namespace ObserverTransportPolicy {
struct Endpoint {
  MqttTransport transport = MqttTransport::Unknown;
  char host[128] = {};
  char path[160] = {};
  uint16_t port = 0;
};

inline bool parseEndpoint(const char* uri, Endpoint& endpoint) {
  endpoint = Endpoint();
  endpoint.transport = mqttTransportFromUri(uri);
  if (endpoint.transport == MqttTransport::Unknown) return false;
  const char* start = strstr(uri, "://") + 3;
  const char* end = start + strcspn(start, "/?#");
  const char* colon = static_cast<const char*>(memchr(start, ':', end - start));
  const char* host_end = colon ? colon : end;
  const size_t host_len = host_end - start;
  if (!host_len || host_len >= sizeof(endpoint.host)) return false;
  for (const char* p = start; p < host_end; ++p)
    if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' )) return false;
  memcpy(endpoint.host, start, host_len);
  endpoint.port = endpoint.transport == MqttTransport::Wss ? 443 :
                  endpoint.transport == MqttTransport::Ws ? 80 :
                  endpoint.transport == MqttTransport::Tls ? 8883 : 1883;
  if (colon) {
    if (colon + 1 == end) return false;
    uint32_t value = 0;
    for (const char* p = colon + 1; p < end; ++p) {
      if (*p < '0' || *p > '9') return false;
      value = value * 10 + (*p - '0');
      if (value > 65535) return false;
    }
    if (!value) return false;
    endpoint.port = value;
  }
  if (strchr(end, '#')) return false;
  if (endpoint.transport == MqttTransport::Tcp || endpoint.transport == MqttTransport::Tls)
    return *end == 0;
  const size_t path_len = strlen(end);
  const size_t prefix = *end != '/';
  if (path_len + prefix >= sizeof(endpoint.path)) return false;
  if (prefix) endpoint.path[0] = '/';
  memcpy(endpoint.path + prefix, end, path_len + 1);
  return true;
}

inline bool customEndpoint(const char* host, uint16_t port, Endpoint& endpoint) {
  if (!host || !*host) return false;
  if (strstr(host, "://")) {
    if (!parseEndpoint(host, endpoint)) return false;
    const char* authority = strstr(host, "://") + 3;
    const size_t length = strcspn(authority, "/?#");
    if (port && !memchr(authority, ':', length)) endpoint.port = port;
    return true;
  }
  char uri[176];
  const int length = snprintf(uri, sizeof(uri), "%s://%s:%u",
                              port == 8883 ? "mqtts" : "mqtt", host, port ? port : 1883);
  return length > 0 && static_cast<size_t>(length) < sizeof(uri) && parseEndpoint(uri, endpoint);
}

template<size_t Capacity> class ByteQueue {
  uint8_t _data[Capacity];
  size_t _head = 0, _count = 0;
public:
  void clear() { _head = _count = 0; }
  size_t size() const { return _count; }
  bool append(const uint8_t* data, size_t length) {
    if (length > Capacity - _count || (!data && length)) return false;
    for (size_t i = 0; i < length; ++i) _data[(_head + _count + i) % Capacity] = data[i];
    _count += length;
    return true;
  }
  int peek() const { return _count ? _data[_head] : -1; }
  int read() {
    if (!_count) return -1;
    int value = _data[_head];
    _head = (_head + 1) % Capacity;
    --_count;
    return value;
  }
};
}
