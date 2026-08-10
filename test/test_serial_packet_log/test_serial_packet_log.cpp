// The packet-logging writer exists because USBCDC::write() spins forever on a full TX FIFO
// when the host has the port open but has stopped reading it, wedging the mesh loop with no
// crash and no reboot. These cases pin the two properties that matter: it never waits on a
// host that isn't draining, and a dropped line is reported rather than silently lost.
#include <gtest/gtest.h>

#include <string>

#include "helpers/SerialPacketLog.h"

namespace {

// A CDC-like port with a bounded TX FIFO that refills only as fast as the host reads it.
class FakePort : public Stream {
public:
  explicit FakePort(size_t capacity = 64, size_t bytes_per_ms = 64)
    : _cap(capacity), _free(capacity), _rate(bytes_per_ms), _last_ms(millis()) { }

  int availableForWrite() override {
    uint32_t now = millis();
    _free += (size_t)(now - _last_ms) * _rate;
    _last_ms = now;
    if (_free > _cap) _free = _cap;
    return (int)_free;
  }

  size_t write(const uint8_t* buffer, size_t size) override {
    if (size > _free) size = _free;   // a real port would block here instead
    _written.append((const char*)buffer, size);
    _free -= size;
    if (_written.size() >= _wedge_after) wedge();
    return size;
  }

  // Host stops reading: the FIFO fills and never drains again.
  void wedge() { availableForWrite(); _rate = 0; _free = 0; }
  void wedgeAfter(size_t bytes) { _wedge_after = bytes; }

  const std::string& written() const { return _written; }

private:
  size_t _cap, _free, _rate;
  size_t _wedge_after = (size_t)-1;
  uint32_t _last_ms;
  std::string _written;
};

class SerialPacketLogTest : public ::testing::Test {
protected:
  void SetUp() override {
    g_mock_millis = 1000;
    serialLogDroppedCount() = 0;
    serialLogPortSeen() = false;
  }

