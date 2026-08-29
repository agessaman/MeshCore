#include <gtest/gtest.h>
#include "helpers/ConfigSerializer.h"
#include "helpers/DynamicConfigSerializer.h"

class NativeFileSystem {
public:
    void mkdir(const char*) { }
};
#define FILESYSTEM NativeFileSystem
#include "helpers/CommonCLI.h"
#undef FILESYSTEM

#define TEST_INT_S  "56"
#define TEST_INT     56
#define TEST_FLOAT_S  "-6.123"
#define TEST_FLOAT     -6.1230f
#define TEST_DOUBLE_S "12.123456"
#define TEST_DOUBLE    12.123456

class MockInputStream : public Stream {
    const char* _text;
    int pos, len;
public:
    MockInputStream(const char* text) : _text(text) { pos = 0; len = strlen(text); }
    int available() override { return len - pos; }
    int read() override { if (pos < len) { return _text[pos++]; } return -1; }
    int peek() override { if (pos < len) { return _text[pos]; } return -1; }
};

class MockPrintStream : public Stream {
    int len = 0;
    uint8_t _buf[1024];

    size_t printSigned(long long value) {
        char text[24];
        snprintf(text, sizeof(text), "%lld", value);
        return Print::print(text);
    }

    size_t printUnsigned(unsigned long long value) {
        char text[24];
        snprintf(text, sizeof(text), "%llu", value);
        return Print::print(text);
    }

public:
    size_t write(uint8_t b) override {
        if (len < sizeof(_buf)) {
            _buf[len++] = b;
            return 1;
        }
        return 0;
    }

    size_t print(unsigned char v, int r) override { return printUnsigned(v); }
    size_t print(int v, int r) override { return printSigned(v); }
    size_t print(unsigned int v, int r) override { return printUnsigned(v); }
    size_t print(long v, int r) override { return printSigned(v); }
    size_t print(unsigned long v, int r) override { return printUnsigned(v); }
    size_t print(long long v, int r) override { return printSigned(v); }
    size_t print(unsigned long long v, int r) override { return printUnsigned(v); }
    size_t print(double v, int p = 2) override {
        char text[32];
        snprintf(text, sizeof(text), "%.*f", p, v);
        return Print::print(text);
    }

    int getLength() const { return len; }
    const uint8_t* getBytes() const { return _buf; }
};

class TestStruct : public ConfigSerializer {
  protected:
    void structure() override {
        def("age", age);
        def("flags", flags);
        def("name", name, sizeof(name));
    }
  public:
    int32_t age;
    char    name[16];
    uint8_t flags;
};

// ── saveSerial: basic ───────────────────────────────────────────────────────

TEST(ConfigSerializer, SaveSerial_Basic) {
    MockPrintStream s;
    TestStruct data;

    data.age = TEST_INT;
    data.flags = TEST_INT;
    strcpy(data.name, "Scott");

    bool success = data.saveSerial(s);
    EXPECT_TRUE(success);

    auto l = s.getLength();
    const char* expect = "{age:" TEST_INT_S ",flags:" TEST_INT_S ",name:\"Scott\"}";
    EXPECT_EQ(strlen(expect), l);

    bool match = memcmp(s.getBytes(), expect, l) == 0;
    EXPECT_TRUE(match);
}


TEST(ConfigSerializer, SaveSerial_EscChars) {
    MockPrintStream s;
    TestStruct data;

    data.age = TEST_INT;
    data.flags = TEST_INT;
    strcpy(data.name, "\"Scott\"\n");

    bool success = data.saveSerial(s);
    EXPECT_TRUE(success);

    auto l = s.getLength();
    const char* expect = "{age:" TEST_INT_S ",flags:" TEST_INT_S ",name:\"\\\"Scott\\\"\\n\"}";
    EXPECT_EQ(strlen(expect), l);

    bool match = memcmp(s.getBytes(), expect, l) == 0;
    EXPECT_TRUE(match);
}

// ── loadSerial: basic ───────────────────────────────────────────────────────

TEST(ConfigSerializer, LoadSerial_Basic) {
    MockInputStream s("{age:" TEST_INT_S ",flags:" TEST_INT_S ",name:\"Scott\"}");
    TestStruct data;

    bool success = data.loadSerial(s);
    EXPECT_TRUE(success);

    EXPECT_EQ(TEST_INT, data.age);
    EXPECT_EQ(TEST_INT, data.flags);
    bool match = strcmp("Scott", data.name) == 0;
    EXPECT_TRUE(match);
}

