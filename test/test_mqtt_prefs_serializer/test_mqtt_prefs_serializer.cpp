#include <gtest/gtest.h>

#include <string>

#define WITH_MQTT_BRIDGE 1
#define PROGMEM
#include "helpers/MQTTPrefsSerializer.h"

class InputStream : public Stream {
public:
  explicit InputStream(const std::string& text) : _text(text) {}
  int available() override { return static_cast<int>(_text.size() - _pos); }
  int read() override { return _pos < _text.size() ? _text[_pos++] : -1; }
  int peek() override { return _pos < _text.size() ? _text[_pos] : -1; }
private:
  std::string _text;
  size_t _pos = 0;
};

class OutputStream : public Stream {
public:
  size_t write(uint8_t byte) override { _text.push_back(static_cast<char>(byte)); return 1; }
  size_t print(int value, int = DEC) override { return appendNumber(value); }
  size_t print(unsigned int value, int = DEC) override { return appendNumber(value); }
  size_t print(long value, int = DEC) override { return appendNumber(value); }
  size_t print(unsigned long value, int = DEC) override { return appendNumber(value); }
  size_t print(long long value, int = DEC) override { return appendNumber(value); }
  size_t print(unsigned long long value, int = DEC) override { return appendNumber(value); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  const std::string& text() const { return _text; }
private:
  template <typename T> size_t appendNumber(T value) {
    const std::string number = std::to_string(value);
    _text += number;
    return number.size();
  }
  std::string _text;
};

class StickyFailingStream : public Stream {
public:
  explicit StickyFailingStream(size_t limit) : _limit(limit) {}
  size_t write(uint8_t) override {
    if (_failed || _written >= _limit) {
      _failed = true;
      return 0;
    }
    ++_written;
    return 1;
  }
  size_t print(int value, int = DEC) override {
    const std::string number = std::to_string(value);
    return Print::print(number.c_str());
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  bool failed() const { return _failed; }
private:
  size_t _limit;
  size_t _written = 0;
  bool _failed = false;
};

static MQTTPrefs defaults() {
  MQTTPrefs prefs = {};
  prefs.mqtt_status_enabled = 1;
  prefs.mqtt_packets_enabled = 1;
  prefs.mqtt_tx_enabled = 2;
  prefs.mqtt_rx_enabled = 1;
  prefs.mqtt_status_interval = 300000;
  prefs.wifi_power_save = 1;
  prefs.timezone_offset = -7;
  prefs.radio_watchdog_minutes = 5;
  prefs.alert_wifi_minutes = 30;
  prefs.alert_mqtt_minutes = 240;
  prefs.alert_min_interval_min = 60;
  prefs.mqtt_neighbors_interval = MQTT_NEIGHBORS_DEFAULT_INTERVAL_MS;
  prefs.display_timeout_secs = DISPLAY_TIMEOUT_DEFAULT_SECS;
  strcpy(prefs.snmp_community, "public");
  for (int i = 0; i < MQTT_PREFS_SLOT_COUNT; ++i) {
    strcpy(prefs.mqtt_slot_preset[i], "none");
    prefs.mqtt_slot_packet_filter[i] = 0xffff;
  }
  return prefs;
}

TEST(MQTTPrefsSerializer, RoundTripsEveryGroupAndNumericSlotKeys) {
  MQTTPrefs source = defaults();
  strcpy(source.wifi_ssid, "mesh-net");
  strcpy(source.wifi_password, "p\\\"ass\nword");
  strcpy(source.timezone_string, "MST7MDT,M3.2.0");
  strcpy(source.mqtt_ntp_server, "time.example");
  strcpy(source.mqtt_origin, "observer-one");
  strcpy(source.mqtt_iata, "SEA");
  source.mqtt_neighbors_enabled = 1;
  source.mqtt_neighbors_interval = MQTT_NEIGHBORS_MAX_INTERVAL_MS;
  strcpy(source.mqtt_owner_public_key,
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  strcpy(source.mqtt_email, "owner@example.com");
  strcpy(source.mqtt_slot_preset[5], "custom");
  strcpy(source.mqtt_slot_host[5], "broker.example");
  source.mqtt_slot_port[5] = 65535;
  strcpy(source.mqtt_slot_username[5], "user-six");
  strcpy(source.mqtt_slot_password[5], "secret-six");
  strcpy(source.mqtt_slot_token[5], "token-six");
  strcpy(source.mqtt_slot_topic[5], "mesh/{iata}/{type}");
  strcpy(source.mqtt_slot_audience[5], "audience-six");
  source.mqtt_slot_packet_filter[5] = 0x8001;
  source.snmp_enabled = 1;
  source.radio_watchdog_minutes = 120;
  source.alert_enabled = 1;
  strcpy(source.alert_psk_hex, "0123456789abcdef0123456789abcdef");
  strcpy(source.alert_hashtag, "#ops");
  strcpy(source.alert_region, "PNW");

  OutputStream output;
  MQTTPrefsSerializer writer(&source);
  ASSERT_TRUE(writer.saveSerial(output));
  EXPECT_NE(std::string::npos, output.text().find("slot6:{")) << output.text();
  EXPECT_NE(std::string::npos, output.text().find("packet_filter:32769")) << output.text();

  MQTTPrefs loaded = defaults();
  InputStream input(output.text());
  MQTTPrefsSerializer reader(&loaded);
  ASSERT_TRUE(reader.loadSerial(input)) << output.text();
  bool repaired = true;
  ASSERT_TRUE(reader.apply(&repaired));
  EXPECT_FALSE(repaired);
  EXPECT_STREQ(source.wifi_password, loaded.wifi_password);
  EXPECT_STREQ(source.mqtt_owner_public_key, loaded.mqtt_owner_public_key);
  EXPECT_STREQ(source.mqtt_slot_host[5], loaded.mqtt_slot_host[5]);
  EXPECT_EQ(65535, loaded.mqtt_slot_port[5]);
  EXPECT_EQ(0x8001, loaded.mqtt_slot_packet_filter[5]);
  EXPECT_EQ(MQTT_NEIGHBORS_MAX_INTERVAL_MS, loaded.mqtt_neighbors_interval);
  EXPECT_STREQ("PNW", loaded.alert_region);
}

TEST(MQTTPrefsSerializer, MissingOptionalKeysKeepDefaults) {
  MQTTPrefs prefs = defaults();
  InputStream input("{version:1,mqtt:{origin:\"changed\"}}");
  MQTTPrefsSerializer serializer(&prefs);
  ASSERT_TRUE(serializer.loadSerial(input));
  bool repaired = true;
  ASSERT_TRUE(serializer.apply(&repaired));
  EXPECT_FALSE(repaired);
  EXPECT_STREQ("changed", prefs.mqtt_origin);
  EXPECT_EQ(1, prefs.mqtt_packets_enabled);
  EXPECT_EQ(300000u, prefs.mqtt_status_interval);
  EXPECT_EQ(0xffff, prefs.mqtt_slot_packet_filter[5]);
}

TEST(MQTTPrefsSerializer, RequiresSupportedVersion) {
  MQTTPrefs missing = defaults();
  InputStream no_version("{wifi:{ssid:\"x\"}}");
  MQTTPrefsSerializer missing_serializer(&missing);
  ASSERT_TRUE(missing_serializer.loadSerial(no_version));
  bool repaired = false;
  EXPECT_FALSE(missing_serializer.apply(&repaired));

  MQTTPrefs future = defaults();
  InputStream future_input("{version:2,wifi:{ssid:\"x\"}}");
  MQTTPrefsSerializer future_serializer(&future);
  ASSERT_TRUE(future_serializer.loadSerial(future_input));
  EXPECT_TRUE(future_serializer.hasFutureVersion());
  EXPECT_FALSE(future_serializer.apply(&repaired));
}

TEST(MQTTPrefsSerializer, FutureVersionProbeIgnoresV1FieldTypeChanges) {
  InputStream probe_input("{version:2,wifi:{power_save:\"max\"}}");
  MQTTPrefsVersionProbe probe;
  ASSERT_TRUE(probe.loadSerial(probe_input));
  EXPECT_TRUE(probe.hasFutureVersion());

  // The same image is not valid under v1, demonstrating why recovery must
  // probe the version before invoking the current schema.
  MQTTPrefs prefs = defaults();
  InputStream v1_input("{version:2,wifi:{power_save:\"max\"}}");
  MQTTPrefsSerializer v1(&prefs);
  EXPECT_FALSE(v1.loadSerial(v1_input));

  for (const char* future_text : {
           "{version:2,this_key_is_too_long:1}",
           "{version:2,x:[1]}",
           "{version:2,wifi:{ssid:\"torn\"}"}) {
    InputStream future_input(future_text);
    MQTTPrefsVersionProbe future_probe;
    EXPECT_FALSE(future_probe.loadSerial(future_input)) << future_text;
    EXPECT_TRUE(future_probe.hasFutureVersion()) << future_text;
  }
}

TEST(MQTTPrefsSerializer, VersionIsWrittenAsTheFirstRootProperty) {
  MQTTPrefs prefs = defaults();
  MQTTPrefsSerializer writer(&prefs);
  OutputStream output;
  ASSERT_TRUE(writer.saveSerial(output));
  EXPECT_EQ(0u, output.text().find("{version:1,")) << output.text();
}

TEST(MQTTPrefsSerializer, FutureGrammarAheadOfVersionCannotBeRecognizedAsFuture) {
  // Why the version-first invariant is part of the format rather than a style
  // preference. These two files differ only in key order, and only the
  // compliant one keeps its preservation guarantee on this firmware.
  InputStream compliant("{version:2,x:[1]}");
  MQTTPrefsVersionProbe compliant_probe;
  EXPECT_FALSE(compliant_probe.loadSerial(compliant));
  EXPECT_TRUE(compliant_probe.hasFutureVersion());

  InputStream violating("{x:[1],version:2}");
  MQTTPrefsVersionProbe violating_probe;
  EXPECT_FALSE(violating_probe.loadSerial(violating));
  EXPECT_FALSE(violating_probe.hasFutureVersion());
}

TEST(MQTTPrefsSerializer, RejectsDuplicateKnownKey) {
  MQTTPrefs prefs = defaults();
  InputStream input("{version:1,mqtt:{origin:\"one\",origin:\"two\"}}");
  MQTTPrefsSerializer serializer(&prefs);
  EXPECT_FALSE(serializer.loadSerial(input));
}

TEST(MQTTPrefsSerializer, RejectsOverlongStringAndIntegerOverflow) {
  MQTTPrefs prefs = defaults();
  InputStream long_string(
      "{version:1,wifi:{ssid:\"12345678901234567890123456789012\"}}");
  MQTTPrefsSerializer string_serializer(&prefs);
  EXPECT_FALSE(string_serializer.loadSerial(long_string));

  prefs = defaults();
  InputStream overflow("{version:1,mqtt:{slot1:{port:999999999999}}}");
  MQTTPrefsSerializer number_serializer(&prefs);
  EXPECT_FALSE(number_serializer.loadSerial(overflow));

  prefs = defaults();
  InputStream quoted_number("{version:\"1\"}");
  MQTTPrefsSerializer quoted_number_serializer(&prefs);
  EXPECT_FALSE(quoted_number_serializer.loadSerial(quoted_number));

  prefs = defaults();
  InputStream bare_string("{version:1,wifi:{ssid:meshnet}}");
  MQTTPrefsSerializer bare_string_serializer(&prefs);
  EXPECT_FALSE(bare_string_serializer.loadSerial(bare_string));
}

TEST(MQTTPrefsSerializer, RejectsScalarObjectShapeMismatches) {
  MQTTPrefs prefs = defaults();
  InputStream object_version("{version:{x:1}}");
  MQTTPrefsSerializer object_version_serializer(&prefs);
  EXPECT_FALSE(object_version_serializer.loadSerial(object_version));

  prefs = defaults();
  InputStream object_port("{version:1,mqtt:{slot1:{port:{x:1883}}}}");
  MQTTPrefsSerializer object_port_serializer(&prefs);
  EXPECT_FALSE(object_port_serializer.loadSerial(object_port));

  prefs = defaults();
  InputStream scalar_mqtt("{version:1,mqtt:1}");
  MQTTPrefsSerializer scalar_mqtt_serializer(&prefs);
  EXPECT_FALSE(scalar_mqtt_serializer.loadSerial(scalar_mqtt));

  prefs = defaults();
  InputStream scalar_slot("{version:1,mqtt:{slot1:1}}");
  MQTTPrefsSerializer scalar_slot_serializer(&prefs);
  EXPECT_FALSE(scalar_slot_serializer.loadSerial(scalar_slot));
}

TEST(MQTTPrefsSerializer, RepairsSemanticRanges) {
  MQTTPrefs prefs = defaults();
  InputStream input(
      "{version:1,wifi:{power_save:9},time:{utc_offset:99},"
      "mqtt:{tx_enabled:7,status:{enabled:3,interval_ms:10},"
      "neighbors:{enabled:2,interval_ms:100},slot1:{port:-1,packet_filter:-2}},"
      "radio:{watchdog_min:121},alert:{rate_limit_min:1}}");
  MQTTPrefsSerializer serializer(&prefs);
  ASSERT_TRUE(serializer.loadSerial(input));
  bool repaired = false;
  ASSERT_TRUE(serializer.apply(&repaired));
  EXPECT_TRUE(repaired);
  EXPECT_EQ(1, prefs.wifi_power_save);
  EXPECT_EQ(-7, prefs.timezone_offset);
  EXPECT_EQ(2, prefs.mqtt_tx_enabled);
  EXPECT_EQ(300000u, prefs.mqtt_status_interval);
  EXPECT_EQ(MQTT_NEIGHBORS_DEFAULT_INTERVAL_MS, prefs.mqtt_neighbors_interval);
  EXPECT_EQ(0, prefs.mqtt_slot_port[0]);
  EXPECT_EQ(0xffff, prefs.mqtt_slot_packet_filter[0]);
  EXPECT_EQ(5, prefs.radio_watchdog_minutes);
  EXPECT_EQ(60, prefs.alert_min_interval_min);
}

TEST(MQTTPrefsSerializer, RepairsTextValuesToSafeDefaults) {
  MQTTPrefs prefs = defaults();
  InputStream input(
      "{version:1,time:{ntp_server:\"bad/host\"},mqtt:{iata:\"sea\","
      "owner:{public_key:\"not-a-key\"},slot1:{preset:\"not-a-preset\"},"
      "slot2:{preset:\"analyzer-us\"},slot3:{preset:\"analyzer-us\"}},"
      "alert:{psk_hex:\"not-hex\",hashtag:\"#stale\"}}");
  MQTTPrefsSerializer serializer(&prefs);
  ASSERT_TRUE(serializer.loadSerial(input));
  bool repaired = false;
  ASSERT_TRUE(serializer.apply(&repaired));
  EXPECT_TRUE(repaired);
  EXPECT_STREQ("SEA", prefs.mqtt_iata);
  EXPECT_STREQ("", prefs.mqtt_ntp_server);
  EXPECT_STREQ("", prefs.mqtt_owner_public_key);
  EXPECT_STREQ("none", prefs.mqtt_slot_preset[0]);
  EXPECT_STREQ("analyzer-us", prefs.mqtt_slot_preset[1]);
  // Historical firmware allowed duplicate aliases. Preserve them on load;
  // current setters prevent creating new duplicates without silently changing
  // a deployed configuration during migration.
  EXPECT_STREQ("analyzer-us", prefs.mqtt_slot_preset[2]);
  EXPECT_STREQ("", prefs.alert_psk_hex);
  EXPECT_STREQ("", prefs.alert_hashtag);
}

TEST(MQTTPrefsSerializer, LateParseOrVersionFailureCannotMutateLivePrefs) {
  MQTTPrefs live = defaults();
  strcpy(live.wifi_ssid, "live-network");

  MQTTPrefs scratch = defaults();
  InputStream truncated("{wifi:{ssid:\"uncommitted\"},version:1");
  MQTTPrefsSerializer truncated_serializer(&scratch);
  EXPECT_FALSE(truncated_serializer.loadSerial(truncated));
  EXPECT_STREQ("live-network", live.wifi_ssid);

  scratch = defaults();
  InputStream future("{wifi:{ssid:\"future-network\"},version:2}");
  MQTTPrefsSerializer future_serializer(&scratch);
  ASSERT_TRUE(future_serializer.loadSerial(future));
  EXPECT_TRUE(future_serializer.hasFutureVersion());
  bool repaired = false;
  EXPECT_FALSE(future_serializer.apply(&repaired));
  EXPECT_STREQ("live-network", live.wifi_ssid);
}

TEST(MQTTPrefsSerializer, StickyShortWriteFailsTheCompleteSave) {
  MQTTPrefs prefs = defaults();
  MQTTPrefsSerializer serializer(&prefs);
  StickyFailingStream output(20);
  EXPECT_FALSE(serializer.saveSerial(output));
  EXPECT_TRUE(output.failed());
}

TEST(MQTTPrefsSerializer, SaveNormalizationIsIdempotentAgainstKnownDefaults) {
  MQTTPrefs prefs = defaults();
  prefs.timezone_offset = 99;
  strcpy(prefs.mqtt_ntp_server, "bad/host");
  strcpy(prefs.mqtt_iata, "not-iata");
  strcpy(prefs.mqtt_slot_preset[0], "not-a-preset");

  MQTTPrefs repair_defaults = defaults();
  repair_defaults.timezone_offset = -7;
  strcpy(repair_defaults.mqtt_iata, "sea");
  strcpy(repair_defaults.mqtt_slot_preset[0], "analyzer-us");

  MQTTPrefsSerializer writer(&prefs, &repair_defaults);
  bool repaired = false;
  ASSERT_TRUE(writer.normalize(&repaired));
  EXPECT_TRUE(repaired);
  EXPECT_EQ(-7, prefs.timezone_offset);
  EXPECT_STREQ("", prefs.mqtt_ntp_server);
  EXPECT_STREQ("SEA", prefs.mqtt_iata);
  EXPECT_STREQ("none", prefs.mqtt_slot_preset[0]);

  OutputStream output;
  ASSERT_TRUE(writer.saveSerial(output));

  MQTTPrefs loaded = defaults();
  InputStream input(output.text());
  MQTTPrefsSerializer reader(&loaded);
  ASSERT_TRUE(reader.loadSerial(input));
  repaired = true;
  ASSERT_TRUE(reader.apply(&repaired));
  EXPECT_FALSE(repaired) << output.text();
}

TEST(MQTTPrefsSerializer, DisplayTimeoutRoundTrips) {
  for (uint16_t secs : {(uint16_t)0, (uint16_t)45, DISPLAY_TIMEOUT_MAX_SECS}) {
    MQTTPrefs source = defaults();
    source.display_timeout_secs = secs;

    OutputStream output;
    MQTTPrefsSerializer writer(&source);
    ASSERT_TRUE(writer.saveSerial(output)) << secs;

    MQTTPrefs loaded = defaults();
    InputStream input(output.text());
    MQTTPrefsSerializer reader(&loaded);
    ASSERT_TRUE(reader.loadSerial(input)) << secs;
    bool repaired = false;
    ASSERT_TRUE(reader.apply(&repaired)) << secs;
    EXPECT_FALSE(repaired) << secs;
    EXPECT_EQ(secs, loaded.display_timeout_secs);
  }
}

TEST(MQTTPrefsSerializer, RepairsDisplayTimeoutOutOfRange) {
  MQTTPrefs prefs = defaults();
  InputStream input("{version:1,display:{timeout_s:99999}}");
  MQTTPrefsSerializer serializer(&prefs);
  ASSERT_TRUE(serializer.loadSerial(input));
  bool repaired = false;
  ASSERT_TRUE(serializer.apply(&repaired));
  EXPECT_TRUE(repaired);
  EXPECT_EQ(DISPLAY_TIMEOUT_DEFAULT_SECS, prefs.display_timeout_secs);

  prefs = defaults();
  InputStream negative("{version:1,display:{timeout_s:-5}}");
  MQTTPrefsSerializer negative_serializer(&prefs);
  ASSERT_TRUE(negative_serializer.loadSerial(negative));
  repaired = false;
  ASSERT_TRUE(negative_serializer.apply(&repaired));
  EXPECT_TRUE(repaired);
  EXPECT_EQ(DISPLAY_TIMEOUT_DEFAULT_SECS, prefs.display_timeout_secs);
}

TEST(MQTTPrefsSerializer, PrefsWrittenBeforeTheDisplayGroupStillLoad) {
  // Upgrade path: a /mqtt.json from firmware without the display group must
  // load cleanly and keep the default rather than collapsing to 0 ("stay on").
  MQTTPrefs prefs = defaults();
  InputStream input("{version:1,radio:{watchdog_min:5}}");
  MQTTPrefsSerializer serializer(&prefs);
  ASSERT_TRUE(serializer.loadSerial(input));
  bool repaired = false;
  ASSERT_TRUE(serializer.apply(&repaired));
  EXPECT_EQ(DISPLAY_TIMEOUT_DEFAULT_SECS, prefs.display_timeout_secs);
}

TEST(MQTTPrefsSerializer, UnknownGroupsAreIgnoredSoAppendedKeysAreDowngradeSafe) {
  // The mirror of the case above, and the reason appending `display` needed no
  // MQTT_PREFS_JSON_FORMAT_VERSION bump: firmware that predates a group skips
  // it rather than failing the load.
  MQTTPrefs prefs = defaults();
  InputStream input(
      "{version:1,display:{timeout_s:45},future:{thing:1,nested:{x:2}}}");
  MQTTPrefsSerializer serializer(&prefs);
  ASSERT_TRUE(serializer.loadSerial(input));
  bool repaired = false;
  ASSERT_TRUE(serializer.apply(&repaired));
  EXPECT_EQ(45, prefs.display_timeout_secs);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
