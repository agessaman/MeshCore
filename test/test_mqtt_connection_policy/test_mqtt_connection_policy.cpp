#include <gtest/gtest.h>
#include <stdint.h>
#include <limits>

#include "helpers/MQTTConnectionPolicy.h"

namespace Policy = MQTTConnectionPolicy;

TEST(MQTTConnectionPolicy, ElapsedTimeHandlesNormalAndWrappedClocks) {
  EXPECT_EQ(4000U, Policy::elapsedMs(5000U, 1000U));

  const uint32_t before_wrap = std::numeric_limits<uint32_t>::max() - 99U;
  EXPECT_EQ(150U, Policy::elapsedMs(50U, before_wrap));
}

TEST(MQTTConnectionPolicy, DeferredSetupStartsImmediatelyWhenNothingWasAttempted) {
  // reconnectGuardActive(now, 0) is true for the first 15 s of uptime. An unset
  // cross-slot stamp must not borrow that and hold the first slot.
  EXPECT_TRUE(Policy::reconnectGuardActive(1000U, 0U));
  EXPECT_TRUE(Policy::deferredSlotSetupDue(1000U, 0U, 60000U, 0U, false));
  EXPECT_TRUE(Policy::deferredSlotSetupDue(0U, 0U, 60000U, 0U, false));
}

TEST(MQTTConnectionPolicy, DeferredSetupSpacesLaterSlotsWithTheReconnectGuard) {
  const uint32_t armed = 5000U;
  EXPECT_FALSE(Policy::deferredSlotSetupDue(armed + 14999U, 0U, 60000U, armed, false));
  EXPECT_TRUE(Policy::deferredSlotSetupDue(armed + 15000U, 0U, 60000U, armed, false));
}

TEST(MQTTConnectionPolicy, DeferredSetupRetryWaitsOutItsOwnInterval) {
  // An unset stamp is not a boot-time deadline, even just before the interval.
  EXPECT_TRUE(Policy::deferredSlotSetupDue(59999U, 0U, 60000U, 0U, false));
  EXPECT_FALSE(Policy::deferredSlotSetupDue(64999U, 5000U, 60000U, 0U, false));
  EXPECT_TRUE(Policy::deferredSlotSetupDue(65000U, 5000U, 60000U, 0U, false));
}

TEST(MQTTConnectionPolicy, DeferredSetupRetryIsWrapSafeAndOnePerPass) {
  const uint32_t last = std::numeric_limits<uint32_t>::max() - 999U;
  EXPECT_FALSE(Policy::deferredSlotSetupDue(58999U, last, 60000U, 0U, false));
  EXPECT_TRUE(Policy::deferredSlotSetupDue(59000U, last, 60000U, 0U, false));
  // The one-setup budget blocks even a never-attempted slot.
  EXPECT_FALSE(Policy::deferredSlotSetupDue(1000U, 0U, 60000U, 0U, true));
  // A clear retry still loses to a guard armed earlier in the pass.
  EXPECT_FALSE(Policy::deferredSlotSetupDue(65000U, 5000U, 60000U, 65000U, false));
}

TEST(MQTTConnectionPolicy, FirstSetupHandshakeDoneTracksTheArmersAttempt) {
  // Still negotiating, or connected with its on-connect work not yet run.
  EXPECT_FALSE(Policy::firstSetupHandshakeDone(true, true, true, false));
  EXPECT_FALSE(Policy::firstSetupHandshakeDone(true, true, false, true));
  // Connected (or failed) and its completion work has run.
  EXPECT_TRUE(Policy::firstSetupHandshakeDone(true, true, false, false));
  // Disabled or destroyed: nothing left to finish.
  EXPECT_TRUE(Policy::firstSetupHandshakeDone(false, true, true, true));
  EXPECT_TRUE(Policy::firstSetupHandshakeDone(true, false, true, true));
}

TEST(MQTTConnectionPolicy, FirstSetupReleasesGuardOnceHandshakeIsDone) {
  const uint32_t armed = 5000U;
  const bool released = Policy::firstSetupGuardReleased(armed + 4000U, armed, true, true, true);
  EXPECT_TRUE(released);
  EXPECT_TRUE(Policy::deferredSlotSetupDue(armed + 4000U, 0U, 60000U, armed, false, released));
  // The one-setup budget still applies to a released guard.
  EXPECT_FALSE(Policy::deferredSlotSetupDue(armed + 4000U, 0U, 60000U, armed, true, released));
}

TEST(MQTTConnectionPolicy, FirstSetupWaitsWhileHandshakeIsRunning) {
  const uint32_t armed = 5000U;
  const bool released = Policy::firstSetupGuardReleased(armed + 14999U, armed, true, true, false);
  EXPECT_FALSE(released);
  EXPECT_FALSE(Policy::deferredSlotSetupDue(armed + 14999U, 0U, 60000U, armed, false, released));
  // 15 s is still the upper bound.
  EXPECT_TRUE(Policy::deferredSlotSetupDue(armed + 15000U, 0U, 60000U, armed, false, false));
}

TEST(MQTTConnectionPolicy, FirstSetupKeepsMinimumGapAfterInstantConnect) {
  const uint32_t armed = 5000U;
  EXPECT_FALSE(Policy::firstSetupGuardReleased(armed, armed, true, true, true));
  EXPECT_FALSE(Policy::firstSetupGuardReleased(armed + Policy::kFirstSetupMinGapMs - 1U,
                                               armed, true, true, true));
  EXPECT_TRUE(Policy::firstSetupGuardReleased(armed + Policy::kFirstSetupMinGapMs,
                                              armed, true, true, true));
}

TEST(MQTTConnectionPolicy, FirstSetupReleaseWhenArmerIsDisabledOrDestroyed) {
  const uint32_t armed = 5000U;
  const bool disabled_done = Policy::firstSetupHandshakeDone(false, true, true, false);
  const bool destroyed_done = Policy::firstSetupHandshakeDone(true, false, true, false);
  EXPECT_TRUE(Policy::firstSetupGuardReleased(armed + 3000U, armed, true, true, disabled_done));
  EXPECT_TRUE(Policy::firstSetupGuardReleased(armed + 3000U, armed, true, true, destroyed_done));
}

TEST(MQTTConnectionPolicy, FirstSetupReleaseIsWrapSafe) {
  const uint32_t armed = std::numeric_limits<uint32_t>::max() - 999U;
  // 1999 ms after the stamp, across the wrap.
  EXPECT_FALSE(Policy::firstSetupGuardReleased(999U, armed, true, true, true));
  EXPECT_TRUE(Policy::firstSetupGuardReleased(1000U, armed, true, true, true));
  EXPECT_TRUE(Policy::deferredSlotSetupDue(1000U, 0U, 60000U, armed, false, true));
  EXPECT_FALSE(Policy::deferredSlotSetupDue(1000U, 0U, 60000U, armed, false, false));
}

