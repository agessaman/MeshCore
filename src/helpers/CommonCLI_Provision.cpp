// CommonCLI_Provision.cpp — fork-owned support for the /provision defaults package
// (user docs in PROVISIONING.md). A provision file is a plain-text list of CLI
// commands under a "#meshcore-provision v1" header; each line is fed verbatim to
// CommonCLI::handleCommand(), so validation, privilege checks (the existing
// sender_timestamp == 0 guards) and persistence are exactly the same as typing the
// commands. Nothing here touches any persisted struct layout.
//
// Command family (all builds):
//   provision                        — status: file? size? header version? marker?
//   provision show [start]           — serial: full dump; remote: paged lines
//   provision apply                  — run the file with the invoker's privileges
//   provision remove                 — delete /provision and /provision_done
//   provision begin / provision end  — serial-only paste capture into /provision
//   provision fetch <url> [insecure] — HTTPS/HTTP download (WITH_MQTT_BRIDGE builds)
//
// Boot auto-apply is autoApplyProvisionFile(); apps call it at the very end of
// begin() and reboot when it returns true. The /provision_done marker is written
// BEFORE the file runs, so a crash or reset mid-apply can never cause a boot loop.

#include <Arduino.h>
#include "CommonCLI.h"

#if defined(WITH_MQTT_BRIDGE) && defined(ESP_PLATFORM)
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#endif

#define PROVISION_FILE      "/provision"
#define PROVISION_MARKER    "/provision_done"
#define PROVISION_MAX_SIZE  4096
#define PROVISION_MAX_LINE  159   // matches the 160-byte serial command buffer
#define PROVISION_VERSION   1

static const char PROVISION_HEADER_PREFIX[] = "#meshcore-provision v";

// ---------------------------------------------------------------------------
// filesystem open helpers (platform patterns mirror saveMQTTPrefs/openAppend)

static File provOpenRead(FILESYSTEM* fs) {
#if defined(RP2040_PLATFORM)
  return fs->open(PROVISION_FILE, "r");
#else
  return fs->open(PROVISION_FILE);
#endif
}

static File provOpenWrite(FILESYSTEM* fs, const char* path) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  fs->remove(path);
  File f = fs->open(path, FILE_O_WRITE);
  return f;
#elif defined(RP2040_PLATFORM)
  return fs->open(path, "w");
#else
  return fs->open(path, "w", true);
#endif
}

static File provOpenAppend(FILESYSTEM* fs) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return fs->open(PROVISION_FILE, FILE_O_WRITE);   // Adafruit LittleFS: opens at end
#elif defined(RP2040_PLATFORM)
  return fs->open(PROVISION_FILE, "a");
#else
  return fs->open(PROVISION_FILE, "a", true);
#endif
}

// ---------------------------------------------------------------------------
// small parsing helpers

// Parse a header line ("#meshcore-provision v<N>"). Returns the version, or -1.
static int provParseHeader(const char* line) {
  const size_t plen = sizeof(PROVISION_HEADER_PREFIX) - 1;
  if (strncmp(line, PROVISION_HEADER_PREFIX, plen) != 0) return -1;
  const char* vp = line + plen;
  if (*vp < '0' || *vp > '9') return -1;
  return atoi(vp);
}

// Commands never run from a provision file. Everything here either destroys state
// (erase), reboots/halts/flashes mid-apply (reboot/clkreboot/poweroff/shutdown/ota),
// changes credentials a defaults package has no business touching (password,
// prv.key), recurses (provision), or enters an interactive multi-line mode that
// cannot work when lines are fed from a file (region load).
static const char* const PROVISION_BLOCKLIST[] = {
  "provision",
  "erase",
  "start ota",
  "ota ",
  "password ",
  "set prv.key",
  "reboot",
  "clkreboot",
  "poweroff",
  "shutdown",
  "region load",
};

static bool provisionBlocked(const char* line) {
  for (size_t i = 0; i < sizeof(PROVISION_BLOCKLIST) / sizeof(PROVISION_BLOCKLIST[0]); i++) {
    if (strncmp(line, PROVISION_BLOCKLIST[i], strlen(PROVISION_BLOCKLIST[i])) == 0) return true;
  }
  return false;
}

// Case-insensitive prefix test (strncasecmp isn't reliably available everywhere).
static bool provPrefixNoCase(const char* s, const char* prefix) {
  while (*prefix) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    s++; prefix++;
  }
  return true;
}

