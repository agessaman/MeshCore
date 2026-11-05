#include "helpers/Radio2Profile.h"
#include "helpers/Radio2Scanner.h"

#include <gtest/gtest.h>

namespace {

Radio2Params params(float freq, float bw, uint8_t sf, uint8_t cr) {
  Radio2Params p;
  p.freq = freq; p.bw = bw; p.sf = sf; p.cr = cr;
  return p;
}

// A typical main channel and MediaMesh.
const Radio2Params kMain = params(910.525f, 62.5f, 7, 5);
const Radio2Params kMediaMesh = params(911.150f, 250.0f, 7, 5);

// Drives the scanner the way MyMesh::loop() does: ask, retune, report.
struct Rig {
  Radio2Scanner s;
  uint32_t now = 1000;
  Radio2Scanner::RetuneResult next = Radio2Scanner::RETUNE_OK;
  uint32_t switch_us = 400;
  int tick(bool can_leave = true) {
    int want = s.step(now, can_leave);
    if (want >= 0) s.onRetune((uint8_t)want, next, now + switch_us, switch_us);
    return want;
  }
};

} // namespace

// ---- parsing and formatting --------------------------------------------------

TEST(Radio2Parse, Presets) {
  Radio2Params p;
  ASSERT_EQ(radio2::parse("mediamesh", p), radio2::PARSE_OK);
  EXPECT_EQ(p, kMediaMesh);
  ASSERT_EQ(radio2::parse("mediamesh-range", p), radio2::PARSE_OK);
  EXPECT_EQ(p, params(911.150f, 125.0f, 7, 5));
}

TEST(Radio2Parse, Tuple) {
  Radio2Params p;
  ASSERT_EQ(radio2::parse("911.150,250,7,5", p), radio2::PARSE_OK);
  EXPECT_EQ(p, kMediaMesh);
  ASSERT_EQ(radio2::parse("869.618,62.5,8,8,rx", p), radio2::PARSE_OK);
  EXPECT_EQ(p, params(869.618f, 62.5f, 8, 8));
}

TEST(Radio2Parse, KeymindStyleTrailingPreambleIsAccepted) {
  Radio2Params p;
  EXPECT_EQ(radio2::parse("911.150,250,7,5,rx,auto", p), radio2::PARSE_OK);
  EXPECT_EQ(radio2::parse("911.150,250,7,5,rx,48", p), radio2::PARSE_OK);
  EXPECT_EQ(radio2::parse("911.150,250,7,5,rx,4", p), radio2::PARSE_INVALID);
}

TEST(Radio2Parse, OffAndTransmitModes) {
  Radio2Params p = kMain;
  EXPECT_EQ(radio2::parse("off", p), radio2::PARSE_OFF);
  EXPECT_EQ(radio2::parse("911.150,250,7,5,rxtx", p), radio2::PARSE_TX_UNSUPPORTED);
  EXPECT_EQ(p, kMain);   // untouched on anything but OK
}