TEST(MQTTConnectionPolicy, NonFirstAttemptsKeepTheFullGuard) {
  const uint32_t armed = 5000U;
  // A setup retry is not a first attempt.
  EXPECT_FALSE(Policy::firstSetupGuardReleased(armed + 10000U, armed, false, true, true));
  // A guard armed by a reconnect, renewal, probe or reconfigure.
  EXPECT_FALSE(Policy::firstSetupGuardReleased(armed + 10000U, armed, true, false, true));
  EXPECT_FALSE(Policy::deferredSlotSetupDue(armed + 14999U, 0U, 60000U, armed, false, false));
  EXPECT_FALSE(Policy::deferredSlotSetupDue(armed + 14999U, 1000U, 1000U, armed, false, false));
}

TEST(MQTTConnectionPolicy, ReconnectFollowsAFinishedHandshakeAfterTheMinimumGap) {
  const uint32_t armed = 5000U;
  EXPECT_FALSE(Policy::reconnectGuardReleased(armed, armed, true, true));
  EXPECT_FALSE(Policy::reconnectGuardReleased(armed + Policy::kFirstSetupMinGapMs - 1U, armed, true, true));
  EXPECT_TRUE(Policy::reconnectGuardReleased(armed + Policy::kFirstSetupMinGapMs, armed, true, true));
  // Five slots after a link outage: about 3 s per handshake instead of 15 s per slot.
  EXPECT_TRUE(Policy::reconnectGuardReleased(armed + 3000U, armed, true, true));
}

TEST(MQTTConnectionPolicy, ReconnectKeepsTheFullGuardWhileTheArmerIsStillConnecting) {
  const uint32_t armed = 5000U;
  const bool connecting = Policy::firstSetupHandshakeDone(true, true, true, false);
  const bool publishing = Policy::firstSetupHandshakeDone(true, true, false, true);
  EXPECT_FALSE(Policy::reconnectGuardReleased(armed + 14999U, armed, true, connecting));
  EXPECT_FALSE(Policy::reconnectGuardReleased(armed + 14999U, armed, true, publishing));
  // The guard itself still expires at 15 s for a handshake that hangs.
  EXPECT_FALSE(Policy::reconnectGuardActive(armed + Policy::kReconnectGuardMs, armed));
}

TEST(MQTTConnectionPolicy, ReconnectKeepsTheFullGuardAfterARenewalProbeOrReconfigure) {
  const uint32_t armed = 5000U;
  EXPECT_FALSE(Policy::reconnectGuardReleased(armed + 14999U, armed, false, true));
}

TEST(MQTTConnectionPolicy, ReconnectArmsTheEarlyReleaseOnlyWhenNoOlderEventIsOutstanding) {
  EXPECT_TRUE(Policy::reconnectArmsEarlyRelease(false, false));
  // An older attempt's outcome would clear the pending flag for this one.
  EXPECT_FALSE(Policy::reconnectArmsEarlyRelease(true, false));
  // So would the late DISCONNECTED of a session whose soft disconnect timed out.
  EXPECT_FALSE(Policy::reconnectArmsEarlyRelease(false, true));
  EXPECT_FALSE(Policy::reconnectArmsEarlyRelease(true, true));
}

TEST(MQTTConnectionPolicy, ReconnectGuardReleaseSurvivesClockRollover) {
  const uint32_t armed = 0xFFFFFF00U;
  EXPECT_FALSE(Policy::reconnectGuardReleased(armed + 1999U, armed, true, true));
  EXPECT_TRUE(Policy::reconnectGuardReleased(armed + 2000U, armed, true, true));
}

TEST(MQTTConnectionPolicy, ConnectingReconfigureWaitsForTheReconnectGuard) {
  EXPECT_FALSE(Policy::slotReconfigureDue(false, true, 20000U, 0U, false));
  EXPECT_FALSE(Policy::slotReconfigureDue(true, true, 20000U, 0U, true));
  // Guard epoch: last attempt 0 blocks a connect for the first 15 s of uptime.
  EXPECT_TRUE(Policy::reconnectGuardActive(14999U, 0U));
  EXPECT_FALSE(Policy::slotReconfigureDue(true, true, 14999U, 0U, false));
  EXPECT_TRUE(Policy::slotReconfigureDue(true, true, 15000U, 0U, false));

  const uint32_t armed = 80000U;
  EXPECT_FALSE(Policy::slotReconfigureDue(true, true, armed + 14999U, armed, false));
  EXPECT_TRUE(Policy::slotReconfigureDue(true, true, armed + 15000U, armed, false));
}

TEST(MQTTConnectionPolicy, DisableReconfigureIsNotHeldByTheReconnectGuard) {
  const uint32_t armed = 80000U;
  EXPECT_TRUE(Policy::slotReconfigureDue(true, false, armed + 1U, armed, false));
  EXPECT_TRUE(Policy::slotReconfigureDue(true, false, 1U, 0U, false));
  // Still one per pass, and a clear flag is not invented.
  EXPECT_FALSE(Policy::slotReconfigureDue(true, false, armed + 1U, armed, true));
  EXPECT_FALSE(Policy::slotReconfigureDue(false, false, armed + 15000U, armed, false));
}

TEST(MQTTConnectionPolicy, ReconfigureGuardBoundaryIsWrapSafe) {
  const uint32_t last = std::numeric_limits<uint32_t>::max() - 9999U;
  EXPECT_FALSE(Policy::slotReconfigureDue(true, true, 4999U, last, false));
  EXPECT_TRUE(Policy::slotReconfigureDue(true, true, 5000U, last, false));
}

TEST(MQTTConnectionPolicy, CrossSlotReconnectGuardHasExactBoundary) {
  EXPECT_TRUE(Policy::reconnectGuardActive(14999U, 0U));
  EXPECT_FALSE(Policy::reconnectGuardActive(15000U, 0U));

  const uint32_t last = std::numeric_limits<uint32_t>::max() - 9999U;
  EXPECT_TRUE(Policy::reconnectGuardActive(4999U, last));
  EXPECT_FALSE(Policy::reconnectGuardActive(5000U, last));
}

TEST(MQTTConnectionPolicy, StableResetRequiresARealStartAndFullWindow) {
  EXPECT_FALSE(Policy::stableConnection(500000U, 0U));
  EXPECT_FALSE(Policy::stableConnection(120999U, 1000U));
  EXPECT_TRUE(Policy::stableConnection(121000U, 1000U));
}

TEST(MQTTConnectionPolicy, StableResetWindowSurvivesMillisRollover) {
  const uint32_t connected_at = std::numeric_limits<uint32_t>::max() - 59999U;
  EXPECT_FALSE(Policy::stableConnection(59999U, connected_at));
  EXPECT_TRUE(Policy::stableConnection(60000U, connected_at));
}

TEST(MQTTConnectionPolicy, BackoffLadderSaturatesAtFiveMinutes) {
  EXPECT_EQ(10000U, Policy::reconnectBackoffMs(0));
  EXPECT_EQ(30000U, Policy::reconnectBackoffMs(1));
  EXPECT_EQ(60000U, Policy::reconnectBackoffMs(2));
  EXPECT_EQ(120000U, Policy::reconnectBackoffMs(3));
  EXPECT_EQ(300000U, Policy::reconnectBackoffMs(4));
  EXPECT_EQ(300000U, Policy::reconnectBackoffMs(5));
  EXPECT_EQ(300000U, Policy::reconnectBackoffMs(255));
}

