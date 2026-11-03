#pragma once
#include <stdint.h>
#include <stddef.h>
inline uint32_t test_clock = 100;
inline uint32_t millis() { return test_clock; }
inline void delay(uint32_t time) { test_clock += time; }
struct IPAddress {};
namespace BearSSL {
class X509List {
public:
  inline static bool fail_append = false;
  inline static int instances = 0, appends = 0;
  X509List(const char* = nullptr) { ++instances; }
  ~X509List() { --instances; }
  int getCount() const { return 1; }
  bool append(const char*) { ++appends; return !fail_append; }
};
}
class Client {
public:
  virtual ~Client() = default;
  virtual int connect(const char*, uint16_t) = 0;
  virtual int connect(IPAddress, uint16_t) = 0;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t*, size_t) = 0;
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int read(uint8_t*, size_t) = 0;
  virtual int peek() = 0;
  virtual void flush() = 0;
  virtual void stop() = 0;
  virtual uint8_t connected() = 0;
  virtual operator bool() = 0;
};
