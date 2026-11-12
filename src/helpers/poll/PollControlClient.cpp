#include "PollControlClient.h"

#if defined(WITH_REPEATER_POLL)

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <new>

#include <ed_25519.h>
#include <secure_zero.h>

#include "../NetworkLink.h"
#include "PollControlJson.h"
#include "PollHttp.h"

#if defined(MQTT_DEBUG)
  #define POLL_CTL_LOG(F, ...) do { if (Serial.availableForWrite() > 0) { Serial.printf("POLL: " F "\n", ##__VA_ARGS__); } } while (0)
#else
  #define POLL_CTL_LOG(...) do {} while (0)
#endif

namespace poll {
namespace {

void* bigAlloc(size_t n) {
#if defined(BOARD_HAS_PSRAM)
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (p) return p;
#endif
  return heap_caps_malloc(n, MALLOC_CAP_8BIT);
}

// Allocated on the first session and kept: a fresh 6 KB block per session
// would fragment the heap the TLS sessions need.
char* s_body = nullptr;

// Zeroes every block on release, so key text the JSON pool copied does not
// linger in freed memory. An 8-byte header holds the block size.
struct WipingAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    uint8_t* base = static_cast<uint8_t*>(bigAlloc(size + 8));
    if (!base) return nullptr;
    memcpy(base, &size, sizeof(size));
    return base + 8;
  }
  void deallocate(void* ptr) override {
    if (!ptr) return;
    uint8_t* base = static_cast<uint8_t*>(ptr) - 8;
    size_t size = 0;
    memcpy(&size, base, sizeof(size));
    ed25519_secure_zero(ptr, size);
    heap_caps_free(base);
  }
  void* reallocate(void* ptr, size_t new_size) override {
    if (!ptr) return allocate(new_size);
    size_t old_size = 0;
    memcpy(&old_size, static_cast<uint8_t*>(ptr) - 8, sizeof(old_size));
    void* p = allocate(new_size);
    if (!p) return nullptr;
    memcpy(p, ptr, old_size < new_size ? old_size : new_size);
    deallocate(ptr);
    return p;
  }
};

// The CA bundle is loaded once, globally, by the bridge; reloading it here
// would free the table under a live MQTT handshake. Only attach to it.
class BundleTlsClient : public WiFiClientSecure {
 public:
  void useLoadedBundle() { _use_ca_bundle = true; }
  // Set the fields directly: setTimeout() also pokes the socket, which after a
  // failed connect is a stale descriptor.
  void setLimits(int connect_ms, unsigned long handshake_ms) {
    _timeout = connect_ms;
    sslclient->handshake_timeout = handshake_ms;
  }
};

bool expired(uint32_t deadline) { return (int32_t)(millis() - deadline) >= 0; }

uint32_t remainingMs(uint32_t deadline) {
  const int32_t left = (int32_t)(deadline - millis());
  return left > 0 ? (uint32_t)left : 0;
}

bool aborted(const ControlEnv& env) {
  if (env.abort && env.abort->load(std::memory_order_acquire)) return true;
  return !activeNetworkLink().isConnected();
}

int readReady(uint32_t deadline, const ControlEnv& env) {
  if (aborted(env)) return -2;
  return expired(deadline) ? 0 : 1;
}

struct Conn {
  HttpBase base;
  bool tls_ready = false;
  WiFiClient* plain = nullptr;
  BundleTlsClient* tls = nullptr;
  Client* c = nullptr;
  bool reusable = false;   // the last reply left the connection open and framed
  int16_t failed = 0;      // a connect or handshake failed; the session makes no more
  uint8_t rbuf[256];
  size_t rpos = 0;
  size_t rlen = 0;

  ~Conn() { destroy(); }

  void close() {
    if (c) c->stop();
    ed25519_secure_zero(rbuf, sizeof(rbuf));
    reusable = false;
    rpos = rlen = 0;
  }
  void destroy() {
    close();
    delete plain;
    delete tls;
    plain = nullptr;
    tls = nullptr;
    c = nullptr;
  }
};

