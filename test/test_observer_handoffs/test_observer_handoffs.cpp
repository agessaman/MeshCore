#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "helpers/ObserverMailbox.h"
#include "helpers/ObserverConfigMailbox.h"
#include "helpers/ObserverAsyncJob.h"
#include "helpers/MQTTEventChannel.h"
#include "helpers/MQTTSlotEvents.h"
#include "helpers/ObserverPreferenceCommit.h"
#include "helpers/ObserverSlotStatsJson.h"


struct Image { uint32_t sequence = 0; uint32_t fields[64]{}; };

TEST(ObserverHandoffs, snapshot_reads_never_mix_publications) {
  ObserverMailbox<Image> mailbox;
  std::atomic<bool> done{false}, consistent{true};
  std::thread writer([&] {
    for (uint32_t n = 1; n <= 10000; ++n) {
      Image image;
      image.sequence = n;
      for (auto& field : image.fields) field = n;
      mailbox.publish(image);
    }
    done = true;
  });
  do {
    Image image = mailbox.read();
    for (auto field : image.fields) if (field != image.sequence) consistent = false;
  } while (!done.load());
  writer.join();
  ASSERT_TRUE(consistent.load());
  EXPECT_EQ(10000, mailbox.read().sequence);
}

TEST(ObserverHandoffs, config_requests_coalesce_with_latest_complete_image) {
  ObserverConfigMailbox<Image, uint32_t> mailbox;
  Image first, second, applied;
  first.sequence = 10;
  second.sequence = 11;
  mailbox.publish(first, 100, 1);
  mailbox.publish(second, 200, 4);
  uint32_t revision = 0, metadata = 0, mask = 0;
  ASSERT_TRUE(mailbox.consume(applied, metadata, revision, mask));
  EXPECT_EQ(11, applied.sequence);
  EXPECT_EQ(200, metadata);
  EXPECT_EQ(5, mask);
  EXPECT_EQ(2, revision);
  ASSERT_FALSE(mailbox.consume(applied, metadata, revision, mask));
  mailbox.publish(second, 200);
  EXPECT_EQ(2, mailbox.revision());
  mailbox.publish(second, 200, 2);
  ASSERT_TRUE(mailbox.consume(applied, metadata, revision, mask));
  EXPECT_EQ(2, mask);
}

TEST(ObserverHandoffs, config_copy_is_coherent_during_concurrent_replacement) {
  ObserverConfigMailbox<Image, uint32_t> mailbox;
  std::atomic<bool> done{false}, consistent{true};
  std::thread writer([&] {
    for (uint32_t n = 1; n <= 10000; ++n) {
      Image image;
      image.sequence = n;
      for (auto& field : image.fields) field = n;
      mailbox.publish(image, n);
    }
    done = true;
  });
  Image image;
  uint32_t revision = 0, metadata = 0, mask = 0;
  do {
    if (mailbox.consume(image, metadata, revision, mask)) {
      if (metadata != image.sequence) consistent = false;
      for (auto field : image.fields) if (field != metadata) consistent = false;
    }
  } while (!done.load());
  writer.join();
  ASSERT_TRUE(consistent.load());
}

TEST(ObserverHandoffs, overflow_identifies_every_affected_slot) {
  MqttEventChannel<2> events;
  MqttSlotEvent event;
  event.slot = 0;
  event.incarnation = 9;
  ASSERT_TRUE(events.push(event));
  event.kind = MqttSlotEvent::Kind::Connected;
  ASSERT_TRUE(events.push(event));
  event.slot = 1;
  ASSERT_FALSE(events.push(event));
  event.slot = 4;
  ASSERT_FALSE(events.push(event));
  EXPECT_EQ(18, events.takeOverflowSlots());
  EXPECT_EQ(0, events.takeOverflowSlots());
  EXPECT_EQ(2, events.overflows());
  ASSERT_TRUE(events.pop(event));
  ASSERT_TRUE(event.kind == MqttSlotEvent::Kind::Disconnected);
  EXPECT_EQ(9, event.incarnation);
  ASSERT_TRUE(events.pop(event));
  ASSERT_TRUE(event.kind == MqttSlotEvent::Kind::Connected);
  ASSERT_FALSE(events.pop(event));
}

TEST(ObserverHandoffs, retired_slot_is_discarded_without_losing_other_slots) {
  MqttEventChannel<3> events;
  MqttSlotEvent event;
  event.slot = 1;
  events.push(event);
  events.pop(event);  // move the ring head off zero
  event.slot = 0; event.incarnation = 5; events.push(event);
  event.slot = 1; event.incarnation = 6; events.push(event);
  event.slot = 0; event.incarnation = 7; events.push(event);
  events.discardSlot(0);
  ASSERT_TRUE(events.pop(event));
  EXPECT_EQ(1, event.slot);
  EXPECT_EQ(6, event.incarnation);
  ASSERT_FALSE(events.pop(event));
}