TEST(Radio2Parse, RejectsBadInput) {
  Radio2Params p;
  const char* bad[] = {
    "", "911.150", "911.150,250,7", "911.150,251,7,5", "911.150,250,13,5", "911.150,250,7,9",
    "911.150,250,7,4", "100,250,7,5", "abc,250,7,5", "911.150,250,7,5,tx", "911.150,250,7,5,rx,auto,x",
    "911.150,250,7x,5", "911.150,250,-1,5",
    "911.150,250,7,5,rx,auto,aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  };
  for (const char* t : bad) EXPECT_EQ(radio2::parse(t, p), radio2::PARSE_INVALID) << t;
}

TEST(Radio2Format, ReplyMatchesKeymindShape) {
  char buf[64];
  radio2::formatReply(buf, sizeof(buf), true, kMediaMesh);
  EXPECT_STREQ(buf, "> 911.150,250.000,7,5,rx,32");
  radio2::formatReply(buf, sizeof(buf), false, kMediaMesh);
  EXPECT_STREQ(buf, "> off");
}

TEST(Radio2Format, StatusRadioMatchesStatusRadioField) {
  char buf[64];
  radio2::formatStatusRadio(buf, sizeof(buf), kMediaMesh);
  EXPECT_STREQ(buf, "911.150000,250.0,7,5");
}

TEST(Radio2Format, IntegerPersistenceRoundTrips) {
  EXPECT_EQ(radio2::freqToKhz(911.150f), 911150u);
  EXPECT_EQ(radio2::bwToHz(62.5f), 62500u);
  EXPECT_EQ(radio2::bwToHz(7.8f), 7800u);
  EXPECT_FLOAT_EQ(radio2::khzToFreq(911150), 911.150f);
  EXPECT_FLOAT_EQ(radio2::hzToBw(41700), 41.7f);
}

// ---- schedule --------------------------------------------------------------

TEST(Radio2Schedule, SlowMainGetsMinimumVisitMediaMeshGetsTheRest) {
  radio2::Schedule s = radio2::schedule(kMain, kMediaMesh, 1000);
  EXPECT_EQ(s.slow, 0);
  EXPECT_TRUE(s.fits);
  // SF7/62.5 kHz: 2048 us symbols. 4.6 symbols = 9421 us.
  EXPECT_EQ(s.listen_us[0], 9421u);
  // Half a 32-symbol preamble (32768 us) - main visit - two switches.
  EXPECT_EQ(s.listen_us[1], 32768u - 9421u - 2000u);
  // The radio is away from the main channel for less than half its preamble,
  // so a main packet always finds it back in time.
  EXPECT_LT(s.listen_us[1] + 2000u, 32768u);
}

TEST(Radio2Schedule, SlowerRadio2SwapsRoles) {
  radio2::Schedule s = radio2::schedule(kMediaMesh, kMain, 1000);
  EXPECT_EQ(s.slow, 1);
  EXPECT_EQ(s.listen_us[1], 9421u);
  EXPECT_EQ(s.listen_us[0], 32768u - 9421u - 2000u);
}

TEST(Radio2Schedule, ExpensiveSwitchFallsBackToMinimumVisit) {
  // Equal profiles with a 20 ms switch leave nothing for the fast visit.
  radio2::Schedule s = radio2::schedule(kMediaMesh, kMediaMesh, 20000);
  EXPECT_FALSE(s.fits);
  EXPECT_EQ(s.listen_us[1], radio2::minimumListenUs(kMediaMesh));
}

// ---- scanner ---------------------------------------------------------------

TEST(Radio2Scanner, OffNeverMoves) {
  Rig r;
  r.s.configure(false, kMain, kMediaMesh);
  for (int i = 0; i < 100; i++, r.now += 1000) EXPECT_EQ(r.tick(), -1);
  EXPECT_EQ(r.s.current(), 0);
}

TEST(Radio2Scanner, AlternatesOnSchedule) {
  Rig r;
  r.s.configure(true, kMain, kMediaMesh);
  EXPECT_EQ(r.tick(), -1);                       // just started on main
  r.now += r.s.schedule().listen_us[0];
  EXPECT_EQ(r.tick(), 1);
  EXPECT_EQ(r.s.current(), 1);
  r.now += r.switch_us + 1000;
  EXPECT_EQ(r.tick(), -1);                       // still inside the radio2 visit
  r.now += r.s.schedule().listen_us[1];
  EXPECT_EQ(r.tick(), 0);
  EXPECT_EQ(r.s.current(), 0);
  EXPECT_EQ(r.s.switches(), 2u);
}

TEST(Radio2Scanner, StaysOnMainWhileASendIsDue) {
  Rig r;
  r.s.configure(true, kMain, kMediaMesh);
  r.now += 100000;
  EXPECT_EQ(r.tick(false), -1);
  EXPECT_EQ(r.s.current(), 0);
  EXPECT_EQ(r.tick(true), 1);
}

TEST(Radio2Scanner, BusyKeepsProfileAndRetries) {
  Rig r;
  r.s.configure(true, kMain, kMediaMesh);
  r.now += 100000;
  r.next = Radio2Scanner::RETUNE_BUSY;
  EXPECT_EQ(r.tick(), 1);
  EXPECT_EQ(r.s.current(), 0);
  EXPECT_EQ(r.s.busy(), 1u);
  r.next = Radio2Scanner::RETUNE_OK;
  EXPECT_EQ(r.tick(), 1);
  EXPECT_EQ(r.s.current(), 1);
}

TEST(Radio2Scanner, TurningOffReturnsToMain) {
  Rig r;
  r.s.configure(true, kMain, kMediaMesh);
  r.now += 100000;
  ASSERT_EQ(r.tick(), 1);
  r.s.configure(false, kMain, kMediaMesh);
  EXPECT_EQ(r.tick(), 0);       // immediately, without waiting out the visit
  EXPECT_EQ(r.s.current(), 0);
  EXPECT_EQ(r.tick(), -1);
}

TEST(Radio2Scanner, RepeatedFailuresFaultAndRestoreMain) {
  Rig r;
  r.s.configure(true, kMain, kMediaMesh);
  r.now += 100000;
  r.next = Radio2Scanner::RETUNE_FAILED;
  for (int i = 0; i < Radio2Scanner::MaxConsecutiveFailures; i++) r.tick();
  EXPECT_TRUE(r.s.faulted());
  EXPECT_FALSE(r.s.enabled());
  // Keeps asking for main until a retune there succeeds, then stays.
  EXPECT_EQ(r.tick(), 0);
  r.next = Radio2Scanner::RETUNE_OK;
  EXPECT_EQ(r.tick(), 0);
  EXPECT_EQ(r.s.current(), 0);
  r.now += 1000000;
  EXPECT_EQ(r.tick(), -1);
  // A new configuration clears the fault.
  r.s.configure(true, kMain, params(911.150f, 125.0f, 7, 5));
  EXPECT_TRUE(r.s.enabled());
}

TEST(Radio2Scanner, OneFailureRestoresMainThenCarriesOn) {
  Rig r;
  r.s.configure(true, kMain, kMediaMesh);
  r.now += 100000;
  r.next = Radio2Scanner::RETUNE_FAILED;
  EXPECT_EQ(r.tick(), 1);
  EXPECT_FALSE(r.s.faulted());
  r.next = Radio2Scanner::RETUNE_OK;
  EXPECT_EQ(r.tick(), 0);                        // back to main first
  EXPECT_EQ(r.s.current(), 0);
  r.now += 100000;
  EXPECT_EQ(r.tick(), 1);                        // then scanning resumes
}

TEST(Radio2Scanner, MeasuredSwitchShortensRadio2Visit) {
  Rig r;
  r.s.configure(true, kMain, kMediaMesh);
  const uint32_t before = r.s.schedule().listen_us[1];
  r.switch_us = 3000;
  r.now += 100000;
  r.tick();
  EXPECT_EQ(r.s.switchMaxUs(), 3000u);
  EXPECT_EQ(r.s.switchBudgetUs(), 3300u);
  EXPECT_LT(r.s.schedule().listen_us[1], before);
}

TEST(Radio2Scanner, ExternalRetuneResetsToMain) {
  Rig r;
  r.s.configure(true, kMain, kMediaMesh);
  r.now += 100000;
  ASSERT_EQ(r.tick(), 1);
  r.s.notePrimary(r.now);
  EXPECT_EQ(r.s.current(), 0);
  EXPECT_EQ(r.tick(), -1);
}

TEST(Radio2Scanner, SurvivesMicrosWrap) {
  Rig r;
  r.now = 0xFFFFFF00u;
  r.s.configure(true, kMain, kMediaMesh);
  r.s.notePrimary(r.now);
  r.now += r.s.schedule().listen_us[0];          // wraps past zero
  EXPECT_EQ(r.tick(), 1);
}