TEST(ConfigSerializer, LoadSerial_HandleWhitespace) {
    MockInputStream s("  { age:  " TEST_INT_S " ,  flags:  " TEST_INT_S " ,  name:  \"Scott\" }  ");
    TestStruct data;

    bool success = data.loadSerial(s);
    EXPECT_TRUE(success);

    EXPECT_EQ(TEST_INT, data.age);
    EXPECT_EQ(TEST_INT, data.flags);
    bool match = strcmp("Scott", data.name) == 0;
    EXPECT_TRUE(match);
}

TEST(ConfigSerializer, LoadSerial_EscChars) {
    MockInputStream s("{age:" TEST_INT_S ",flags:" TEST_INT_S ",name:\"\\\"Scott\\\"\\n\"}");
    TestStruct data;

    bool success = data.loadSerial(s);
    EXPECT_TRUE(success);

    bool match = strcmp("\"Scott\"\n", data.name) == 0;
    EXPECT_TRUE(match);
}

TEST(ConfigSerializer, LoadSerial_UnmatchedBraces) {
    MockInputStream s("{age:" TEST_INT_S ",flags:" TEST_INT_S ",name:\"Scott\"");
    TestStruct data;

    bool success = data.loadSerial(s);
    EXPECT_FALSE(success);
}

TEST(ConfigSerializer, LoadSerial_MissingCommas) {
    MockInputStream s("{age:" TEST_INT_S " flags:" TEST_INT_S " name:\"Scott\"}");
    TestStruct data;

    bool success = data.loadSerial(s);
    EXPECT_FALSE(success);
}

TEST(ConfigSerializer, LoadSerial_IgnoreUnknowns) {
    MockInputStream s("{age:" TEST_INT_S ",xxx:" TEST_INT_S ",name:\"Scott\"}");
    TestStruct data;
    data.flags = 1;

    // should ignore the 'xxx' property
    bool success = data.loadSerial(s);
    EXPECT_TRUE(success);

    EXPECT_EQ(TEST_INT, data.age);
    EXPECT_EQ(1, data.flags);   // flags should be unmodified
    bool match = strcmp("Scott", data.name) == 0;
    EXPECT_TRUE(match);
}

TEST(DynamicConfigSerializer, GetSet_Basic) {
    DynamicConfigSerializer data;

    bool s1 = data.setByKey("age", "11");
    bool s2 = data.setByKey("name", "Scott");
    EXPECT_TRUE(s1 && s2);

    char tmp[32];
    bool g1 = data.getByKey("age", tmp, 31);
    EXPECT_TRUE(g1);
    EXPECT_STREQ("11", tmp);

    bool g2 = data.getByKey("name", tmp, 31);
    EXPECT_TRUE(g2);
    EXPECT_STREQ("Scott", tmp);
}

TEST(NodePrefs, FemGainSettingsRoundTrip) {
    NodePrefs saved;
    saved.radio_fem_rxgain = 0;
    saved.radio_fem_txgain = 1;

    MockPrintStream output;
    ASSERT_TRUE(saved.saveSerial(output));

    std::string serialised(reinterpret_cast<const char*>(output.getBytes()), output.getLength());
    EXPECT_NE(std::string::npos, serialised.find("fem_rxgain:0"));
    EXPECT_NE(std::string::npos, serialised.find("fem_txgain:1"));

    MockInputStream input(serialised.c_str());
    NodePrefs loaded;
    loaded.radio_fem_rxgain = 1;
    loaded.radio_fem_txgain = 0;

    ASSERT_TRUE(loaded.loadSerial(input)) << serialised;
    EXPECT_EQ(0, loaded.radio_fem_rxgain);
    EXPECT_EQ(1, loaded.radio_fem_txgain);
}

TEST(NodePrefs, TxPowerRemainsSignedThroughRadioPrefs) {
    NodePrefs prefs;
    prefs.tx_power_dbm = -9;

    EXPECT_EQ(-9, prefs.getRadioPrefs()->getTxPower());
    prefs.getRadioPrefs()->setTxPower(-8);
    EXPECT_EQ(-8, prefs.tx_power_dbm);
}

