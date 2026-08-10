#pragma once

#include <Arduino.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Bounded serial writer for the packet-logging paths.
//
// USBCDC::write() (arduino-esp32 2.0.17) spins with no timeout when the CDC TX FIFO is
// full and the host still has the port open but has stopped reading it:
//
//     size_t space = tud_cdc_n_write_available(itf);
//     if(!space){ tud_cdc_n_write_flush(itf); continue; }
//
// The FIFO is 64 bytes (CONFIG_TINYUSB_CDC_TX_BUFSIZE), so every packet log line needs the
// host to drain it several times mid-write. The spin runs in the Arduino loop task, which
// is pinned to core 1, and the task watchdog only covers the core 0 idle task
// (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 is unset) — so the node simply goes quiet: no
// CLI, no logging, no crash, no reboot, port still enumerated.
//
// Writes here only ever hand the port as many bytes as it reports it can take, so a host
// that stops draining costs a dropped log line instead of the mesh loop.

// Enough for a hex dump of a 255-byte packet plus its header.
#ifndef SERIAL_LOG_LINE_MAX
  #define SERIAL_LOG_LINE_MAX  640
#endif

// Only spent once a line is already part-written, to avoid truncating it.
#ifndef SERIAL_LOG_WRITE_BUDGET_MS
  #define SERIAL_LOG_WRITE_BUDGET_MS  20
#endif

// Log lines lost since the last DROP: marker got through.
inline uint32_t& serialLogDroppedCount() { static uint32_t n = 0; return n; }

// Whether the port has ever accepted output. An unattached port reports no room, exactly
// like a wedged one, so counting drops before this is set would greet the first host to
// connect with a tally of every line emitted since boot.
inline bool& serialLogPortSeen() { static bool seen = false; return seen; }

// Pushes out whatever the port will take without blocking, waiting only until the budget
// expires. Returns false if any of it had to be abandoned.
template <class T> bool serialLogEmit(T& out, const char* data, size_t len) {
  if (out.availableForWrite() <= 0) return false;   // host is not draining: drop now
  uint32_t start = millis();
  size_t sent = 0;
  while (sent < len) {
    int space = out.availableForWrite();
    if (space <= 0) {
      if ((uint32_t)(millis() - start) >= SERIAL_LOG_WRITE_BUDGET_MS) return false;
      delay(1);   // yields to the USB task
      continue;
    }
    size_t n = (size_t)space;
    if (n > len - sent) n = len - sent;
    size_t written = out.write((const uint8_t *)(data + sent), n);
    if (written == 0) return false;   // port closed
    sent += written;
  }
  serialLogPortSeen() = true;
  return true;
}

template <size_t CAP = SERIAL_LOG_LINE_MAX>
class SerialLogLine {
public:
  void printf(const char* fmt, ...) {
    size_t room = capacity() - _len;
    if (room == 0) { _truncated = true; return; }

    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(&_buf[_len], room, fmt, args);
    va_end(args);
    if (n < 0) return;
    if ((size_t)n >= room) {   // vsnprintf truncated; keep what fit, minus its terminator
      _len = capacity() - 1;
      _truncated = true;
    } else {
      _len += n;
    }
  }

  void hex(const uint8_t* src, size_t len) {
    static const char hex_chars[] = "0123456789ABCDEF";
    while (len > 0) {
      if (_len + 2 > capacity()) { _truncated = true; return; }
      uint8_t b = *src++;
      _buf[_len++] = hex_chars[b >> 4];
      _buf[_len++] = hex_chars[b & 0x0F];
      len--;
    }
  }

  // Terminates the line and pushes it out. Never blocks on a host that has stopped
  // reading. Returns false if the line was dropped or cut short.
  template <class T> bool flush(T& out) {
    bool complete = !_truncated;
    _buf[_len++] = '\r';   // CRLF, matching the Serial.println() these lines replaced
    _buf[_len++] = '\n';
    size_t len = _len;
    _len = 0;
    _truncated = false;

    // Tell whoever is parsing that they are looking at a gap, not a quiet mesh.
    uint32_t& dropped = serialLogDroppedCount();
    if (dropped > 0) {
      char marker[24];
      int n = snprintf(marker, sizeof(marker), "DROP:%u\r\n", (unsigned)dropped);
      if (n > 0 && serialLogEmit(out, marker, (size_t)n)) dropped = 0;
    }

    if (!serialLogEmit(out, _buf, len)) complete = false;
    if (!complete && serialLogPortSeen()) dropped++;
    return complete;
  }

private:
  size_t capacity() const { return CAP - 2; }   // leaves room for the CRLF

  char _buf[CAP];
  size_t _len = 0;
  bool _truncated = false;
};

// USBCDC/HWCDC wait up to tx_timeout_ms (250 by default) just to take the TX lock, so a
// write from another task can stall the mesh loop even when the FIFO has room.
inline void serialLogBegin() {
#if defined(ESP32_PLATFORM) && defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
  Serial.setTxTimeoutMs(0);
#endif
}