  // Drops are only counted once the port has proven it can take output, so tests about
  // drop accounting have to get one line through first.
  static void primePort() {
    FakePort port(4096);
    SerialLogLine<> line;
    line.printf("primed");
    ASSERT_TRUE(line.flush(port));
    ASSERT_TRUE(serialLogPortSeen());
  }
};

TEST_F(SerialPacketLogTest, WritesWholeLineThroughASmallFifo) {
  FakePort port;   // 64-byte FIFO, so a hex dump needs several drains
  uint8_t raw[40];
  for (size_t i = 0; i < sizeof(raw); i++) raw[i] = (uint8_t)i;

  SerialLogLine<> line;
  line.printf("12:00:00 - 1/1/2026 U RAW: snr_q=%d rssi=%d len=%d hex=", -22, -95, (int)sizeof(raw));
  line.hex(raw, sizeof(raw));
  EXPECT_TRUE(line.flush(port));

  EXPECT_EQ('\n', port.written().back());
  EXPECT_NE(std::string::npos, port.written().find("snr_q=-22 rssi=-95 len=40 hex=000102"));
  EXPECT_EQ(0u, serialLogDroppedCount());
}

TEST_F(SerialPacketLogTest, HexMatchesUppercaseWireFormat) {
  FakePort port(4096);
  const uint8_t raw[] = { 0x00, 0x0F, 0xAB, 0xFF };

  SerialLogLine<> line;
  line.hex(raw, sizeof(raw));
  ASSERT_TRUE(line.flush(port));

  EXPECT_EQ("000FABFF\r\n", port.written());
}

// The wedge case: a host with the port open that has stopped reading must cost a line, not
// the loop.
TEST_F(SerialPacketLogTest, DropsImmediatelyWhenHostStoppedDraining) {
  primePort();
  FakePort port;
  port.wedge();

  uint32_t before = millis();
  SerialLogLine<> line;
  line.printf("RAW: hex=");
  line.hex((const uint8_t*)"\x01\x02", 2);
  EXPECT_FALSE(line.flush(port));

  EXPECT_EQ(before, millis()) << "must not wait at all on a port with no room";
  EXPECT_TRUE(port.written().empty());
  EXPECT_EQ(1u, serialLogDroppedCount());
}

// Same host, but it stops reading part-way through a line: finishing it is worth a short
// wait, hanging on it is not.
TEST_F(SerialPacketLogTest, GivesUpWithinBudgetWhenHostWedgesMidLine) {
  primePort();
  FakePort port(64, 64);
  port.wedgeAfter(64);   // host reads one FIFO-full, then stops
  uint8_t raw[200];
  memset(raw, 0xA5, sizeof(raw));

  SerialLogLine<> line;
  line.printf("RAW: hex=");
  line.hex(raw, sizeof(raw));   // ~409 bytes: more than one FIFO-full

  uint32_t before = millis();
  EXPECT_FALSE(line.flush(port));

  uint32_t waited = millis() - before;
  EXPECT_GE(waited, (uint32_t)SERIAL_LOG_WRITE_BUDGET_MS);
  EXPECT_LE(waited, (uint32_t)SERIAL_LOG_WRITE_BUDGET_MS + 2) << "wait must be bounded";
  EXPECT_EQ((size_t)64, port.written().size());
  EXPECT_EQ(1u, serialLogDroppedCount());
}

TEST_F(SerialPacketLogTest, ReportsTheGapOnceTheHostRecovers) {
  primePort();
  FakePort wedged;
  wedged.wedge();
  for (int i = 0; i < 3; i++) {
    SerialLogLine<> line;
    line.printf("RAW: hex=00");
    EXPECT_FALSE(line.flush(wedged));
  }
  ASSERT_EQ(3u, serialLogDroppedCount());

  FakePort recovered(4096);
  SerialLogLine<> line;
  line.printf("RAW: hex=01");
  EXPECT_TRUE(line.flush(recovered));

  EXPECT_EQ("DROP:3\r\nRAW: hex=01\r\n", recovered.written());
  EXPECT_EQ(0u, serialLogDroppedCount());
}

// An unattached port reports no room exactly like a wedged one, so a host connecting to a
// node that has been logging to nobody for hours must not be handed that whole tally.
TEST_F(SerialPacketLogTest, DoesNotCountDropsBeforeAnyHostHasRead) {
  FakePort never_attached;
  never_attached.wedge();
  for (int i = 0; i < 5; i++) {
    SerialLogLine<> line;
    line.printf("RAW: hex=00");
    EXPECT_FALSE(line.flush(never_attached));
  }
  EXPECT_EQ(0u, serialLogDroppedCount());

  FakePort attached(4096);
  SerialLogLine<> line;
  line.printf("RAW: hex=01");
  EXPECT_TRUE(line.flush(attached));
  EXPECT_EQ("RAW: hex=01\r\n", attached.written()) << "no DROP: preamble for lines nobody wanted";
}

TEST_F(SerialPacketLogTest, OversizedLineIsTruncatedAndCounted) {
  primePort();
  FakePort port(4096);
  uint8_t raw[64];
  memset(raw, 0x5A, sizeof(raw));

  SerialLogLine<32> line;   // deliberately too small for the hex that follows
  line.printf("RAW: hex=");
  line.hex(raw, sizeof(raw));
  EXPECT_FALSE(line.flush(port));

  EXPECT_LE(port.written().size(), (size_t)32);
  EXPECT_EQ('\n', port.written().back());
  EXPECT_EQ(1u, serialLogDroppedCount());
}

// printf() overruns take the same path as hex() overruns.
TEST_F(SerialPacketLogTest, OversizedPrintfIsTruncatedAndCounted) {
  primePort();
  FakePort port(4096);

  SerialLogLine<16> line;
  line.printf("%s", "0123456789abcdefghij");
  EXPECT_FALSE(line.flush(port));

  // No stray NUL from vsnprintf's terminator, and still newline-terminated.
  EXPECT_EQ("0123456789abc\r\n", port.written());
  EXPECT_EQ(1u, serialLogDroppedCount());
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