TEST(ConfigSerializer, LoadSerial_KeyDigitsAfterFirstCharacter) {
    MockInputStream s("{age:1,flags:2,name:\"ok\",slot1:7}");
    TestStruct data;
    data.age = data.flags = 0;
    strcpy(data.name, "before");
    EXPECT_TRUE(data.loadSerial(s));
    EXPECT_EQ(1, data.age);
    EXPECT_EQ(2, data.flags);
    EXPECT_STREQ("ok", data.name);
}

TEST(ConfigSerializer, LoadSerial_RejectsLeadingDigitKey) {
    MockInputStream s("{1slot:7}");
    TestStruct data;
    EXPECT_FALSE(data.loadSerial(s));
}

TEST(ConfigSerializer, LoadSerial_AcceptsEmptyObject) {
    MockInputStream s("{}");
    TestStruct data;
    EXPECT_TRUE(data.loadSerial(s));
}

// ── /prefs.json compatibility under the strict shape checks ─────────────────
//
// The scalar-vs-object rejection added for /mqtt.json also runs for the plain
// def() overloads that NodePrefs uses, and loadPrefsInt() applies /prefs.json
// straight onto the live object without consulting the return value. These
// pin what that combination does to files already on deployed devices.

// A settings file in the exact shape the firmware writes, transcribed rather
// than produced by saveSerial() so a change to the writer cannot quietly move
// the fixture with it.
static const char* DEPLOYED_PREFS_JSON =
    "{name:\"Repeater-1\",pass:\"hunter2\",guest:\"\",owner:\"ops@example.com\","
    "adv_int:4,f_adv_int:12,lat:47.612345,lon:-122.334567,disc_mod:1719791234,"
    "radio:{freq:910.5250,bw:250.0000,sf:10,cr:5,cad:0,int_thr:0,rxgain:1,"
    "fem_rxgain:0,fem_txgain:1,tx:22,af:1.0000,rxdelay:1000.0000,"
    "f_txdelay:0.5000,d_txdelay:0.2000,agc_int:0,hash_mode:0,multi_ack:0},"
    "bridge:{en:0,delay:500,src:1,baud:115200,ch:1,secret:\"\"},"
    "gps:{en:0,int:60,adv_loc:0},"
    "repeat:{disable:0,f_max:64,f_max_uns:32,f_max_adv:16,loop:1},"
    "room:{rd_only:0},power:{adc_mult:1.0000,pwr_sav_en:0}}";

TEST(NodePrefs, DeployedPrefsJsonStillLoadsCompletely) {
    MockInputStream s(DEPLOYED_PREFS_JSON);
    NodePrefs prefs;

    ASSERT_TRUE(prefs.loadSerial(s));
    EXPECT_STREQ("Repeater-1", prefs.node_name);
    EXPECT_STREQ("ops@example.com", prefs.owner_info);
    EXPECT_EQ(4, prefs.advert_interval);
    EXPECT_DOUBLE_EQ(47.612345, prefs.node_lat);
    EXPECT_DOUBLE_EQ(-122.334567, prefs.node_lon);
    EXPECT_EQ(1719791234u, prefs.discovery_mod_timestamp);
    EXPECT_FLOAT_EQ(910.525f, prefs.freq);
    EXPECT_EQ(10, prefs.sf);
    EXPECT_EQ(22, prefs.tx_power_dbm);
    EXPECT_EQ(1, prefs.radio_fem_txgain);
    EXPECT_EQ(1, prefs.bridge_pkt_src);
    EXPECT_EQ(115200u, prefs.bridge_baud);
    EXPECT_EQ(60u, prefs.gps_interval);
    EXPECT_EQ(64, prefs.flood_max);
    EXPECT_EQ(1, prefs.loop_detect);
}

TEST(NodePrefs, UnknownNestedObjectCannotBeMistakenForARootScalar) {
    // Depth is what keeps an unknown group's inner keys from matching a root
    // field of the same name, so an unrecognized section must stay ignorable.
    MockInputStream s("{name:\"Repeater-1\",mqtt:{name:\"other\",owner:\"nobody\"},adv_int:4}");
    NodePrefs prefs;
    strcpy(prefs.owner_info, "ops@example.com");

    EXPECT_TRUE(prefs.loadSerial(s));
    EXPECT_STREQ("Repeater-1", prefs.node_name);
    EXPECT_STREQ("ops@example.com", prefs.owner_info);
    EXPECT_EQ(4, prefs.advert_interval);
}

