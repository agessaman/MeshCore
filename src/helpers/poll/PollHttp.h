#pragma once

// The small slice of HTTP/1.1 the CoreScope control client speaks: base URL
// split, status line, the three headers that frame a body, and chunk sizes.
// Pure: no sockets, no heap.

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "PollControlTypes.h"

namespace poll {

static const uint8_t kHttpHostMax = 90;
static const uint8_t kHttpPrefixMax = 90;

static const size_t kHttpFramingMax = 8192;

// One bounded line; all headers, chunk framing and trailers share the byte budget.
// Returns 1, a reader failure (0/-1/-2), or -3 for oversized framing.
template <typename ReadByte>
static inline int readHttpLine(ReadByte read_byte, char* line, size_t cap, size_t* total) {
  size_t n = 0;
  for (;;) {
    if (*total >= kHttpFramingMax) return -3;
    uint8_t b = 0;
    const int r = read_byte(&b);
    if (r <= 0) return r;
    (*total)++;
    if (b == '\n') break;
    if (n + 1 >= cap) return -3;
    line[n++] = (char)b;
  }
  if (n > 0 && line[n - 1] == '\r') n--;
  line[n] = '\0';
  return 1;
}

// Nonblocking writes yield when full and recheck cancellation and budget each time.
// Returns 1, 0 for timeout, -2 for cancellation, or -1 for transport failure.
template <typename Ready, typename WriteSome, typename Wait>
static inline int writeHttpBytes(const uint8_t* data, size_t len, Ready ready,
                                 WriteSome write_some, Wait wait) {
  size_t sent = 0;
  while (sent < len) {
    const int r = ready();
    if (r <= 0) return r;
    const int n = write_some(data + sent, len - sent);
    if (n < 0 || (size_t)n > len - sent) return -1;
    sent += (size_t)n;
    if (n == 0) wait();
  }
  return ready();
}

struct HttpBase {
  bool     tls;
  uint16_t port;
  char     host[kHttpHostMax];
  char     prefix[kHttpPrefixMax];   // path before /api, "" or "/x/y"
};

static inline bool httpLower(char c, char want) {
  if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return c == want;
}

// Case-insensitive prefix match against a lowercase literal.
static inline bool httpStartsWith(const char* s, const char* lower) {
  for (; *lower; s++, lower++) {
    if (!httpLower(*s, *lower)) return false;
  }
  return true;
}

// http[s]://host[:port][/prefix]; no userinfo, query, fragment or IPv6 literal.
static inline bool parseHttpBase(const char* url, HttpBase& out) {
  memset(&out, 0, sizeof(out));
  if (!url) return false;
  const char* s = url;
  if (httpStartsWith(s, "https://")) {
    out.tls = true;
    out.port = 443;
    s += 8;
  } else if (httpStartsWith(s, "http://")) {
    out.port = 80;
    s += 7;
  } else {
    return false;
  }
  size_t h = 0;
  while (*s && *s != ':' && *s != '/') {
    const char c = *s;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '.' || c == '_';
    if (!ok || h + 1 >= sizeof(out.host)) return false;
    out.host[h++] = c;
    s++;
  }
  if (h == 0) return false;
  if (*s == ':') {
    s++;
    uint32_t port = 0;
    if (*s < '0' || *s > '9') return false;
    while (*s >= '0' && *s <= '9') {
      port = port * 10 + (uint32_t)(*s - '0');
      if (port > 65535) return false;
      s++;
    }
    if (port == 0) return false;
    out.port = (uint16_t)port;
  }
  if (*s != '\0' && *s != '/') return false;
  size_t p = 0;
  while (*s) {
    const char c = *s;
    if (c == '?' || c == '#' || c == ' ' || c == '@' || (unsigned char)c < 0x21) return false;
    if (p + 1 >= sizeof(out.prefix)) return false;
    out.prefix[p++] = c;
    s++;
  }
  while (p > 0 && out.prefix[p - 1] == '/') out.prefix[--p] = '\0';
  return true;
}

// "HTTP/1.x NNN ..." -> NNN, else -1. *http10 is set for an HTTP/1.0 reply.
static inline int parseHttpStatusLine(const char* line, bool* http10) {
  if (!line || !httpStartsWith(line, "http/1.")) return -1;
  if (http10) *http10 = line[7] == '0';
  const char* s = line + 8;
  if (*s != ' ') return -1;
  while (*s == ' ') s++;
  int v = 0;
  for (int i = 0; i < 3; i++) {
    if (s[i] < '0' || s[i] > '9') return -1;
    v = v * 10 + (s[i] - '0');
  }
  if (s[3] != '\0' && s[3] != ' ' && s[3] != '\r') return -1;
  if (v < 100) return -1;
  return v;
}

struct HttpFraming {
  int32_t content_length;   // -1 = not given
  bool    chunked;
  bool    close;            // the server will close after this reply
  bool    keep_alive;       // explicit keep-alive (matters for HTTP/1.0)
};

static inline void httpFramingInit(HttpFraming& f) {
  f.content_length = -1;
  f.chunked = false;
  f.close = false;
  f.keep_alive = false;
}

static inline const char* httpHeaderValue(const char* line, const char* lower_name) {
  if (!httpStartsWith(line, lower_name)) return nullptr;
  const char* s = line + strlen(lower_name);
  if (*s != ':') return nullptr;
  s++;
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

static inline bool httpValueHasToken(const char* v, const char* lower_token) {
  const size_t n = strlen(lower_token);
  while (*v) {
    while (*v == ' ' || *v == '\t' || *v == ',') v++;
    if (httpStartsWith(v, lower_token)) {
      const char e = v[n];
      if (e == '\0' || e == ',' || e == ' ' || e == '\t' || e == ';' || e == '\r') return true;
    }
    while (*v && *v != ',') v++;
  }
  return false;
}

// One header line without its CRLF. False: a malformed Content-Length.
static inline bool applyHttpHeader(const char* line, HttpFraming& f) {
  const char* v = httpHeaderValue(line, "content-length");
  if (v) {
    if (*v < '0' || *v > '9') return false;
    int64_t n = 0;
    while (*v >= '0' && *v <= '9') {
      n = n * 10 + (*v - '0');
      if (n > 0x7fffffff) return false;
      v++;
    }
    while (*v == ' ' || *v == '\t' || *v == '\r') v++;
    if (*v != '\0') return false;
    if (f.content_length >= 0 && f.content_length != (int32_t)n) return false;
    f.content_length = (int32_t)n;
    return true;
  }
  v = httpHeaderValue(line, "transfer-encoding");
  if (v) {
    if (httpValueHasToken(v, "chunked")) f.chunked = true;
    return true;
  }
  v = httpHeaderValue(line, "connection");
  if (v) {
    if (httpValueHasToken(v, "close")) f.close = true;
    if (httpValueHasToken(v, "keep-alive")) f.keep_alive = true;
  }
  return true;
}

// Hex chunk size, extensions after ';' ignored. -1 on a malformed line.
static inline int32_t parseHttpChunkSize(const char* line) {
  if (!line) return -1;
  int64_t n = 0;
  int digits = 0;
  for (const char* s = line; *s; s++) {
    const char c = *s;
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else if (c == ';' || c == ' ' || c == '\t' || c == '\r') break;
    else return -1;
    n = n * 16 + d;
    if (n > 0x7fffffff) return -1;
    digits++;
  }
  return digits ? (int32_t)n : -1;
}

// The label the analyzer sees on the owners and policy GETs.
static const char kClientName[] = "meshcore.observer_fw";
static const uint8_t kClientVersionMax = 24;   // the size of ControlRequest::client_version

// A URL-safe version label: [A-Za-z0-9._-] kept, every other byte '_'.
static inline void sanitizeClientVersion(const char* in, char* out, size_t cap) {
  if (!out || cap == 0) return;
  size_t n = 0;
  if (in) {
    for (const char* s = in; *s && n + 1 < cap; s++) {
      const char c = *s;
      const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
      out[n++] = keep ? c : '_';
    }
  }
  out[n] = '\0';
}

// A path under construction; ok turns false once the next byte would not fit.
struct HttpPath {
  char*  out;
  size_t cap;
  size_t len;
  bool   ok;
};

static inline HttpPath httpPath(char* out, size_t cap) {
  HttpPath p = {out, cap, 0, out && cap > 0};
  if (p.ok) out[0] = '\0';
  return p;
}

static inline void httpPathAdd(HttpPath& p, const char* text) {
  if (!p.ok) return;
  for (; *text; text++) {
    if (p.len + 1 >= p.cap) {
      p.ok = false;
      p.len = 0;
      p.out[0] = '\0';
      return;
    }
    p.out[p.len++] = *text;
  }
  p.out[p.len] = '\0';
}

// client= always; client_version= only when the caller supplied one.
static inline void appendClientQuery(HttpPath& p, const char* client_version) {
  httpPathAdd(p, "&client=");
  httpPathAdd(p, kClientName);
  if (client_version && *client_version) {
    httpPathAdd(p, "&client_version=");
    httpPathAdd(p, client_version);
  }
}

// The owners GET path in CoreScope's parameter order, with the client identity last.
static inline size_t buildOwnersPath(const char* observer_hex, const char* plugin_version,
                                     const char* client_version, char* out, size_t cap) {
  char clean[kClientVersionMax];
  sanitizeClientVersion(client_version, clean, sizeof(clean));
  HttpPath p = httpPath(out, cap);
  httpPathAdd(p, "/api/guest-polling/owners?observer=");
  if (observer_hex) httpPathAdd(p, observer_hex);
  httpPathAdd(p, "&plugin_version=");
  if (plugin_version) httpPathAdd(p, plugin_version);
  appendClientQuery(p, clean);
  return p.ok ? p.len : 0;
}

// The policy GET path in CoreScope's parameter order, with the client identity last.
static inline size_t buildPolicyPath(const char* observer_hex, const char* plugin_version,
                                     const char* client_version, char* out, size_t cap) {
  char clean[kClientVersionMax];
  sanitizeClientVersion(client_version, clean, sizeof(clean));
  HttpPath p = httpPath(out, cap);
  httpPathAdd(p, "/api/guest-polling/policy?plugin_version=");
  if (plugin_version) httpPathAdd(p, plugin_version);
  httpPathAdd(p, "&observer=");
  if (observer_hex) httpPathAdd(p, observer_hex);
  appendClientQuery(p, clean);
  return p.ok ? p.len : 0;
}

// The control client's path buffer.
static const size_t kHttpPathCap = 200;

static const size_t kHttpHostTextMax = kHttpHostMax - 1;
static const size_t kHttpPrefixTextMax = kHttpPrefixMax - 1;
static const size_t kHttpPathTextMax = kHttpPathCap - 1;
static const size_t kHttpPortTextMax = 5;                 // 65535
static const size_t kHttpMethodTextMax = sizeof("POST") - 1;
static const size_t kHttpAgentVersionMax = sizeof(kPluginVersion) - 1;
static const size_t kHttpContentLengthDigits = 10;        // "%u" of an unsigned body length

// Space after the method, then the POST literals, including the colon before the port.
static const size_t kHttpRequestFixedPost =
    sizeof(" " " HTTP/1.1\r\nHost: :\r\nUser-Agent: meshcore-observer/\r\n"
           "Accept: application/json\r\nConnection: keep-alive\r\n"
           "Content-Type: application/json\r\nContent-Length: \r\n\r\n") - 1;

// Worst-case POST header length, not counting the terminator.
static const size_t kHttpRequestHeaderMax =
    kHttpMethodTextMax + kHttpPrefixTextMax + kHttpPathTextMax + kHttpHostTextMax +
    kHttpPortTextMax + kHttpAgentVersionMax + kHttpContentLengthDigits + kHttpRequestFixedPost;

static_assert(kHttpHostTextMax == 89, "host text maximum");
static_assert(kHttpPrefixTextMax == 89, "prefix text maximum");
static_assert(kHttpPathTextMax == 199, "path text maximum");
static_assert(kHttpAgentVersionMax == 6, "plugin version length");
static_assert(kHttpRequestFixedPost == 155, "fixed POST header literals");
static_assert(kHttpRequestHeaderMax == 4 + 89 + 199 + 89 + 5 + 6 + 10 + 155, "header budget");
static_assert(kHttpRequestHeaderMax > 352, "a long accepted URL must fit");

// Request header, without the body. 0 means it did not fit; out is then empty.
static inline size_t formatHttpRequestHeader(char* out, size_t cap, const char* method,
                                             const char* prefix, const char* path, const char* host,
                                             uint16_t port, bool default_port,
                                             const char* agent_version, const char* body,
                                             size_t body_len) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!method || !path || !host || !agent_version) return 0;
  if (!prefix) prefix = "";
  int n;
  if (body) {
    if (default_port) {
      n = snprintf(out, cap,
                   "%s %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: meshcore-observer/%s\r\n"
                   "Accept: application/json\r\nConnection: keep-alive\r\n"
                   "Content-Type: application/json\r\nContent-Length: %u\r\n\r\n",
                   method, prefix, path, host, agent_version, (unsigned)body_len);
    } else {
      n = snprintf(out, cap,
                   "%s %s%s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: meshcore-observer/%s\r\n"
                   "Accept: application/json\r\nConnection: keep-alive\r\n"
                   "Content-Type: application/json\r\nContent-Length: %u\r\n\r\n",
                   method, prefix, path, host, (unsigned)port, agent_version, (unsigned)body_len);
    }
  } else if (default_port) {
    n = snprintf(out, cap,
                 "%s %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: meshcore-observer/%s\r\n"
                 "Accept: application/json\r\nConnection: keep-alive\r\n\r\n",
                 method, prefix, path, host, agent_version);
  } else {
    n = snprintf(out, cap,
                 "%s %s%s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: meshcore-observer/%s\r\n"
                 "Accept: application/json\r\nConnection: keep-alive\r\n\r\n",
                 method, prefix, path, host, (unsigned)port, agent_version);
  }
  if (n <= 0 || (size_t)n >= cap) {
    out[0] = '\0';
    return 0;
  }
  return (size_t)n;
}

}  // namespace poll