TEST(MQTTConnectionPolicy, LaterSlotsReceiveThreeSecondStagger) {
  EXPECT_EQ(10000U, Policy::reconnectDelayMs(0, 0));
  EXPECT_EQ(13000U, Policy::reconnectDelayMs(0, 1));
  EXPECT_EQ(315000U, Policy::reconnectDelayMs(5, 5));
}

TEST(MQTTConnectionPolicy, ReconnectDueUsesDelayBoundaryAndWrapSafeElapsedTime) {
  EXPECT_FALSE(Policy::reconnectDue(12999U, 0U, 0, 1));
  EXPECT_TRUE(Policy::reconnectDue(13000U, 0U, 0, 1));

  const uint32_t last = std::numeric_limits<uint32_t>::max() - 4999U;
  EXPECT_FALSE(Policy::reconnectDue(4999U, last, 0, 0));
  EXPECT_TRUE(Policy::reconnectDue(5000U, last, 0, 0));
}

TEST(MQTTConnectionPolicy, BackoffAdvanceClimbsThenCountsFailuresAtMaximum) {
  Policy::BackoffAdvance first = Policy::advanceBackoff(0, 0);
  EXPECT_EQ(1, first.reconnect_backoff);
  EXPECT_EQ(0, first.max_backoff_failures);
  EXPECT_FALSE(first.circuit_breaker_tripped);
  EXPECT_TRUE(first.should_reconnect);

  Policy::BackoffAdvance enters_maximum = Policy::advanceBackoff(4, 0);
  EXPECT_EQ(5, enters_maximum.reconnect_backoff);
  EXPECT_EQ(0, enters_maximum.max_backoff_failures);
  EXPECT_FALSE(enters_maximum.circuit_breaker_tripped);
  EXPECT_TRUE(enters_maximum.should_reconnect);

  Policy::BackoffAdvance first_max_failure = Policy::advanceBackoff(5, 0);
  EXPECT_EQ(5, first_max_failure.reconnect_backoff);
  EXPECT_EQ(1, first_max_failure.max_backoff_failures);
  EXPECT_FALSE(first_max_failure.circuit_breaker_tripped);
  EXPECT_TRUE(first_max_failure.should_reconnect);
}

TEST(MQTTConnectionPolicy, ThirdFailureAtMaximumTripsWithoutAnotherHandshake) {
  Policy::BackoffAdvance result = Policy::advanceBackoff(5, 2);
  EXPECT_EQ(5, result.reconnect_backoff);
  EXPECT_EQ(3, result.max_backoff_failures);
  EXPECT_TRUE(result.circuit_breaker_tripped);
  EXPECT_FALSE(result.should_reconnect);
}

TEST(MQTTConnectionPolicy, CircuitBreakerProbeHasExactThirtyMinuteBoundary) {
  const uint32_t probe = Policy::kCircuitBreakerProbeMs;
  EXPECT_EQ(1800000U, probe);
  EXPECT_FALSE(Policy::circuitBreakerProbeDue(1799999U, 0U, probe));
  EXPECT_TRUE(Policy::circuitBreakerProbeDue(1800000U, 0U, probe));

  const uint32_t last = std::numeric_limits<uint32_t>::max() - 899999U;
  EXPECT_FALSE(Policy::circuitBreakerProbeDue(899999U, last, probe));
  EXPECT_TRUE(Policy::circuitBreakerProbeDue(900000U, last, probe));
}

TEST(MQTTConnectionPolicy, BreakerProbeMinutesAcceptOnlyTenToThirty) {
  EXPECT_FALSE(Policy::breakerProbeMinutesValid(9));
  EXPECT_TRUE(Policy::breakerProbeMinutesValid(10));
  EXPECT_TRUE(Policy::breakerProbeMinutesValid(30));
  EXPECT_FALSE(Policy::breakerProbeMinutesValid(31));
  EXPECT_FALSE(Policy::breakerProbeMinutesValid(-1));

  EXPECT_EQ(600000U, Policy::breakerProbeMs(10));
  EXPECT_EQ(1800000U, Policy::breakerProbeMs(30));
  // Corrupt or zeroed values never shorten the probe below the floor.
  EXPECT_EQ(Policy::kCircuitBreakerProbeMs, Policy::breakerProbeMs(0));
  EXPECT_EQ(Policy::kCircuitBreakerProbeMs, Policy::breakerProbeMs(5));
  EXPECT_EQ(Policy::kCircuitBreakerProbeMs, Policy::breakerProbeMs(255));
}

TEST(MQTTConnectionPolicy, BreakerProbeMinutesParseRejectsJunkAndOverflow) {
  uint8_t minutes = 0;
  EXPECT_TRUE(Policy::parseBreakerProbeMinutes("10", &minutes));
  EXPECT_EQ(10, minutes);
  EXPECT_TRUE(Policy::parseBreakerProbeMinutes("030", &minutes));
  EXPECT_EQ(30, minutes);
  for (const char* bad : {"", "9", "31", "0", "-10", "15m", " 15", "15 ", "1.5",
                          "0010", "4294967306", "266"}) {
    minutes = 77;
    EXPECT_FALSE(Policy::parseBreakerProbeMinutes(bad, &minutes)) << bad;
    EXPECT_EQ(77, minutes) << bad;
  }
  EXPECT_FALSE(Policy::parseBreakerProbeMinutes(nullptr, &minutes));
}

TEST(MQTTConnectionPolicy, ConfiguredBreakerIntervalIsWrapSafe) {
  const uint32_t probe = Policy::breakerProbeMs(10);
  EXPECT_FALSE(Policy::circuitBreakerProbeDue(599999U, 0U, probe));
  EXPECT_TRUE(Policy::circuitBreakerProbeDue(600000U, 0U, probe));

  const uint32_t last = std::numeric_limits<uint32_t>::max() - 99999U;
  EXPECT_FALSE(Policy::circuitBreakerProbeDue(499999U, last, probe));
  EXPECT_TRUE(Policy::circuitBreakerProbeDue(500000U, last, probe));
}

TEST(MQTTConnectionPolicy, LinkRecoveryMakesCurrentBackoffDueImmediately) {
  const uint32_t now = 1234U;
  const uint32_t probe = Policy::kCircuitBreakerProbeMs;
  const uint32_t last = Policy::immediateRetryLastAttempt(now, false, 4, 2, probe);
  EXPECT_TRUE(Policy::reconnectDue(now, last, 4, 2));
  EXPECT_FALSE(Policy::circuitBreakerProbeDue(now, last, probe));
}