// Heuristic over the CLI reply conventions: "Err...", "ERR:", "Error:", "(ERR: ...)",
// "Unknown command", "unknown config: ...", "??: ..." all mean the line didn't take.
static bool provisionReplyIsError(const char* reply) {
  while (*reply == ' ' || *reply == '(') reply++;
  if (provPrefixNoCase(reply, "err")) return true;
  if (provPrefixNoCase(reply, "unknown")) return true;
  if (reply[0] == '?' && reply[1] == '?') return true;
  return false;
}

// Read one newline-terminated line into buf, stripping '\r'. Returns false only at
// EOF with nothing read. A line longer than the buffer is consumed to its newline
// and flagged via *truncated.
static bool provisionReadLine(File& f, char* buf, size_t buf_size, bool* truncated) {
  *truncated = false;
  size_t n = 0;
  bool got_any = false;
  while (true) {
    int c = f.read();
    if (c < 0) break;         // EOF
    got_any = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (n + 1 < buf_size) {
      buf[n++] = (char)c;
    } else {
      *truncated = true;      // keep consuming to end of line
    }
  }
  buf[n] = 0;
  return got_any;
}

// ---------------------------------------------------------------------------
// core runner

void CommonCLI::runProvisionFile(uint32_t sender_timestamp, char* reply) {
  if (_fs == NULL) { strcpy(reply, "Err - filesystem not ready"); return; }
#ifdef WITH_MQTT_BRIDGE
  if (_mqtt_prefs_hold) {
    // /mqtt_prefs was written by newer firmware (see saveMQTTPrefs). Applying a
    // provision file would rewrite it in this firmware's older format.
    strcpy(reply, "Err - /mqtt_prefs is from newer firmware; not applying");
    return;
  }
#endif
  File f = provOpenRead(_fs);
  if (!f) { strcpy(reply, "Err - no /provision file"); return; }
  uint32_t fsize = f.size();
  if (fsize > PROVISION_MAX_SIZE) {
    f.close();
    sprintf(reply, "Err - /provision too big (%u > %d bytes)", (unsigned)fsize, PROVISION_MAX_SIZE);
    return;
  }

  char line[PROVISION_MAX_LINE + 1];
  bool truncated;
  bool got_header = false;
  int applied = 0, failed = 0, skipped = 0;

  while (provisionReadLine(f, line, sizeof(line), &truncated)) {
    if (truncated) {
      failed++;
      MESH_DEBUG_PRINTLN("provision: line too long, skipped");
      continue;
    }
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == 0) continue;   // blank line

    if (!got_header) {
      // header must be the FIRST non-blank line — nothing is applied before this check
      int ver = provParseHeader(p);
      if (ver < 0) {
        f.close();
        strcpy(reply, "Err - missing '#meshcore-provision v1' header");
        return;
      }
      if (ver != PROVISION_VERSION) {
        f.close();
        sprintf(reply, "Err - unsupported provision version %d", ver);
        return;
      }
      got_header = true;
      continue;
    }
    if (*p == '#') continue;   // comment

    if (provisionBlocked(p)) {
      skipped++;
      MESH_DEBUG_PRINTLN("provision: blocked '%s'", p);
      continue;
    }

    // Copy the line before handing it to handleCommand: the CLI mutates its
    // command buffer in place, and our read buffer is reused for the next line.
    char cmd[PROVISION_MAX_LINE + 1];
    strcpy(cmd, p);
    char line_reply[160];
    line_reply[0] = 0;
    handleCommand(sender_timestamp, cmd, line_reply);
    MESH_DEBUG_PRINTLN("provision: '%s' -> %s", p, line_reply);
    if (provisionReplyIsError(line_reply)) failed++; else applied++;
  }
  f.close();

  if (!got_header) { strcpy(reply, "Err - /provision is empty"); return; }
  sprintf(reply, "Provision: %d applied, %d failed, %d skipped", applied, failed, skipped);
}

bool CommonCLI::autoApplyProvisionFile(char* reply) {
  reply[0] = 0;
  if (_fs == NULL) return false;
  if (!_fs->exists(PROVISION_FILE) || _fs->exists(PROVISION_MARKER)) return false;

  // Marker FIRST: if a command below crashes or resets the device mid-apply, the
  // next boot must not re-enter the runner (reboot-loop guard). Manual
  // 'provision apply' deliberately ignores the marker.
  File m = provOpenWrite(_fs, PROVISION_MARKER);
  if (!m) {
    strcpy(reply, "Err - cannot write /provision_done; provision not auto-applied");
    return false;
  }
  m.close();

  runProvisionFile(0, reply);   // serial privileges: this is the first-boot jump-start path
  return true;
}

