#include <gtest/gtest.h>

#include <queue>
#include <vector>

#include "KissModem.h"

class SimpleStream : public Stream {
public:
  void pushRx(const std::vector<uint8_t>& bytes) {
    for (uint8_t b : bytes) {
      _rx.push(b);
    }
  }

  const std::vector<uint8_t>& writesSnapshot() const { return _writes; }

  int availableForWrite() override { return 4096; }

  size_t write(const uint8_t* buffer, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      _writes.push_back(buffer[i]);
    }
    return size;
  }

  size_t write(uint8_t b) override { return write(&b, 1); }

  int available() override { return static_cast<int>(_rx.size()); }

  int read() override {
    if (_rx.empty()) return -1;
    int b = _rx.front();
    _rx.pop();
    return b;
  }

private:
  std::queue<uint8_t> _rx;
  std::vector<uint8_t> _writes;
};

class FakeRNG : public mesh::RNG {
public:
  void random(uint8_t* dest, size_t sz) override {
    for (size_t i = 0; i < sz; i++) {
      dest[i] = (uint8_t)i;
    }
  }
};

class FakeRadio : public mesh::Radio {
public:
  bool isReceiving() override { return false; }
  uint32_t getEstAirtimeFor(uint16_t) override { return 10; }
  bool startSendRaw(const uint8_t*, uint16_t) override { return true; }
  bool isSendComplete() override { return true; }
  void onSendFinished() override {}
  int16_t getNoiseFloor() override { return -120; }
};

class FakeBoard : public mesh::MainBoard {
public:
  uint16_t getBattMilliVolts() override { return 4200; }
  float getMCUTemperature() override { return 24.0f; }
  const char* getManufacturerName() override { return "test-board"; }
  void reboot() override {}
};

class FakeSensors : public SensorManager {
public:
  bool querySensors(uint8_t, CayenneLPP&) override { return false; }
};

static std::vector<uint8_t> hwFrame(uint8_t sub_cmd, const std::vector<uint8_t>& payload = {}) {
  std::vector<uint8_t> frame = {KISS_FEND, KISS_CMD_SETHARDWARE, sub_cmd};
  frame.insert(frame.end(), payload.begin(), payload.end());
  frame.push_back(KISS_FEND);
  return frame;
}

class KissModemNoCryptoFixture : public ::testing::Test {
protected:
  SimpleStream serial;
  mesh::LocalIdentity identity;
  FakeRNG rng;
  FakeRadio radio;
  FakeBoard board;
  FakeSensors sensors;
  KissModem modem;

  KissModemNoCryptoFixture()
    : modem(serial, identity, rng, radio, board, sensors) {
    modem.begin();
  }
};

TEST_F(KissModemNoCryptoFixture, PingStillWorksWhenCryptoDisabled) {
  serial.pushRx(hwFrame(HW_CMD_PING));
  modem.loop();

  const std::vector<uint8_t> expected = {
      KISS_FEND, KISS_CMD_SETHARDWARE, HW_RESP(HW_CMD_PING), KISS_FEND};
  EXPECT_EQ(serial.writesSnapshot(), expected);
}

TEST_F(KissModemNoCryptoFixture, GetRadioStillWorksWhenCryptoDisabled) {
  serial.pushRx(hwFrame(HW_CMD_GET_RADIO));
  modem.loop();

  std::vector<uint8_t> expected = {KISS_FEND, KISS_CMD_SETHARDWARE, HW_RESP(HW_CMD_GET_RADIO)};
  expected.insert(expected.end(), 10, 0x00);  // zero-initialized RadioConfig
  expected.push_back(KISS_FEND);
  EXPECT_EQ(serial.writesSnapshot(), expected);
}

TEST_F(KissModemNoCryptoFixture, GetRandomStillWorksWhenCryptoDisabled) {
  serial.pushRx(hwFrame(HW_CMD_GET_RANDOM, {0x04}));
  modem.loop();

  const std::vector<uint8_t> expected = {
      KISS_FEND, KISS_CMD_SETHARDWARE, HW_RESP(HW_CMD_GET_RANDOM), 0x00, 0x01, 0x02, 0x03, KISS_FEND};
  EXPECT_EQ(serial.writesSnapshot(), expected);
}

class KissModemDisabledCommandTest : public ::testing::TestWithParam<uint8_t> {
protected:
  SimpleStream serial;
  mesh::LocalIdentity identity;
  FakeRNG rng;
  FakeRadio radio;
  FakeBoard board;
  FakeSensors sensors;
  KissModem modem;

  KissModemDisabledCommandTest()
    : modem(serial, identity, rng, radio, board, sensors) {
    modem.begin();
  }
};

TEST_P(KissModemDisabledCommandTest, ReturnsUnknownCommandWhenCryptoDisabled) {
  serial.pushRx(hwFrame(GetParam()));
  modem.loop();

  const std::vector<uint8_t> expected = {
      KISS_FEND, KISS_CMD_SETHARDWARE, HW_RESP_ERROR, HW_ERR_UNKNOWN_CMD, KISS_FEND};
  EXPECT_EQ(serial.writesSnapshot(), expected);
}

INSTANTIATE_TEST_SUITE_P(
    AllCryptoCommands, KissModemDisabledCommandTest,
    ::testing::Values(
        HW_CMD_GET_IDENTITY, HW_CMD_VERIFY_SIGNATURE, HW_CMD_SIGN_DATA, HW_CMD_ENCRYPT_DATA,
        HW_CMD_DECRYPT_DATA, HW_CMD_KEY_EXCHANGE, HW_CMD_HASH));

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