TEST(MQTTConnectionPolicy, LinkRecoveryMakesBreakerProbeDueWithoutClearingIt) {
  const uint32_t now = 1234U;
  for (uint8_t minutes : {(uint8_t)10, (uint8_t)30}) {
    const uint32_t probe = Policy::breakerProbeMs(minutes);
    const uint32_t last = Policy::immediateRetryLastAttempt(now, true, 5, 2, probe);
    EXPECT_TRUE(Policy::circuitBreakerProbeDue(now, last, probe)) << (int)minutes;
  }
}

// One failure short of the breaker, an ordinary due check trips without a
// handshake. The first retry after link recovery must still connect.
TEST(MQTTConnectionPolicy, LinkRecoveryRetryAtThresholdAttemptsInsteadOfTripping) {
  Policy::BackoffAdvance recovery = Policy::advanceBackoffForLinkRecovery(5, 2);
  EXPECT_EQ(5, recovery.reconnect_backoff);
  EXPECT_EQ(3, recovery.max_backoff_failures);
  EXPECT_FALSE(recovery.circuit_breaker_tripped);
  EXPECT_TRUE(recovery.should_reconnect);

  // The failure still counts: the next ordinary due attempt trips.
  Policy::BackoffAdvance next = Policy::advanceBackoff(recovery.reconnect_backoff,
                                                       recovery.max_backoff_failures);
  EXPECT_TRUE(next.circuit_breaker_tripped);
  EXPECT_FALSE(next.should_reconnect);
}

TEST(MQTTConnectionPolicy, LinkRecoveryRetryBelowThresholdMatchesOrdinaryAdvance) {
  for (uint8_t backoff = 0; backoff <= 5; ++backoff) {
    for (uint8_t failures = 0; failures < 2; ++failures) {
      Policy::BackoffAdvance ordinary = Policy::advanceBackoff(backoff, failures);
      Policy::BackoffAdvance recovery = Policy::advanceBackoffForLinkRecovery(backoff, failures);
      EXPECT_EQ(ordinary.reconnect_backoff, recovery.reconnect_backoff);
      EXPECT_EQ(ordinary.max_backoff_failures, recovery.max_backoff_failures);
      EXPECT_EQ(ordinary.circuit_breaker_tripped, recovery.circuit_breaker_tripped);
      EXPECT_EQ(ordinary.should_reconnect, recovery.should_reconnect);
    }
  }
}

TEST(MQTTConnectionPolicy, JwtLifetimeUsesCappedPerSlotStagger) {
  EXPECT_EQ(86400U, Policy::jwtLifetimeSecs(86400U, 0));
  EXPECT_EQ(86100U, Policy::jwtLifetimeSecs(86400U, 1));
  EXPECT_EQ(84900U, Policy::jwtLifetimeSecs(86400U, 5));
}

TEST(MQTTConnectionPolicy, ShortJwtLifetimeUsesFivePercentPerSlot) {
  EXPECT_EQ(3300U, Policy::jwtLifetimeSecs(3300U, 0));
  EXPECT_EQ(3135U, Policy::jwtLifetimeSecs(3300U, 1));
  EXPECT_EQ(2970U, Policy::jwtLifetimeSecs(3300U, 2));
  EXPECT_EQ(85U, Policy::jwtLifetimeSecs(100U, 3));
}

TEST(MQTTConnectionPolicy, JwtLifetimeCannotUnderflowForUnexpectedSlotInput) {
  EXPECT_EQ(0U, Policy::jwtLifetimeSecs(100U, 255));
}

TEST(MQTTConnectionPolicy, RenewalBufferHasOneMinuteFloorAndFiveMinuteCap) {
  EXPECT_EQ(60U, Policy::renewalBufferSecs(0U));
  EXPECT_EQ(60U, Policy::renewalBufferSecs(599U));
  EXPECT_EQ(60U, Policy::renewalBufferSecs(600U));
  EXPECT_EQ(299U, Policy::renewalBufferSecs(2999U));
  EXPECT_EQ(300U, Policy::renewalBufferSecs(3000U));
  EXPECT_EQ(300U, Policy::renewalBufferSecs(86400U));
}

TEST(MQTTConnectionPolicy, UnsynchronizedClockOnlyCreatesAMissingToken) {
  EXPECT_TRUE(Policy::tokenNeedsRenewal(false, 0U, 0U, 300U));
  EXPECT_FALSE(Policy::tokenNeedsRenewal(false, 0U, 1735693200U, 300U));
}

TEST(MQTTConnectionPolicy, SyncedClockRenewsInvalidExpiredOrImminentTokens) {
  const uint32_t expires = 1735693200U;
  EXPECT_TRUE(Policy::tokenNeedsRenewal(true, 1735689000U, 0U, 300U));
  EXPECT_TRUE(Policy::tokenNeedsRenewal(true, 1735689000U, 999999999U, 300U));
  EXPECT_FALSE(Policy::tokenNeedsRenewal(true, expires - 301U, expires, 300U));
  EXPECT_TRUE(Policy::tokenNeedsRenewal(true, expires - 300U, expires, 300U));
  EXPECT_TRUE(Policy::tokenNeedsRenewal(true, expires, expires, 300U));
  EXPECT_TRUE(Policy::tokenNeedsRenewal(true, expires + 1U, expires, 300U));
}

TEST(MQTTConnectionPolicy, JwtReconnectReusesOnlyProvenValidCredentials) {
  const uint32_t now = 1735689600U;
  const uint32_t usable_expiry = now + Policy::kJwtReconnectSafetyMarginSecs + 1U;

  // Buffer 0 gates on the flat margin alone, which is what these cases cover.
  EXPECT_TRUE(Policy::canReuseJwtForReconnect(true, true, false, now, usable_expiry, 0U));
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(
      true, true, false, now, Policy::kMinimumValidEpoch - 1U, 0U));
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(true, true, false, now, 0U, 0U));
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(
      true, true, false, now, now + Policy::kJwtReconnectSafetyMarginSecs, 0U));
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(true, true, false, now, now - 1U, 0U));
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(true, false, false, now, usable_expiry, 0U));
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(false, true, false, now, usable_expiry, 0U));
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(true, true, true, now, usable_expiry, 0U));
}

TEST(MQTTConnectionPolicy, JwtReuseRefusesATokenTheRenewalPathIsAboutToBounce) {
  const uint32_t now = 1735689600U;
  const uint32_t buffer = Policy::renewalBufferSecs(3300U);  // waev's 55-minute tokens
  ASSERT_EQ(buffer, 300U);
  const uint32_t floor_secs = buffer + Policy::kJwtReconnectSafetyMarginSecs;

  // The observed defect (d3, 2026-08-17T14:31): reused at 302 s remaining under the
  // flat 60 s margin, then renewed five seconds later, which bounced the slot.
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(true, true, false, now, now + 302U, buffer));

  // Anything the renewal path would act on is refused, and so is the slack above it.
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(true, true, false, now, now + buffer, buffer));
  EXPECT_FALSE(
      Policy::canReuseJwtForReconnect(true, true, false, now, now + floor_secs, buffer));
  const uint32_t tightest_reuse = now + floor_secs + 1U;
  EXPECT_TRUE(Policy::canReuseJwtForReconnect(true, true, false, now, tightest_reuse, buffer));

  // What the margin above the buffer buys: the tightest token reuse will accept is still
  // not due for renewal a full margin later, so no renewal can bounce in behind it.
  EXPECT_FALSE(Policy::tokenNeedsRenewal(
      true, now + Policy::kJwtReconnectSafetyMarginSecs, tightest_reuse, buffer));

  // A buffer that would wrap the floor has to fail closed, not reuse a doomed token.
  EXPECT_FALSE(Policy::canReuseJwtForReconnect(
      true, true, false, now, now + 100000U, std::numeric_limits<uint32_t>::max()));
}