// ---------------------------------------------------------------------------
// paste capture ('provision begin' ... 'provision end', serial-only)

void CommonCLI::provisionCaptureLine(const char* line, char* reply) {
  reply[0] = 0;   // silent per-line so pasting a file doesn't flood the console

  const char* p = line;
  while (*p == ' ' || *p == '\t') p++;

  if (!_prov_capture_got_header) {
    if (*p == 0) return;   // leading blank lines tolerated (and not written)
    int ver = provParseHeader(p);
    if (ver != PROVISION_VERSION) {
      _prov_capture = false;
      _fs->remove(PROVISION_FILE);
      strcpy(reply, "Err - first line must be '#meshcore-provision v1'; capture aborted");
      return;
    }
    _prov_capture_got_header = true;
  }

  size_t len = strlen(line);
  if (len > PROVISION_MAX_LINE) {   // can't happen via the 160-char serial buffer, but be safe
    _prov_capture = false;
    _fs->remove(PROVISION_FILE);
    strcpy(reply, "Err - line too long; capture aborted");
    return;
  }
  if (_prov_capture_bytes + len + 1 > PROVISION_MAX_SIZE) {
    _prov_capture = false;
    _fs->remove(PROVISION_FILE);
    sprintf(reply, "Err - /provision would exceed %d bytes; capture aborted", PROVISION_MAX_SIZE);
    return;
  }

  File f = provOpenAppend(_fs);
  if (!f) {
    _prov_capture = false;
    strcpy(reply, "Err - write failed; capture aborted");
    return;
  }
  f.write((const uint8_t*)line, len);
  f.write((uint8_t)'\n');
  f.close();
  _prov_capture_bytes += len + 1;
  _prov_capture_lines++;
}

// ---------------------------------------------------------------------------
// fetch (Phase 2 — WITH_MQTT_BRIDGE builds are all ESP32-family)

#if defined(WITH_MQTT_BRIDGE) && defined(ESP_PLATFORM)

#define PROV_FETCH_TOO_BIG  -1000   // local sentinel, distinct from HTTPClient errors

// One GET attempt with the given client. Returns the HTTP code (>0), an HTTPClient
// error (<0), or PROV_FETCH_TOO_BIG. On 200, streams the body into buf; *total may
// end up cap+1 to signal an over-cap body when the length wasn't known up front.
static int provisionHttpGet(WiFiClient& client, const char* url, char* buf, int cap, int* total) {
  *total = 0;
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    int expected = http.getSize();   // -1 when chunked/unknown
    if (expected > cap) {
      http.end();
      return PROV_FETCH_TOO_BIG;
    }
    WiFiClient* stream = http.getStreamPtr();
    int n = 0;
    uint32_t deadline = millis() + 20000;
    while ((int32_t)(millis() - deadline) < 0) {
      int avail = stream->available();
      if (avail > 0) {
        int want = cap + 1 - n;   // read one byte past cap to detect oversize
        if (want <= 0) break;
        int r = stream->read((uint8_t*)buf + n, (avail < want) ? avail : want);
        if (r > 0) n += r;
      } else if (!http.connected()) {
        break;
      } else {
        delay(10);
      }
      if (expected >= 0 && n >= expected) break;
    }
    *total = n;
  }
  http.end();
  return code;
}