// > 0 bytes read, 0 timeout, -1 peer closed, -2 link down or stop requested.
int readSome(Conn& k, uint8_t* dst, size_t want, uint32_t deadline, const ControlEnv& env) {
  for (;;) {
    const int ready = readReady(deadline, env);
    if (ready <= 0) return ready;
    const int avail = k.c->available();
    if (avail > 0) {
      const int n = k.c->read(dst, (size_t)avail < want ? (size_t)avail : want);
      if (n > 0) return n;
    } else if (!k.c->connected()) {
      return -1;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

int readByte(Conn& k, uint8_t* out, uint32_t deadline, const ControlEnv& env) {
  const int ready = readReady(deadline, env);
  if (ready <= 0) return ready;
  if (k.rpos == k.rlen) {
    const int n = readSome(k, k.rbuf, sizeof(k.rbuf), deadline, env);
    if (n <= 0) return n;
    k.rpos = 0;
    k.rlen = (size_t)n;
  }
  *out = k.rbuf[k.rpos++];
  return 1;
}

int16_t readFailure(int r) {
  if (r == -2) return CTL_ERR_LINK;
  if (r == 0) return CTL_ERR_TIMEOUT;
  return CTL_ERR_PROTOCOL;   // closed mid-reply
}

int readLine(Conn& k, char* line, size_t cap, size_t* total, uint32_t deadline,
             const ControlEnv& env) {
  return readHttpLine([&](uint8_t* b) { return readByte(k, b, deadline, env); },
                      line, cap, total);
}

int16_t connectConn(Conn& k, uint32_t deadline, const ControlEnv& env) {
  k.close();
  if (aborted(env)) return CTL_ERR_LINK;
  if (k.base.tls && !k.tls_ready) return CTL_ERR_CONNECT;
  // operator new aborts on failure without exceptions; refuse before it can.
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) < 4096) return CTL_ERR_NO_MEMORY;
  if (!k.c) {
    if (k.base.tls) {
      k.tls = new (std::nothrow) BundleTlsClient();
      if (!k.tls) return CTL_ERR_NO_MEMORY;
      k.tls->useLoadedBundle();
      k.c = k.tls;
    } else {
      k.plain = new (std::nothrow) WiFiClient();
      if (!k.plain) return CTL_ERR_NO_MEMORY;
      k.c = k.plain;
    }
  }
  IPAddress ip;
  if (!activeNetworkLink().resolveHost(k.base.host, ip)) {
    return aborted(env) ? CTL_ERR_LINK : CTL_ERR_CONNECT;
  }
  const uint32_t left = remainingMs(deadline);
  if (left < 250) return CTL_ERR_TIMEOUT;
  const uint32_t connect_ms = left < kControlConnectMs ? left : kControlConnectMs;
  const uint32_t t0 = millis();
  int ok = 0;
  if (k.tls) {
    k.tls->setLimits((int)connect_ms, left < kControlHandshakeMs ? left : kControlHandshakeMs);
    ok = k.tls->connect(ip, k.base.port, k.base.host, nullptr, nullptr, nullptr);
  } else {
    ok = k.plain->connect(ip, k.base.port, (int32_t)connect_ms);
  }
  const uint32_t took = millis() - t0;
  if (!ok) {
    POLL_CTL_LOG("connect failed (%s) after %lums", k.tls ? "tls" : "tcp", (unsigned long)took);
    k.close();
    if (aborted(env)) return CTL_ERR_LINK;
    return expired(deadline) ? CTL_ERR_TIMEOUT : CTL_ERR_CONNECT;
  }
  POLL_CTL_LOG("connected (%s) in %lums", k.tls ? "tls" : "tcp", (unsigned long)took);
  (void)took;
  return 0;
}

struct Reply {
  int16_t status;   // HTTP status or ControlFailure
  size_t  len;      // body bytes in s_body
  bool    retry;    // a reused connection was found closed before any reply byte
};

bool appendBody(Conn& k, size_t want, size_t* len, uint32_t deadline, const ControlEnv& env,
                int16_t* fail) {
  if (*len + want > kControlBodyMax) {
    *fail = CTL_ERR_OVERSIZE;
    return false;
  }
  while (want > 0) {
    const int ready = readReady(deadline, env);
    if (ready <= 0) {
      *fail = readFailure(ready);
      return false;
    }
    if (k.rpos < k.rlen) {
      size_t n = k.rlen - k.rpos;
      if (n > want) n = want;
      memcpy(s_body + *len, k.rbuf + k.rpos, n);
      k.rpos += n;
      *len += n;
      want -= n;
      continue;
    }
    const int r = readSome(k, reinterpret_cast<uint8_t*>(s_body + *len), want, deadline, env);
    if (r <= 0) {
      *fail = readFailure(r);
      return false;
    }
    *len += (size_t)r;
    want -= (size_t)r;
  }
  return true;
}

Reply readReply(Conn& k, bool reused, uint32_t deadline, const ControlEnv& env) {
  Reply out = {0, 0, false};
  char line[256];
  size_t header_bytes = 0;
  bool http10 = false;
  HttpFraming f;
  int status = 0;
  for (;;) {
    const int r = readLine(k, line, sizeof(line), &header_bytes, deadline, env);
    if (r <= 0) {
      out.retry = reused && r == -1 && header_bytes == 0;
      out.status = readFailure(r);
      return out;
    }
    status = parseHttpStatusLine(line, &http10);
    if (status < 0) {
      out.status = CTL_ERR_PROTOCOL;
      return out;
    }
    httpFramingInit(f);
    for (;;) {
      const int h = readLine(k, line, sizeof(line), &header_bytes, deadline, env);
      if (h <= 0) {
        out.status = readFailure(h);
        return out;
      }
      if (line[0] == '\0') break;
      if (!applyHttpHeader(line, f)) {
        out.status = CTL_ERR_PROTOCOL;
        return out;
      }
    }
    if (status >= 200) break;   // an interim 1xx reply is followed by the real one
  }

  int16_t fail = 0;
  size_t len = 0;
  bool framed = true;
  // Only a 200 body is parsed. Another reply keeps its status even when its
  // error page is large: a small framed one is drained, anything else closes.
  if (status != 200 && !(f.content_length >= 0 && f.content_length <= (int32_t)kControlBodyMax &&
                         !f.chunked)) {
    k.reusable = false;
    out.status = (int16_t)status;
    return out;
  }
  if (status == 204 || status == 304) {
    // no body
  } else if (f.chunked) {
    for (;;) {
      const int r = readLine(k, line, sizeof(line), &header_bytes, deadline, env);
      if (r <= 0) {
        out.status = readFailure(r);
        return out;
      }
      const int32_t size = parseHttpChunkSize(line);
      if (size < 0) {
        out.status = CTL_ERR_PROTOCOL;
        return out;
      }
      if (size == 0) {
        // Trailers end at an empty line.
        for (;;) {
          const int t = readLine(k, line, sizeof(line), &header_bytes, deadline, env);
          if (t <= 0) {
            out.status = readFailure(t);
            return out;
          }
          if (line[0] == '\0') break;
        }
        break;
      }
      if (!appendBody(k, (size_t)size, &len, deadline, env, &fail)) {
        out.status = fail;
        return out;
      }
      const int crlf = readLine(k, line, sizeof(line), &header_bytes, deadline, env);
      if (crlf <= 0 || line[0] != '\0') {
        out.status = crlf <= 0 ? readFailure(crlf) : CTL_ERR_PROTOCOL;
        return out;
      }
    }
  } else if (f.content_length >= 0) {
    if (!appendBody(k, (size_t)f.content_length, &len, deadline, env, &fail)) {
      out.status = fail;
      return out;
    }
  } else {
    // Unframed: the body runs to the close.
    framed = false;
    for (;;) {
      const int ready = readReady(deadline, env);
      if (ready <= 0) {
        out.status = readFailure(ready);
        return out;
      }
      if (len >= kControlBodyMax) {
        uint8_t extra = 0;
        const int r = readByte(k, &extra, deadline, env);
        if (r == -1) break;
        out.status = r > 0 ? CTL_ERR_OVERSIZE : readFailure(r);
        return out;
      }
      if (k.rpos < k.rlen) {
        s_body[len++] = (char)k.rbuf[k.rpos++];
        continue;
      }
      const int r = readSome(k, reinterpret_cast<uint8_t*>(s_body + len), kControlBodyMax - len,
                             deadline, env);
      if (r == -1) break;
      if (r <= 0) {
        out.status = readFailure(r);
        return out;
      }
      len += (size_t)r;
    }
  }
  k.reusable = framed && !f.close && !(http10 && !f.keep_alive);
  out.status = (int16_t)status;
  out.len = status == 200 ? len : 0;
  return out;
}

// Path without its query, for the log.
void logRequest(const char* method, const char* path, int16_t status, size_t len, uint32_t ms) {
  char shown[64];
  size_t n = 0;
  while (path[n] && path[n] != '?' && n + 1 < sizeof(shown)) {
    shown[n] = path[n];
    n++;
  }
  shown[n] = '\0';
  POLL_CTL_LOG("%s %s %d %uB %lums", method, shown, (int)status, (unsigned)len, (unsigned long)ms);
  (void)method; (void)shown; (void)status; (void)len; (void)ms;
}

Reply request(Conn& k, const char* method, const char* path, const char* body, size_t body_len,
              uint32_t session_deadline, const ControlEnv& env) {
  Reply out = {0, 0, false};
  const uint32_t t0 = millis();
  if (k.failed != 0) {
    // One connection attempt per session: a host that failed once fails the rest.
    out.status = k.failed;
    return out;
  }
  if (expired(session_deadline)) return out;   // 0: not made
  if (aborted(env)) {
    out.status = CTL_ERR_LINK;
    return out;
  }

  char head[352];
  const bool default_port = (k.base.tls && k.base.port == 443) || (!k.base.tls && k.base.port == 80);
  char host[kHttpHostMax + 8];
  if (default_port) snprintf(host, sizeof(host), "%s", k.base.host);
  else snprintf(host, sizeof(host), "%s:%u", k.base.host, (unsigned)k.base.port);
  int n;
  if (body) {
    n = snprintf(head, sizeof(head),
                 "%s %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: meshcore-observer/%s\r\n"
                 "Accept: application/json\r\nConnection: keep-alive\r\n"
                 "Content-Type: application/json\r\nContent-Length: %u\r\n\r\n",
                 method, k.base.prefix, path, host, kPluginVersion, (unsigned)body_len);
  } else {
    n = snprintf(head, sizeof(head),
                 "%s %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: meshcore-observer/%s\r\n"
                 "Accept: application/json\r\nConnection: keep-alive\r\n\r\n",
                 method, k.base.prefix, path, host, kPluginVersion);
  }
  if (n <= 0 || (size_t)n >= sizeof(head)) {
    out.status = CTL_ERR_PROTOCOL;
    return out;
  }

  for (int attempt = 0; attempt < 2; attempt++) {
    const bool reused = k.reusable && k.c && k.c->connected();
    if (!reused) {
      const int16_t c = connectConn(k, session_deadline, env);
      if (c != 0) {
        out.status = c;
        k.failed = c;
        break;
      }
    }
    k.reusable = false;
    k.rpos = k.rlen = 0;
    bool sent = k.c->write(reinterpret_cast<const uint8_t*>(head), (size_t)n) == (size_t)n;
    if (sent && body) {
      sent = k.c->write(reinterpret_cast<const uint8_t*>(body), body_len) == body_len;
    }
    if (!sent) {
      k.close();
      if (reused && attempt == 0) continue;   // the server dropped an idle connection
      out.status = aborted(env) ? CTL_ERR_LINK : CTL_ERR_CONNECT;
      break;
    }
    uint32_t reply_deadline = millis() + kControlResponseMs;
    if ((int32_t)(reply_deadline - session_deadline) > 0) reply_deadline = session_deadline;
    out = readReply(k, reused, reply_deadline, env);
    if (out.status < 0) k.close();
    if (out.retry && attempt == 0) continue;
    if (!k.reusable) k.close();
    break;
  }
  out.retry = false;
  logRequest(method, path, out.status, out.len, millis() - t0);
  return out;
}

void hexLower(char* out, const uint8_t* data, size_t len) {
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = kHex[data[i] >> 4];
    out[2 * i + 1] = kHex[data[i] & 15];
  }
  out[2 * len] = '\0';
}