TEST(MQTTConnectionPolicy, RenewalThrottleHasExactBoundaryAndHandlesRollover) {
  EXPECT_FALSE(Policy::renewalAttemptAllowed(59999U, 0U));
  EXPECT_TRUE(Policy::renewalAttemptAllowed(60000U, 0U));

  const uint32_t last = std::numeric_limits<uint32_t>::max() - 29999U;
  EXPECT_FALSE(Policy::renewalAttemptAllowed(29999U, last));
  EXPECT_TRUE(Policy::renewalAttemptAllowed(30000U, last));
}

TEST(MQTTConnectionPolicy, JwtClockNeedsNtpOrAReasonableWallClock) {
  EXPECT_FALSE(Policy::jwtClockAvailable(false, Policy::kJwtClockThreshold - 1U));
  EXPECT_TRUE(Policy::jwtClockAvailable(false, Policy::kJwtClockThreshold));
  EXPECT_TRUE(Policy::jwtClockAvailable(true, 0U));
}

TEST(MQTTConnectionPolicy, WifiBackoffLadderStartsAtFifteenSecondsAndSaturates) {
  EXPECT_EQ(15000U, Policy::wifiReconnectBackoffMs(0));
  EXPECT_EQ(30000U, Policy::wifiReconnectBackoffMs(1));
  EXPECT_EQ(60000U, Policy::wifiReconnectBackoffMs(2));
  EXPECT_EQ(120000U, Policy::wifiReconnectBackoffMs(3));
  EXPECT_EQ(300000U, Policy::wifiReconnectBackoffMs(4));
  // Clamps at the 300 s rung for the saturated attempt count and beyond.
  EXPECT_EQ(300000U, Policy::wifiReconnectBackoffMs(5));
  EXPECT_EQ(300000U, Policy::wifiReconnectBackoffMs(200));
}

TEST(MQTTConnectionPolicy, WifiBackoffAttemptClimbsThenSaturatesAtFive) {
  uint8_t attempt = 0;
  for (uint8_t expected = 1; expected <= 5; ++expected) {
    attempt = Policy::nextWifiBackoffAttempt(attempt);
    EXPECT_EQ(expected, attempt);
  }
  // Saturated: never advances past 5 (index stays clamped at the 300 s rung).
  EXPECT_EQ(5U, Policy::nextWifiBackoffAttempt(attempt));
  EXPECT_EQ(5U, Policy::nextWifiBackoffAttempt(5));
}

TEST(MQTTConnectionPolicy, WifiReconnectRequiresBothDownAndSinceAttemptToClearRung) {
  const uint32_t down_since = 1000U;
  const uint32_t last_attempt = 1000U;
  const uint8_t attempt = 0;  // 15 s rung
  // Neither interval has elapsed yet.
  EXPECT_FALSE(Policy::wifiReconnectDue(1000U + 14999U, down_since, last_attempt, attempt));
  // Down long enough, but an attempt was made only 5 s ago (since-attempt short).
  EXPECT_FALSE(Policy::wifiReconnectDue(1000U + 15000U, down_since, 1000U + 10000U, attempt));
  // Both cleared at the exact boundary: due.
  EXPECT_TRUE(Policy::wifiReconnectDue(1000U + 15000U, down_since, last_attempt, attempt));
}

TEST(MQTTConnectionPolicy, WifiOutageStartStickyAcrossReconnectAttempts) {
  // First observe-down (or connected→down) records `now`. Further down samples
  // — including STA backoff WiFi.disconnect() — must keep that start so
  // AlertReporter quotes the outage, not the last attempt / boot event.
  EXPECT_EQ(0U, Policy::wifiCurrentOutageStartMs(5000, true, false, 0, false));
  EXPECT_EQ(5000U, Policy::wifiCurrentOutageStartMs(5000, false, false, 0, false));

  const uint32_t start = 9000U;
  EXPECT_EQ(start, Policy::wifiCurrentOutageStartMs(start, false, true, 0, true));
  EXPECT_EQ(start, Policy::wifiCurrentOutageStartMs(start + 15000U, false, false, start, true));
  EXPECT_EQ(start, Policy::wifiCurrentOutageStartMs(start + 2U * 3600U * 1000U,
                                                    false, false, start, true));
  EXPECT_EQ(0U, Policy::wifiCurrentOutageStartMs(start + 1000U, true, false, start, true));
}

TEST(MQTTConnectionPolicy, WifiReconnectDueSurvivesMillisRollover) {
  const uint32_t down_since = std::numeric_limits<uint32_t>::max() - 100U;
  const uint32_t last_attempt = down_since;
  const uint8_t attempt = 0;  // 15 s rung
  const uint32_t now = down_since + 15000U;  // wraps past zero
  EXPECT_TRUE(Policy::wifiReconnectDue(now, down_since, last_attempt, attempt));
  EXPECT_FALSE(Policy::wifiReconnectDue(down_since + 14999U, down_since, last_attempt, attempt));
}

// --- classifySlotActivation: the "will this slot connect on this hardware" rule
// the CLI uses to warn at `set mqttN.preset` time. Non-PSRAM Heltec V3 is the
// motivating case: RUNTIME_MQTT_SLOTS=3 but only 2 concurrent connections. ---

using Policy::SlotActivation;

TEST(SlotActivation, NonPsramV3FirstTwoConnectThirdIsOverCap) {
  // slot_count = 3 (runtime array), max_active = 2 (concurrent cap).
  const bool enabled[6] = {true, true, true, false, false, false};
  EXPECT_EQ(SlotActivation::Connects,      Policy::classifySlotActivation(0, enabled, 3, 2));
  EXPECT_EQ(SlotActivation::Connects,      Policy::classifySlotActivation(1, enabled, 3, 2));
  // slot 3 (index 2) is the case in the field log: iterated, but skipped.
  EXPECT_EQ(SlotActivation::OverActiveCap, Policy::classifySlotActivation(2, enabled, 3, 2));
}

TEST(SlotActivation, NonPsramSlotsBeyondRuntimeArrayAreNeverIterated) {
  const bool enabled[6] = {true, true, true, true, false, false};
  // mqtt4/5/6 (index 3-5) are outside the 3-slot runtime array on non-PSRAM.
  EXPECT_EQ(SlotActivation::BeyondArray, Policy::classifySlotActivation(3, enabled, 3, 2));
  EXPECT_EQ(SlotActivation::BeyondArray, Policy::classifySlotActivation(5, enabled, 3, 2));
}