TEST(NodePrefs, TornPrefsJsonAppliesOnlyTheFieldsBeforeTheTear) {
    // A power cut during the old non-transactional /prefs.json write leaves a
    // file like this. loadPrefsInt() ignores the failed return and keeps the
    // partial result, which is the pre-existing behavior; the strict checks
    // must not turn it into something worse than a partial load.
    MockInputStream s("{name:\"Repeater-1\",adv_int:4,radio:{freq:910.5250,sf:1");
    NodePrefs prefs;
    prefs.sf = 9;

    EXPECT_FALSE(prefs.loadSerial(s));
    EXPECT_STREQ("Repeater-1", prefs.node_name);
    EXPECT_EQ(4, prefs.advert_interval);
    EXPECT_FLOAT_EQ(910.525f, prefs.freq);
    EXPECT_EQ(9, prefs.sf);  // the torn value never completed a token
}

TEST(NodePrefs, ShapeMismatchIsRejectedAndStopsFurtherApplication) {
    // Hand-edited or corrupted files are the regression surface for the strict
    // checks: a known scalar holding an object now fails the load and stops
    // parsing, so nothing after the mismatch reaches the live object.
    MockInputStream scalar_as_object("{name:{x:1},adv_int:4}");
    NodePrefs prefs;
    prefs.advert_interval = 7;
    EXPECT_FALSE(prefs.loadSerial(scalar_as_object));
    EXPECT_EQ(7, prefs.advert_interval);

    // The reverse mismatch is rejected too: a known group given a scalar.
    MockInputStream object_as_scalar("{name:\"Repeater-1\",radio:1}");
    NodePrefs scalar_group;
    EXPECT_FALSE(scalar_group.loadSerial(object_as_scalar));

    // And a known scalar nested one level deeper than the schema places it.
    MockInputStream over_nested("{radio:{sf:{value:10}}}");
    NodePrefs nested;
    nested.sf = 9;
    EXPECT_FALSE(nested.loadSerial(over_nested));
    EXPECT_EQ(9, nested.sf);
}

TEST(DynamicConfigSerializer, Set_Replaces) {
    DynamicConfigSerializer data;

    bool s1 = data.setByKey("age", "11");
    bool s2 = data.setByKey("name", "Scott");
    EXPECT_TRUE(s1 && s2);

    bool s3 = data.setByKey("age", "333");
    EXPECT_TRUE(s3);

    char tmp[32];
    bool g1 = data.getByKey("age", tmp, 31);
    EXPECT_TRUE(g1);
    EXPECT_STREQ("333", tmp);

    bool g2 = data.getByKey("name", tmp, 31);
    EXPECT_TRUE(g2);
    EXPECT_STREQ("Scott", tmp);
}

TEST(DynamicConfigSerializer, GetUnknown_Fail) {
    DynamicConfigSerializer data;

    bool s1 = data.setByKey("age", "11");
    EXPECT_TRUE(s1);

    char tmp[32];
    bool g2 = data.getByKey("name", tmp, 31);
    EXPECT_FALSE(g2);
}

TEST(DynamicConfigSerializer, SaveCustom_Basic) {
    MockPrintStream s;
    DynamicConfigSerializer data;

    bool s1 = data.setByKey("age", "11");
    bool s2 = data.setByKey("name", "Scott");
    EXPECT_TRUE(s1 && s2);

    bool success = data.saveSerial(s);
    EXPECT_TRUE(success);

    auto l = s.getLength();
    char tmp[128];
    memcpy(tmp, s.getBytes(), l);
    tmp[l] = 0;

    const char* expect = "{age:\"11\",name:\"Scott\"}";
    EXPECT_STREQ(expect, tmp);
}

TEST(DynamicConfigSerializer, LoadCustom_Basic) {
    MockInputStream s("{age:\"" TEST_INT_S "\",name:\"Scott\"}");
    DynamicConfigSerializer data;

    bool success = data.loadSerial(s);
    EXPECT_TRUE(success);

    char tmp[32];
    bool g1 = data.getByKey("age", tmp, 31);
    EXPECT_TRUE(g1);
    EXPECT_STREQ(TEST_INT_S, tmp);

    bool g2 = data.getByKey("name", tmp, 31);
    EXPECT_TRUE(g2);
    EXPECT_STREQ("Scott", tmp);
}

// ── main ───────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
