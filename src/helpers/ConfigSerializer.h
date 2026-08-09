#pragma once

#include <Arduino.h>

#ifndef CONFIG_MAX_DEPTH
  #define CONFIG_MAX_DEPTH   8
#endif

#ifndef CONFIG_MAX_KEYLEN
  #define CONFIG_MAX_KEYLEN  16
#endif

#ifndef CONFIG_MAX_TOKEN_LEN
  #define CONFIG_MAX_TOKEN_LEN   128
#endif

class ConfigSerializer {
  bool _first;
  int8_t _depth;

  enum OP { READ, WRITE };

  class Context {
    Stream* _f;
    OP _op;
    uint8_t rd_len;
    uint8_t rd_mode;
    uint8_t rd_value_depth;
    char pending;
    bool rd_token_quoted;
    bool rd_object_start;
    char rd_buf[CONFIG_MAX_TOKEN_LEN];
    char _keys[CONFIG_MAX_DEPTH][CONFIG_MAX_KEYLEN];

  public:
    bool success = true;
    Context(Stream* f, OP op) : _f(f), _op(op) {
      rd_buf[rd_len = 0] = 0;
      rd_mode = 0;
      rd_value_depth = 0;
      pending = 0;
      rd_token_quoted = false;
      rd_object_start = false;
      memset(_keys, 0, sizeof(_keys));
    }
    OP op() const { return _op; }
    Stream* file() const { return _f; }
    int readNext();
    const char* getToken() const { return rd_buf; }
    bool tokenQuoted() const { return rd_token_quoted; }
    uint8_t valueDepth() const { return rd_value_depth; }
    bool objectStart() const { return rd_object_start; }
    void setValueEvent(uint8_t depth, bool object_start) {
      rd_value_depth = depth;
      rd_object_start = object_start;
    }
    bool keyMatch(int8_t depth, const char* key) { return strcmp(key, _keys[depth]) == 0; }
    void setKey(uint8_t depth, const char* key) { strcpy(_keys[depth], key);  }
  };

  Context* _context = NULL;

  void writeComma();

protected:
  ConfigSerializer() { }

  void def(const char* key, char* value, size_t max_len);  // max_len inclusive of null
  void def(const char* key, void* value, size_t len);  // binary blob (encoded in hex)
  void def(const char* key, int32_t& value);
  void def(const char* key, int16_t& value);
  void def(const char* key, int8_t& value);
  void def(const char* key, uint32_t& value);
  void def(const char* key, uint16_t& value);
  void def(const char* key, uint8_t& value);
  void def(const char* key, float& value);
  void def(const char* key, double& value);
  void def(const char* key, bool& value);
  void def(const char* key, ConfigSerializer& sub_obj);

  // Strict read helpers for schemas whose values may be edited outside the
  // firmware. Unlike the legacy def() overloads, these reject duplicate keys,
  // truncated strings, malformed integers, and integer overflow. `seen` must
  // be a field owned by the schema object and initialized false before load.
  bool defStrict(const char* key, char* value, size_t max_len, bool& seen);
  bool defStrict(const char* key, int32_t& value, bool& seen);

  // Literal keys are checked where a schema defines them, so adding a field
  // that an older ConfigSerializer cannot tokenize is a build failure rather
  // than a silent downgrade trap. Dynamic keys retain the pointer overloads.
  template <size_t N> static void checkKey(const char (&)[N]) {
    static_assert(N <= CONFIG_MAX_KEYLEN,
                  "ConfigSerializer key exceeds the visible-key limit");
  }
  template <size_t N> void def(const char (&key)[N], char* value, size_t max_len) {
    checkKey(key); def(static_cast<const char*>(key), value, max_len);
  }
  template <size_t N> void def(const char (&key)[N], void* value, size_t len) {
    checkKey(key); def(static_cast<const char*>(key), value, len);
  }
  template <size_t N> void def(const char (&key)[N], int32_t& value) {
    checkKey(key); def(static_cast<const char*>(key), value);
  }
  template <size_t N> void def(const char (&key)[N], int16_t& value) {
    checkKey(key); def(static_cast<const char*>(key), value);
  }
  template <size_t N> void def(const char (&key)[N], int8_t& value) {
    checkKey(key); def(static_cast<const char*>(key), value);
  }
  template <size_t N> void def(const char (&key)[N], uint32_t& value) {
    checkKey(key); def(static_cast<const char*>(key), value);
  }
  template <size_t N> void def(const char (&key)[N], uint16_t& value) {
    checkKey(key); def(static_cast<const char*>(key), value);
  }
  template <size_t N> void def(const char (&key)[N], uint8_t& value) {
    checkKey(key); def(static_cast<const char*>(key), value);
  }
  template <size_t N> void def(const char (&key)[N], float& value) {
    checkKey(key); def(static_cast<const char*>(key), value);
  }
  template <size_t N> void def(const char (&key)[N], double& value) {
    checkKey(key); def(static_cast<const char*>(key), value);
  }
  template <size_t N> void def(const char (&key)[N], bool& value) {
    checkKey(key); def(static_cast<const char*>(key), value);
  }
  template <size_t N> void def(const char (&key)[N], ConfigSerializer& sub_obj) {
    checkKey(key); def(static_cast<const char*>(key), sub_obj);
  }
  template <size_t N>
  bool defStrict(const char (&key)[N], char* value, size_t max_len, bool& seen) {
    checkKey(key);
    return defStrict(static_cast<const char*>(key), value, max_len, seen);
  }
  template <size_t N>
  bool defStrict(const char (&key)[N], int32_t& value, bool& seen) {
    checkKey(key);
    return defStrict(static_cast<const char*>(key), value, seen);
  }

  virtual void structure() = 0;

public:
  bool loadSerial(Stream& s);
  bool saveSerial(Stream& s);
};