TEST(SlotActivation, RankCountsOnlyEnabledLowerSlots) {
  // Only slot index 2 enabled (1 and 2 disabled): it is the first enabled slot,
  // so it connects even though its index equals the cap.
  const bool enabled[6] = {false, false, true, false, false, false};
  EXPECT_EQ(SlotActivation::Connects, Policy::classifySlotActivation(2, enabled, 3, 2));

  // Gap in the middle: slots 0 and 2 enabled, slot 1 off. Slot 2 is rank 2 <= 2.
  const bool enabled2[6] = {true, false, true, false, false, false};
  EXPECT_EQ(SlotActivation::Connects, Policy::classifySlotActivation(2, enabled2, 3, 2));
}

TEST(SlotActivation, PsramFiveOfSixConnect) {
  // PSRAM: slot_count = 6, max_active = 5. Sixth enabled slot is over the cap.
  const bool enabled[6] = {true, true, true, true, true, true};
  EXPECT_EQ(SlotActivation::Connects,      Policy::classifySlotActivation(4, enabled, 6, 5));
  EXPECT_EQ(SlotActivation::OverActiveCap, Policy::classifySlotActivation(5, enabled, 6, 5));
}

TEST(SlotActivation, DisabledAndOutOfRangeSlots) {
  const bool enabled[6] = {true, false, true, false, false, false};
  EXPECT_EQ(SlotActivation::Disabled, Policy::classifySlotActivation(1, enabled, 3, 2));
  EXPECT_EQ(SlotActivation::Disabled, Policy::classifySlotActivation(-1, enabled, 3, 2));
  EXPECT_EQ(SlotActivation::Disabled, Policy::classifySlotActivation(0, nullptr, 3, 2));
}

using Policy::StaleTokenAction;

TEST(StaleToken, ConnectedSlotOnAnExpEnforcingBrokerBounces) {
  // The broker will reject the stale token, and esp-mqtt refuses reconnect() from
  // CONNECTED, so the transport has to close first.
  EXPECT_EQ(StaleTokenAction::Bounce,
            Policy::classifyStaleToken(/*minted=*/true, /*connected=*/true,
                                       /*broker_enforces_exp=*/true));
}

TEST(StaleToken, ConnectedSlotOnATolerantBrokerKeepsItsSession) {
  // waev leaves live sessions alone past exp. Bouncing would spend a 16 KiB
  // contiguous handshake to replace a session the broker was not going to drop.
  EXPECT_EQ(StaleTokenAction::KeepAlive,
            Policy::classifyStaleToken(true, true, /*broker_enforces_exp=*/false));
}

TEST(StaleToken, DisconnectedSlotReconnectsRegardlessOfBrokerPolicy) {
  EXPECT_EQ(StaleTokenAction::Reconnect, Policy::classifyStaleToken(true, false, true));
  EXPECT_EQ(StaleTokenAction::Reconnect, Policy::classifyStaleToken(true, false, false));
}

TEST(StaleToken, FailedMintNeverReconnects) {
  // Reconnecting here would re-present the credentials the correction invalidated.
  // Every combination defers — including the disconnected one, which is the case
  // that previously reconnected on the stale token.
  EXPECT_EQ(StaleTokenAction::Defer, Policy::classifyStaleToken(false, false, true));
  EXPECT_EQ(StaleTokenAction::Defer, Policy::classifyStaleToken(false, false, false));
  EXPECT_EQ(StaleTokenAction::Defer, Policy::classifyStaleToken(false, true, true));
  EXPECT_EQ(StaleTokenAction::Defer, Policy::classifyStaleToken(false, true, false));
}

using Policy::RenewalStep;
using Policy::OwedBounceStep;

TEST(RenewalGuard, BounceNeededOnlyForEnforcedExpOrNoSession) {
  EXPECT_TRUE(Policy::renewalBounceNeeded(/*imminent=*/true, /*enforces=*/true, /*connected=*/true));
  EXPECT_FALSE(Policy::renewalBounceNeeded(true, false, true));
  EXPECT_FALSE(Policy::renewalBounceNeeded(false, true, true));
  EXPECT_TRUE(Policy::renewalBounceNeeded(false, false, false));
}

TEST(RenewalGuard, BounceKeepsTheFullGuardAndPassBudget) {
  const uint32_t armed = 100000U;
  EXPECT_FALSE(Policy::bounceHandshakeAllowed(false, armed + 2000U, armed));
  EXPECT_FALSE(Policy::bounceHandshakeAllowed(false, armed + 14999U, armed));
  EXPECT_TRUE(Policy::bounceHandshakeAllowed(false, armed + 15000U, armed));
  // Another slot launched a handshake earlier in this pass.
  EXPECT_FALSE(Policy::bounceHandshakeAllowed(true, armed + 60000U, armed));
}

TEST(RenewalGuard, DueRenewalWithIdleGuardBouncesInTheSamePass) {
  const uint32_t last = 100000U;
  const uint32_t now = last + 60000U;
  EXPECT_EQ(RenewalStep::Bounce,
            Policy::renewalStep(true, true, true, Policy::bounceHandshakeAllowed(false, now, last)));
}

TEST(RenewalGuard, DueRenewalWaitsWhileTheGuardIsBusy) {
  const uint32_t last = 100000U;
  EXPECT_EQ(RenewalStep::Wait,
            Policy::renewalStep(true, true, true,
                                Policy::bounceHandshakeAllowed(false, last + 5000U, last)));
  EXPECT_EQ(RenewalStep::Wait,
            Policy::renewalStep(true, true, true,
                                Policy::bounceHandshakeAllowed(true, last + 60000U, last)));
}

TEST(RenewalGuard, RenewalWithoutABounceNeverWaits) {
  EXPECT_EQ(RenewalStep::Renew, Policy::renewalStep(true, true, false, false));
  EXPECT_EQ(RenewalStep::None, Policy::renewalStep(false, true, true, true));
  EXPECT_EQ(RenewalStep::None, Policy::renewalStep(true, false, true, true));
}

namespace {
// Mirrors maintainSlotConnection()'s order: decide, and only on Bounce touch the session.
struct FakeSlot {
  bool connected = true;
  bool owed = false;
  bool fresh_applied = false;
  int soft_disconnects = 0;
  int handshakes = 0;
  uint32_t started_at = 0;
};

struct FakeGuard {
  uint32_t last = 0;
  bool this_pass = false;
  void startPass(uint32_t now) { this_pass = Policy::reconnectGuardActive(now, last); }
  void arm(uint32_t now) { last = now; this_pass = true; }
};

void renewalPass(FakeSlot& s, FakeGuard& g, uint32_t now) {
  const RenewalStep step = Policy::renewalStep(
      true, true, Policy::renewalBounceNeeded(true, true, s.connected),
      Policy::bounceHandshakeAllowed(g.this_pass, now, g.last));
  if (step != RenewalStep::Bounce) return;
  s.soft_disconnects++;
  s.handshakes++;
  s.started_at = now;
  g.arm(now);
}

void owedPass(FakeSlot& s, FakeGuard& g, uint32_t now) {
  if (!s.owed) return;
  const OwedBounceStep step = Policy::owedBounceStep(
      s.fresh_applied, s.connected, true, Policy::bounceHandshakeAllowed(g.this_pass, now, g.last));
  if (step == OwedBounceStep::Wait) return;
  s.owed = false;
  if (step == OwedBounceStep::Settled) return;
  if (step == OwedBounceStep::Bounce) s.soft_disconnects++;
  s.handshakes++;
  s.started_at = now;
  g.arm(now);
}
}  // namespace