// The pair must belong together or every request is encrypted to a key no one holds.
bool expandSharedKey(const uint8_t pub[kPubKeySize], const uint8_t* prv, uint8_t prv_len,
                     uint8_t out[64]) {
  uint8_t derived[kPubKeySize];
  if (prv_len == 32) {
    ed25519_create_keypair(derived, out, prv);
  } else if (prv_len == 64) {
    memcpy(out, prv, 64);
    ed25519_derive_pub(derived, out);
  } else {
    return false;
  }
  if (memcmp(derived, pub, kPubKeySize) != 0) {
    ed25519_secure_zero(out, 64);
    return false;
  }
  return true;
}

}  // namespace

void wipeControlResult(ControlResult& res) {
  volatile uint8_t* p = res.shared_prv;
  for (size_t i = 0; i < sizeof(res.shared_prv); i++) p[i] = 0;
}

void runControlSession(const ControlRequest& req, ControlResult& res, const ControlEnv& env) {
  memset(&res, 0, sizeof(res));
  res.started = true;
  const uint32_t t0 = millis();
  const uint32_t deadline = t0 + kControlBudgetMs;
  const uint32_t int_before = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const uint32_t blk_before = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  // Boot-wide low-water; a drop across the session means the session set it.
  const uint32_t low_before = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  const bool want_reg = (req.want & WANT_REGISTER) && req.reg_body_len > 0 &&
                        req.reg_body_len <= sizeof(req.reg_body);

  Conn* k = new (std::nothrow) Conn();
  if (!s_body) s_body = static_cast<char*>(bigAlloc(kControlBodyMax + 1));
  int16_t setup = 0;
  if (!k || !s_body) setup = CTL_ERR_NO_MEMORY;
  else if (!parseHttpBase(req.url, k->base)) setup = CTL_ERR_CONNECT;
  else if (k->base.tls && !env.tls_bundle_ready) setup = CTL_ERR_CONNECT;
  POLL_CTL_LOG("control session %s (%s) int_heap=%u max_blk=%u int_low=%u",
               setup == 0 ? "started" : "not started",
               setup == 0 ? (k->base.tls ? "https" : "http")
                          : (setup == CTL_ERR_NO_MEMORY ? "no memory" : "bad url or no CA bundle"),
               (unsigned)int_before, (unsigned)blk_before, (unsigned)low_before);
  if (setup != 0) {
    if (req.want & WANT_OWNERS) res.owners_status = setup;
    if (req.want & WANT_POLICY) res.policy_status = setup;
    if (want_reg) res.reg_status = setup;
    if (req.want & WANT_SHARED_KEY) res.shared_status = setup;
    delete k;
    return;
  }
  k->tls_ready = env.tls_bundle_ready;

  WipingAllocator alloc;
  char obs[2 * kPubKeySize + 1];
  hexLower(obs, req.observer_pub, kPubKeySize);
  char path[200];
  const uint8_t count = req.target_count > kMaxTargets ? kMaxTargets : req.target_count;

  if (req.want & WANT_OWNERS) {
    snprintf(path, sizeof(path), "/api/guest-polling/owners?observer=%s&plugin_version=%s", obs,
             kPluginVersion);
    const Reply r = request(*k, "GET", path, nullptr, 0, deadline, env);
    res.owners_status = r.status;
    if (r.status == 200) {
      bool has_policy = false;
      if (parseOwnersReply(s_body, r.len, req.targets, count, res.owners, &res.owners_ok,
                           &res.policy, &has_policy, &alloc)) {
        if (has_policy) {
          res.policy_ok = true;
          res.policy_status = r.status;
        }
      } else {
        res.owners_status = CTL_ERR_PROTOCOL;
        memset(res.owners, 0, sizeof(res.owners));
      }
    }
  }

  if ((req.want & WANT_POLICY) && !res.policy_ok) {
    snprintf(path, sizeof(path), "/api/guest-polling/policy?plugin_version=%s&observer=%s",
             kPluginVersion, obs);
    const Reply r = request(*k, "GET", path, nullptr, 0, deadline, env);
    res.policy_status = r.status;
    if (r.status == 200) {
      res.policy_ok = parsePolicyReply(s_body, r.len, res.policy, &alloc);
      if (!res.policy_ok) res.policy_status = CTL_ERR_PROTOCOL;
    }
  }
  if (!res.policy_ok) policyDefaults(res.policy);

  if (want_reg) {
    const Reply r = request(*k, "POST", "/api/guest-keys", req.reg_body, req.reg_body_len,
                            deadline, env);
    res.reg_status = r.status;
    // Like the plugin: a 200 keeps its status even with an unreadable body.
    if (r.status == 200) res.reg_body_ok = parseRegistrationReply(s_body, r.len, &alloc);
  }

  if ((req.want & WANT_SHARED_KEY) || res.reg_status == 404) {
    const Reply r = request(*k, "GET", "/api/shared-guest/key", nullptr, 0, deadline, env);
    res.shared_status = r.status;
    if (r.status == 200) {
      uint8_t prv[64];
      uint8_t prv_len = 0;
      if (parseSharedKeyReply(s_body, r.len, res.shared_pub, prv, &prv_len, &alloc)) {
        res.shared_ok = expandSharedKey(res.shared_pub, prv, prv_len, res.shared_prv);
        if (!res.shared_ok) POLL_CTL_LOG("shared key rejected: the halves do not match");
      } else {
        res.shared_status = CTL_ERR_PROTOCOL;
      }
      volatile uint8_t* vp = prv;
      for (size_t i = 0; i < sizeof(prv); i++) vp[i] = 0;
      if (!res.shared_ok) memset(res.shared_pub, 0, sizeof(res.shared_pub));
    }
    // The reply held a private key in clear text.
    volatile char* vb = s_body;
    for (size_t i = 0; i <= kControlBodyMax; i++) vb[i] = 0;
  }

  delete k;
  POLL_CTL_LOG("control session done in %lums: owners=%d policy=%d reg=%d shared=%d "
               "int_heap=%u->%u max_blk=%u->%u int_low=%u->%u",
               (unsigned long)(millis() - t0), (int)res.owners_status, (int)res.policy_status,
               (int)res.reg_status, (int)res.shared_status, (unsigned)int_before,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)blk_before,
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)low_before,
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  (void)int_before; (void)blk_before; (void)low_before; (void)t0;
}

}  // namespace poll

#endif  // WITH_REPEATER_POLL