TEST(ObserverHandoffs, concurrent_callbacks_preserve_each_producer_order) {
  MqttEventChannel<32> events;
  std::atomic<int> done{0};
  auto producer = [&](uint8_t slot) {
    for (uint32_t n = 1; n <= 2000; ++n) {
      MqttSlotEvent event;
      event.slot = slot; event.at_ms = n;
      while (!events.push(event)) std::this_thread::yield();
    }
    ++done;
  };
  std::thread first(producer, 0), second(producer, 1);
  uint32_t last[2]{};
  bool ordered = true;
  MqttSlotEvent event;
  while (done.load() != 2 || last[0] != 2000 || last[1] != 2000) {
    if (events.pop(event)) {
      if (event.at_ms != last[event.slot] + 1) ordered = false;
      last[event.slot] = event.at_ms;
    }
  }
  first.join(); second.join();
  ASSERT_TRUE(ordered);
}

TEST(ObserverHandoffs, job_polling_does_not_restart_or_misattribute_a_result) {
  ObserverAsyncJob<uint32_t> job;
  ASSERT_TRUE(job.request(10));
  ASSERT_FALSE(job.request(11));
  const uint32_t id = job.begin();
  EXPECT_EQ(1, id);
  ASSERT_FALSE(job.request(12));
  EXPECT_EQ(0, job.begin());
  job.complete(id, 20, 42);
  auto result = job.read();
  ASSERT_TRUE(result.state == ObserverAsyncJob<uint32_t>::State::Complete);
  EXPECT_EQ(42, result.result);
  EXPECT_EQ(20, result.finished_ms);
  ASSERT_TRUE(job.request(30));
  const uint32_t second = job.begin();
  job.complete(id, 40, 99);
  EXPECT_EQ(second, job.read().id);
  ASSERT_TRUE(job.read().state == ObserverAsyncJob<uint32_t>::State::Running);
  job.complete(second, 45, 123);
  EXPECT_EQ(123, job.read().result);
}

TEST(ObserverHandoffs, stopped_job_rejects_late_completion_and_can_restart) {
  ObserverAsyncJob<uint32_t> job;
  job.request(0xfffffff0);
  const uint32_t id = job.begin();
  job.cancel(5);
  job.complete(id, 6, 7);
  ASSERT_TRUE(job.read().state == ObserverAsyncJob<uint32_t>::State::Cancelled);
  ASSERT_TRUE(job.request(8));
  EXPECT_NE(id, job.begin());
}

struct TestSlot {
  MqttClientState client_state = MqttClientState::Configured;
  uint32_t incarnation = 3, generation = 0;
  bool enabled = true, connected = false, circuit_breaker_tripped = true;
  uint32_t token_expires_at = 123, applied_token_expires_at = 0;
  uint32_t connected_at_ms = 0, last_error_time = 0, current_outage_started_ms = 0;
  uint32_t disconnect_count = 0, first_disconnect_time = 0;
  int32_t last_tls_err = 0, last_tls_stack_err = 0, last_sock_errno = 0;
  uint8_t last_connack_code = 0;
  uint8_t reconnect_backoff = 4;
};

TEST(ObserverHandoffs, ConnectedBeforeStartReturnsIsAppliedByTheOwner) {
  TestSlot slot;
  MqttEventChannel<2> events;
  bool mint = true, status = false;
  const int result = startMqttSlotAttempt(slot, [&] {
    EXPECT_TRUE(slot.client_state == MqttClientState::Starting);
    MqttSlotEvent event;
    event.kind = MqttSlotEvent::Kind::Connected;
    event.incarnation = slot.incarnation;
    event.at_ms = 10;
    events.push(event);  // SDK callback before start returns
    EXPECT_FALSE(slot.connected);
    return 0;
  });
  EXPECT_EQ(0, result);
  MqttSlotEvent event;
  ASSERT_TRUE(events.pop(event));
  ASSERT_TRUE(applyMqttSlotEvent(slot, event, mint, status));
  EXPECT_TRUE(slot.connected);
  EXPECT_TRUE(status);
  EXPECT_FALSE(mint);
  EXPECT_EQ(4, slot.reconnect_backoff);
  EXPECT_EQ(slot.token_expires_at, slot.applied_token_expires_at);
}

TEST(ObserverHandoffs, FailedStartRestoresStateWithoutApplyingCredential) {
  TestSlot slot;
  EXPECT_EQ(-1, startMqttSlotAttempt(slot, [] { return -1; }));
  EXPECT_TRUE(slot.client_state == MqttClientState::Configured);
  EXPECT_EQ(0u, slot.generation);
  EXPECT_EQ(0u, slot.applied_token_expires_at);
}