TEST(RenewalGuard, DeferredRenewalLeavesTheSessionAloneThenStartsWhenTheGuardClears) {
  FakeGuard g;
  FakeSlot s;
  const uint32_t armed = 200000U;
  // An earlier slot launched a handshake in this same pass.
  g.startPass(armed);
  g.arm(armed);
  renewalPass(s, g, armed);
  EXPECT_EQ(0, s.soft_disconnects);
  EXPECT_EQ(0, s.handshakes);
  for (uint32_t now = armed + 500U; now < armed + 15000U; now += 500U) {
    g.startPass(now);
    renewalPass(s, g, now);
    ASSERT_EQ(0, s.soft_disconnects) << "touched at " << now;
  }
  g.startPass(armed + 15000U);
  renewalPass(s, g, armed + 15000U);
  EXPECT_EQ(1, s.soft_disconnects);
  EXPECT_EQ(1, s.handshakes);
  EXPECT_EQ(armed + 15000U, s.started_at);
}

TEST(RenewalGuard, OwedCorrectedClockBouncesRunOnePerGuardWindowWithNoneSkipped) {
  FakeGuard g;
  g.last = 1000U;
  FakeSlot slots[5];
  for (auto& s : slots) s.owed = true;
  slots[3].connected = false;  // a down client reconnects without a bounce
  const uint32_t t0 = 50000U;
  for (uint32_t now = t0; now < t0 + 120000U; now += 250U) {
    g.startPass(now);
    for (auto& s : slots) owedPass(s, g, now);
  }
  for (int i = 0; i < 5; i++) {
    EXPECT_FALSE(slots[i].owed) << "slot " << i;
    EXPECT_EQ(1, slots[i].handshakes) << "slot " << i;
    if (i > 0) {
      EXPECT_GE(slots[i].started_at - slots[i - 1].started_at, Policy::kReconnectGuardMs);
    }
  }
  EXPECT_EQ(0, slots[3].soft_disconnects);
  EXPECT_EQ(1, slots[4].soft_disconnects);
  // The fifth handshake starts four guard windows after the first.
  EXPECT_EQ(slots[0].started_at + 4U * Policy::kReconnectGuardMs, slots[4].started_at);
}

TEST(RenewalGuard, OwedSlotAlreadyOnTheFreshTokenSettlesWithoutAHandshake) {
  EXPECT_EQ(OwedBounceStep::Settled, Policy::owedBounceStep(true, true, true, false));
  EXPECT_EQ(OwedBounceStep::Settled, Policy::owedBounceStep(false, true, false, false));
  EXPECT_EQ(OwedBounceStep::Wait, Policy::owedBounceStep(false, false, true, false));
  EXPECT_EQ(OwedBounceStep::Reconnect, Policy::owedBounceStep(false, false, false, true));
  EXPECT_EQ(OwedBounceStep::Bounce, Policy::owedBounceStep(false, true, true, true));

  FakeGuard g;
  FakeSlot slots[2];
  slots[0].owed = slots[1].owed = true;
  slots[0].fresh_applied = true;
  g.startPass(100000U);
  for (auto& s : slots) owedPass(s, g, 100000U);
  EXPECT_EQ(0, slots[0].handshakes);
  EXPECT_EQ(1, slots[1].handshakes);
}

namespace {
// Mirrors one maintenance pass over slot 0, whose ordinary reconnect is always rejected
// locally (arm, call fails, roll back), and slot 1, whose renewal bounce is due.
struct StarveSim {
  bool restore_on_reject = true;
  bool yield_to_waiting = true;
  uint32_t last = 0;
  int8_t armer = -1;
  uint8_t waiting_mask = 0;
  uint32_t retry_last_attempt = 0;
  int retry_attempts = 0;
  uint32_t first_retry_at = 0;
  bool renewal_due = true;
  int renewal_handshakes = 0;
  uint32_t renewal_started_at = 0;

  void pass(uint32_t now) {
    bool this_pass = Policy::reconnectGuardActive(now, last);
    uint8_t seen = 0;
    // Slot 0: ordinary backoff reconnect, rolled back to rung 0 on every local rejection.
    const bool yield = yield_to_waiting && Policy::yieldsToWaitingBounce(waiting_mask, 0);
    const bool released = this_pass && armer >= 0 &&
        Policy::reconnectGuardReleased(now, last, true, /*armer_handshake_done=*/true);
    if (!yield && (!this_pass || released) && Policy::reconnectDue(now, retry_last_attempt, 0, 0)) {
      retry_last_attempt = now;
      const uint32_t last_before = last;
      const int8_t armer_before = armer;
      this_pass = true;
      last = now;
      armer = Policy::reconnectArmsEarlyRelease(false, false) ? 0 : -1;
      if (retry_attempts++ == 0) first_retry_at = now;
      if (restore_on_reject) {
        last = last_before;
        armer = armer_before;
      }
    }
    // Slot 1: renewal bounce through the guard.
    const RenewalStep step = Policy::renewalStep(
        renewal_due, true, true, Policy::bounceHandshakeAllowed(this_pass, now, last));
    if (step == RenewalStep::Wait) {
      seen |= 2U;
    } else if (step == RenewalStep::Bounce) {
      renewal_handshakes++;
      renewal_started_at = now;
      renewal_due = false;
      last = now;
      armer = -1;
      this_pass = true;
    }
    waiting_mask = seen;
  }

  void run(uint32_t from, uint32_t to) {
    for (uint32_t now = from; now <= to; now += 100U) pass(now);
  }
};
}  // namespace

TEST(RenewalGuard, WithoutRestoreRepeatedLocalRejectionsStarveARenewal) {
  // The failure being fixed: each rejected retry re-stamps the guard every 10 s.
  StarveSim sim;
  sim.restore_on_reject = false;
  sim.yield_to_waiting = false;
  sim.last = 1000U;  // the last real handshake, by slot 0 itself
  sim.armer = 0;
  sim.retry_last_attempt = 1000U;
  sim.run(2000U, 600000U);
  EXPECT_EQ(0, sim.renewal_handshakes);
  EXPECT_GT(sim.retry_attempts, 50);
}