static void handleProvisionFetch(FILESYSTEM* fs, const char* args, char* reply) {
  while (*args == ' ') args++;
  if (*args == 0) { strcpy(reply, "Err - usage: provision fetch <url> [insecure]"); return; }
  if (WiFi.status() != WL_CONNECTED) {
    strcpy(reply, "Err - WiFi not connected (set wifi.ssid / set wifi.pwd first)");
    return;
  }

  char url[PROVISION_MAX_LINE + 1];
  const char* sp = strchr(args, ' ');
  size_t ulen = sp ? (size_t)(sp - args) : strlen(args);
  if (ulen >= sizeof(url)) { strcpy(reply, "Err - url too long"); return; }
  memcpy(url, args, ulen);
  url[ulen] = 0;
  bool insecure = (sp != NULL) && (strstr(sp, "insecure") != NULL);

  bool is_https = strncmp(url, "https://", 8) == 0;
  bool is_http = strncmp(url, "http://", 7) == 0;
  if (!is_https && !is_http) { strcpy(reply, "Err - url must be http:// or https://"); return; }

  char* buf = (char*)malloc(PROVISION_MAX_SIZE + 1);
  if (!buf) { strcpy(reply, "Err - out of memory"); return; }

  int total = 0;
  int code;
  if (is_http) {
    WiFiClient plain;
    code = provisionHttpGet(plain, url, buf, PROVISION_MAX_SIZE, &total);
  } else if (insecure) {
    WiFiClientSecure client;
    client.setInsecure();
    code = provisionHttpGet(client, url, buf, PROVISION_MAX_SIZE, &total);
  } else {
    // Validate against the CA roots already bundled for the MQTT presets; try each.
    const char* const roots[] = { GTS_ROOT_R4, ISRG_ROOT_X1 };
    code = HTTPC_ERROR_CONNECTION_REFUSED;
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
      WiFiClientSecure client;
      client.setCACert(roots[i]);
      code = provisionHttpGet(client, url, buf, PROVISION_MAX_SIZE, &total);
      if (code > 0 || code == PROV_FETCH_TOO_BIG) break;   // got an HTTP response, TLS was fine
    }
  }

  if (code == PROV_FETCH_TOO_BIG || total > PROVISION_MAX_SIZE) {
    sprintf(reply, "Err - file too big (max %d bytes)", PROVISION_MAX_SIZE);
  } else if (code == HTTP_CODE_OK) {
    buf[total] = 0;
    // Validate the header BEFORE touching /provision — never clobber an existing
    // good file with garbage. The trust decision happens later, at apply time.
    const char* p = buf;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    int ver = provParseHeader(p);
    if (ver < 0) {
      strcpy(reply, "Err - fetched file missing '#meshcore-provision v1' header; not saved");
    } else if (ver != PROVISION_VERSION) {
      sprintf(reply, "Err - unsupported provision version %d; not saved", ver);
    } else {
      File f = provOpenWrite(fs, PROVISION_FILE);
      if (!f) {
        strcpy(reply, "Err - cannot write /provision");
      } else {
        f.write((const uint8_t*)buf, total);
        f.close();
        int lines = 0;
        for (int i = 0; i < total; i++) {
          if (buf[i] == '\n') lines++;
        }
        if (total > 0 && buf[total - 1] != '\n') lines++;
        sprintf(reply, "OK - fetched %d bytes (%d lines); review with 'provision show', run 'provision apply'",
                total, lines);
      }
    }
  } else if (code > 0) {
    sprintf(reply, "Err - HTTP %d", code);
  } else {
    snprintf(reply, 160, "Err - connection failed (%s)", HTTPClient::errorToString(code).c_str());
  }
  free(buf);
}

#endif  // WITH_MQTT_BRIDGE && ESP_PLATFORM

// ---------------------------------------------------------------------------
// command dispatch — called first from CommonCLI::handleCommand

