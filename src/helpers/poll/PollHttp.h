#pragma once

// The small slice of HTTP/1.1 the CoreScope control client speaks: base URL
// split, status line, the three headers that frame a body, and chunk sizes.
// Pure: no sockets, no heap.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

}  // namespace poll