TEST(RenewalGuard, RejectedAttemptRestoresTheGuardSoTheWaitingBounceGoesOneWindowAfterTheRealHandshake) {
  StarveSim sim;
  sim.yield_to_waiting = false;  // the restore alone is enough
  sim.last = 1000U;
  sim.armer = 0;
  sim.retry_last_attempt = 1000U;
  sim.run(2000U, 600000U);
  EXPECT_EQ(1, sim.renewal_handshakes);
  EXPECT_EQ(1000U + Policy::kReconnectGuardMs, sim.renewal_started_at);
  EXPECT_EQ(11000U, sim.first_retry_at);  // a rejected retry ran inside the window and did not extend it
}

TEST(RenewalGuard, RepeatedLocalRejectionsDoNotStarveAnotherSlotsRenewal) {
  StarveSim sim;
  sim.last = 1000U;
  sim.armer = 0;
  sim.retry_last_attempt = 1000U;
  sim.run(2000U, 600000U);
  EXPECT_EQ(1, sim.renewal_handshakes);
  EXPECT_EQ(1000U + Policy::kReconnectGuardMs, sim.renewal_started_at);
  // The retrying slot stood aside while the renewal waited, then resumed after its real guard.
  EXPECT_EQ(sim.renewal_started_at + Policy::kReconnectGuardMs, sim.first_retry_at);
  EXPECT_GT(sim.retry_attempts, 50);
}

TEST(RenewalGuard, YieldIsOnlyToOtherWaitingSlots) {
  EXPECT_FALSE(Policy::yieldsToWaitingBounce(0, 0));
  EXPECT_FALSE(Policy::yieldsToWaitingBounce(1U << 2, 2));
  EXPECT_TRUE(Policy::yieldsToWaitingBounce(1U << 2, 0));
  EXPECT_TRUE(Policy::yieldsToWaitingBounce((1U << 2) | (1U << 0), 0));
}

TEST(RenewalGuard, OwedDispatchMintsWhenTheStagedTokenAgedIntoTheBuffer) {
  const uint32_t now = 1800000000U;
  const uint32_t buffer = 300U;
  // Clear of buffer plus the handshake margin: dispatch as staged.
  EXPECT_FALSE(Policy::owedDispatchNeedsMint(true, true, now, now + buffer + 61U, buffer));
  EXPECT_TRUE(Policy::owedDispatchNeedsMint(true, true, now, now + buffer + 60U, buffer));
  EXPECT_TRUE(Policy::owedDispatchNeedsMint(true, true, now, now + 10U, buffer));
  EXPECT_TRUE(Policy::owedDispatchNeedsMint(true, true, now, now - 1U, buffer));
  EXPECT_TRUE(Policy::owedDispatchNeedsMint(true, false, now, now + 86400U, buffer));
}

TEST(RenewalGuard, OwedDispatchWithAnAgedTokenMintsFirstAndHandshakesOnce) {
  const uint32_t buffer = 300U;
  const uint32_t lifetime = 86400U;
  const uint32_t now = 1800000000U;
  const uint32_t staged_cases[] = {now + 120U, now - 30U, now + lifetime};
  for (uint32_t staged : staged_cases) {
    uint32_t token_exp = staged;
    uint32_t applied = now - 5000U;  // pre-correction credential
    int mints = 0;
    int handshakes = 0;
    // Dispatch: refresh if needed, then the single reconnect records the token as applied.
    if (Policy::owedDispatchNeedsMint(true, true, now, token_exp, buffer)) {
      mints++;
      token_exp = now + lifetime;
    }
    handshakes++;
    applied = token_exp;
    // Later passes: the renewal path must not find this credential due again.
    for (uint32_t t = now + 1U; t < now + 3600U; t += 60U) {
      if (Policy::tokenNeedsRenewal(true, t, applied, buffer)) handshakes++;
    }
    EXPECT_EQ(staged == now + lifetime ? 0 : 1, mints) << staged;
    EXPECT_EQ(1, handshakes) << staged;
    EXPECT_GT(applied, now + buffer);
  }
}

TEST(RenewalGuard, FailedOwedReconnectIsRetriedByTheRenewalPathUnderTheGuard) {
  // The owed flag is cleared at dispatch; a failed reconnect leaves the applied expiry stale.
  const uint32_t now_s = 1800000000U;
  const uint32_t stale_applied = now_s - 5000U;
  ASSERT_TRUE(Policy::tokenNeedsRenewal(true, now_s, stale_applied, 300U));
  const uint32_t armed = 400000U;
  EXPECT_EQ(RenewalStep::Wait,
            Policy::renewalStep(true, true, true,
                                Policy::bounceHandshakeAllowed(false, armed + 1000U, armed)));
  EXPECT_EQ(RenewalStep::Bounce,
            Policy::renewalStep(true, true, true,
                                Policy::bounceHandshakeAllowed(false, armed + 15000U, armed)));
}

using Policy::ClockSource;

// 2026-01-01, the bridge's plausibility floor, and the 2024 placeholder
// ESP32RTCClock::begin() stamps into libc on a power-on reset.
static const uint32_t kFloor = 1767225600;
static const uint32_t kPowerOnPlaceholder = 1715770351;
static const uint32_t kPlausibleNow = 1786000000;

TEST(FallbackClock, PrefersTheSystemClockWhenItIsUsable) {
  // A clock SNTP set recently outranks an RTC that may have drifted.
  EXPECT_EQ(ClockSource::System,
            Policy::chooseFallbackClock(false, kPlausibleNow, kPlausibleNow - 900, kFloor));
}

TEST(FallbackClock, FallsBackToTheRtcOnAColdBoot) {
  // The case the system-clock-only check missed: libc holds the power-on
  // placeholder while a detected chip holds real time.
  EXPECT_EQ(ClockSource::Rtc,
            Policy::chooseFallbackClock(false, kPowerOnPlaceholder, kPlausibleNow, kFloor));
}

TEST(FallbackClock, NothingUsableStaysUnsynced) {
  EXPECT_EQ(ClockSource::None,
            Policy::chooseFallbackClock(false, kPowerOnPlaceholder, kPowerOnPlaceholder, kFloor));
  // rtc_time 0 is how a board with no clock to consult is passed in.
  EXPECT_EQ(ClockSource::None,
            Policy::chooseFallbackClock(false, kPowerOnPlaceholder, 0, kFloor));
}

TEST(FallbackClock, ServerValidationNeverAcceptsALocalClock) {
  // `set mqtt.ntp` asks whether that host answers. No clock can answer for it,
  // however plausible — this is the path where a typo has to fail.
  EXPECT_EQ(ClockSource::None,
            Policy::chooseFallbackClock(true, kPlausibleNow, kPlausibleNow, kFloor));
}

TEST(FallbackClock, TheFloorItselfIsAccepted) {
  EXPECT_EQ(ClockSource::System, Policy::chooseFallbackClock(false, kFloor, 0, kFloor));
  EXPECT_EQ(ClockSource::Rtc, Policy::chooseFallbackClock(false, kFloor - 1, kFloor, kFloor));
  EXPECT_EQ(ClockSource::None, Policy::chooseFallbackClock(false, kFloor - 1, kFloor - 1, kFloor));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