bool CommonCLI::handleProvisionCommand(uint32_t sender_timestamp, char* command, char* reply) {
  // Paste capture: while active, every serial line is written to /provision
  // instead of executed; 'provision end' closes the capture (without applying).
  // Remote (sender_timestamp != 0) commands are unaffected by capture mode.
  if (_prov_capture && sender_timestamp == 0) {
    // tolerate trailing spaces on the end marker
    bool is_end = false;
    if (memcmp(command, "provision end", 13) == 0) {
      const char* t = &command[13];
      while (*t == ' ') t++;
      is_end = (*t == 0);
    }
    if (is_end) {
      _prov_capture = false;
      sprintf(reply, "OK - /provision: %u lines, %u bytes (not applied; run 'provision apply')",
              (unsigned)_prov_capture_lines, (unsigned)_prov_capture_bytes);
    } else {
      provisionCaptureLine(command, reply);
    }
    return true;
  }

  if (memcmp(command, "provision", 9) != 0) return false;
  if (command[9] != 0 && command[9] != ' ') return false;
  const char* args = (command[9] == ' ') ? &command[10] : "";
  while (*args == ' ') args++;

  if (_fs == NULL) { strcpy(reply, "Err - filesystem not ready"); return true; }

  if (*args == 0) {
    // status
    bool marker = _fs->exists(PROVISION_MARKER);
    File f = provOpenRead(_fs);
    if (!f) {
      sprintf(reply, "no /provision; marker: %s", marker ? "present" : "absent");
    } else {
      uint32_t size = f.size();
      // header version lives on the first non-blank line
      char line[PROVISION_MAX_LINE + 1];
      bool truncated;
      int ver = -1;
      while (provisionReadLine(f, line, sizeof(line), &truncated)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0) continue;
        if (!truncated) ver = provParseHeader(p);
        break;
      }
      f.close();
      if (ver >= 0) {
        sprintf(reply, "/provision: %u bytes, v%d; marker: %s", (unsigned)size, ver,
                marker ? "present" : "absent");
      } else {
        sprintf(reply, "/provision: %u bytes, BAD HEADER; marker: %s", (unsigned)size,
                marker ? "present" : "absent");
      }
    }
  } else if (strcmp(args, "show") == 0 || memcmp(args, "show ", 5) == 0) {
    File f = provOpenRead(_fs);
    if (!f) { strcpy(reply, "Err - no /provision file"); return true; }
    char line[PROVISION_MAX_LINE + 1];
    bool truncated;
    if (sender_timestamp == 0) {
      // serial: print the whole file to the console, summary in reply
      uint32_t size = f.size();
      int lines = 0;
      Serial.println("--- /provision ---");
      while (provisionReadLine(f, line, sizeof(line), &truncated)) {
        Serial.println(line);
        lines++;
      }
      Serial.println("--- end ---");
      sprintf(reply, "/provision: %d lines, %u bytes", lines, (unsigned)size);
    } else {
      // remote: page lines into the 160-char reply — 'provision show [start]'
      int start = (args[4] == ' ') ? atoi(&args[5]) : 1;
      if (start < 1) start = 1;
      const size_t reply_cap = 160;
      const size_t reserve_for_next = 16;   // room for "\n... next:NNN"
      size_t used = 0;
      int num = 0, next = 0;
      reply[0] = 0;
      while (provisionReadLine(f, line, sizeof(line), &truncated)) {
        num++;
        if (num < start) continue;
        size_t len = strlen(line);
        if (used + len + 1 >= reply_cap - reserve_for_next) {
          if (used == 0) {
            // single line longer than a page: show a truncated prefix so paging
            // can still advance past it
            len = reply_cap - reserve_for_next - 2;
            memcpy(reply, line, len);
            reply[len] = 0;
            used = len;
            next = num + 1;
          } else {
            next = num;
          }
          break;
        }
        if (used) reply[used++] = '\n';
        memcpy(reply + used, line, len);
        used += len;
        reply[used] = 0;
      }
      if (next) {
        sprintf(reply + used, "%s... next:%d", used ? "\n" : "", next);
      } else if (used == 0) {
        strcpy(reply, "(no lines at or past start)");
      }
    }
    f.close();
  } else if (strcmp(args, "apply") == 0) {
    // Runs with the INVOKER's privileges: remote applies can't change radio params
    // or prv.key (same policy as typing the commands). Marker deliberately ignored.
    if (_prov_capture) {
      strcpy(reply, "Err - capture in progress; finish with 'provision end' first");
    } else {
      runProvisionFile(sender_timestamp, reply);
    }
  } else if (strcmp(args, "remove") == 0) {
    bool had_file = _fs->exists(PROVISION_FILE);
    bool had_marker = _fs->exists(PROVISION_MARKER);
    if (had_file) _fs->remove(PROVISION_FILE);
    if (had_marker) _fs->remove(PROVISION_MARKER);
    if (had_file || had_marker) {
      sprintf(reply, "OK - removed%s%s", had_file ? " /provision" : "",
              had_marker ? " /provision_done" : "");
    } else {
      strcpy(reply, "OK - nothing to remove");
    }
  } else if (strcmp(args, "begin") == 0) {
    if (sender_timestamp != 0) {
      strcpy(reply, "Err - 'provision begin' is serial-only");
    } else {
      File f = provOpenWrite(_fs, PROVISION_FILE);   // create/truncate
      if (!f) {
        strcpy(reply, "Err - cannot create /provision");
      } else {
        f.close();
        _prov_capture = true;
        _prov_capture_got_header = false;
        _prov_capture_lines = 0;
        _prov_capture_bytes = 0;
        strcpy(reply, "OK - capturing to /provision; paste file, finish with 'provision end'");
      }
    }
  } else if (strcmp(args, "end") == 0) {
    strcpy(reply, "Err - no capture in progress");
  } else if (memcmp(args, "fetch", 5) == 0 && (args[5] == 0 || args[5] == ' ')) {
#if defined(WITH_MQTT_BRIDGE) && defined(ESP_PLATFORM)
    handleProvisionFetch(_fs, &args[5], reply);
#else
    strcpy(reply, "Err - fetch not supported on this build");
#endif
  } else {
    strcpy(reply, "Err - usage: provision [show|apply|remove|begin|end|fetch <url>]");
  }
  return true;
}