TEST(ObserverHandoffs, RetiredDisabledAndStoppedClientsCannotResurrect) {
  TestSlot slot;
  slot.client_state = MqttClientState::Starting;
  bool mint = false, status = false;
  MqttSlotEvent event;
  event.kind = MqttSlotEvent::Kind::Connected;
  event.incarnation = 2;
  EXPECT_FALSE(applyMqttSlotEvent(slot, event, mint, status));
  event.incarnation = 3;
  slot.enabled = false;
  EXPECT_FALSE(applyMqttSlotEvent(slot, event, mint, status));
  slot.enabled = true;
  for (auto state : {MqttClientState::Stopped, MqttClientState::Quarantined}) {
    slot.client_state = state;
    for (auto kind : {MqttSlotEvent::Kind::Connected, MqttSlotEvent::Kind::Disconnected, MqttSlotEvent::Kind::Error}) {
      event.kind = kind;
      EXPECT_FALSE(applyMqttSlotEvent(slot, event, mint, status));
    }
    EXPECT_EQ(state, slot.client_state);
  }
  EXPECT_FALSE(slot.connected);
  EXPECT_FALSE(status);
}

TEST(ObserverHandoffs, ErrorThenConnectClearsFailureAndDisconnectCancelsStatus) {
  TestSlot slot;
  slot.client_state = MqttClientState::Starting;
  bool mint = false, status = false;
  MqttSlotEvent event;
  event.incarnation = 3;
  event.kind = MqttSlotEvent::Kind::Error;
  event.connack = 5;
  event.at_ms = 100;
  ASSERT_TRUE(applyMqttSlotEvent(slot, event, mint, status));
  EXPECT_TRUE(mint);
  EXPECT_EQ(5, slot.last_connack_code);
  event.kind = MqttSlotEvent::Kind::Connected;
  event.at_ms = 200;
  ASSERT_TRUE(applyMqttSlotEvent(slot, event, mint, status));
  EXPECT_EQ(0, slot.last_connack_code);
  EXPECT_TRUE(status);
  event.kind = MqttSlotEvent::Kind::Disconnected;
  event.at_ms = 300;
  ASSERT_TRUE(applyMqttSlotEvent(slot, event, mint, status));
  EXPECT_FALSE(status);
  EXPECT_FALSE(slot.connected);
  EXPECT_EQ(300u, slot.current_outage_started_ms);
  EXPECT_EQ(1u, slot.disconnect_count);
}

TEST(ObserverHandoffs, RejectedAndIndeterminateCandidatesNeverBecomeDurable) {
  uint32_t durable = 1, candidate = 2;
  for (bool indeterminate : {false, true}) {
    EXPECT_FALSE(commitObserverPreferenceCandidate(durable, &candidate, [&] {
      EXPECT_EQ(1u, durable);  // A reader during flash I/O still sees the prior image.
      EXPECT_EQ(2u, candidate);
      (void)indeterminate;  // Both outcomes report false; CLI retains distinct text.
      return false;
    }));
    EXPECT_EQ(1u, durable);
  }
  EXPECT_TRUE(commitObserverPreferenceCandidate(durable, &candidate, [&] {
    EXPECT_EQ(1u, durable);
    candidate = 3;  // Serializer normalization is also private until commit.
    return true;
  }));
  EXPECT_EQ(3u, durable);
}

struct JsonSlot {
  bool configured = true;
  const char* name = "custom";
  const char* state = "ok";
  unsigned long publish_ok = 1, publish_err = 2;
  uint16_t filter_mask = MQTTPacketFilter::kAllPacketTypes;
};

TEST(ObserverHandoffs, SharedStatsPreserveSchemaAndOmitDefaultFilter) {
  char buf[256] = "{\"slots\":[";
  JsonSlot slots[2];
  slots[1].filter_mask = 8;
  appendObserverSlotStatsJson(buf, sizeof(buf), strlen(buf), slots, 2);
  EXPECT_STREQ("{\"slots\":[{\"n\":1,\"name\":\"custom\",\"state\":\"ok\",\"ok\":1,\"err\":2},{\"n\":2,\"name\":\"custom\",\"state\":\"ok\",\"ok\":1,\"err\":2,\"filt\":8}]}", buf);
}

TEST(ObserverHandoffs, StatsReserveClosingBracketsAtEveryBufferBoundary) {
  JsonSlot slot;
  char full[256] = "{\"slots\":[";
  const size_t prefix = strlen(full);
  appendObserverSlotStatsJson(full, sizeof(full), prefix, &slot, 1);
  for (size_t size = prefix + 3; size < strlen(full) + 1; ++size) {
    char buf[256];
    memset(buf, 'x', sizeof(buf));
    memcpy(buf, "{\"slots\":[", prefix + 1);
    appendObserverSlotStatsJson(buf, size, prefix, &slot, 1);
    EXPECT_STREQ("{\"slots\":[]}", buf);
    EXPECT_EQ('x', buf[size]);
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
