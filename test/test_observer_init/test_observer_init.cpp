#include <gtest/gtest.h>
#include "helpers/RequiredTransportBuffer.h"
#include "../../lib/PsychicMqttClient/src/MqttClientInit.h"

TEST(ObserverInit, MissingPaddingPreventsSdkInitialization) {
  bool initialized = false, installed = false, freed = false;
  int transport;
  int* result = initWithRequiredTransportBuffer<int*>(1025,
      [](size_t) -> void* { return nullptr; },
      [&] { initialized = true; return &transport; },
      [&](int*, void*) { installed = true; },
      [&](void*) { freed = true; });
  EXPECT_EQ(nullptr, result);
  EXPECT_FALSE(initialized);
  EXPECT_FALSE(installed);
  EXPECT_FALSE(freed);
}

TEST(ObserverInit, FailedSdkInitReleasesReservedPadding) {
  char buffer[1025];
  bool freed = false, installed = false;
  int* result = initWithRequiredTransportBuffer<int*>(sizeof(buffer),
      [&](size_t n) -> void* { EXPECT_EQ(sizeof(buffer), n); return buffer; },
      []() -> int* { return nullptr; },
      [&](int*, void*) { installed = true; },
      [&](void* p) { EXPECT_EQ(buffer, p); freed = true; });
  EXPECT_EQ(nullptr, result);
  EXPECT_TRUE(freed);
  EXPECT_FALSE(installed);
}

TEST(ObserverInit, SuccessfulTransportOwnsProtectionBuffer) {
  char buffer[1025];
  int transport;
  bool installed = false, freed = false;
  int* result = initWithRequiredTransportBuffer<int*>(sizeof(buffer),
      [&](size_t) -> void* { return buffer; },
      [&] { return &transport; },
      [&](int* t, void* p) { EXPECT_EQ(&transport, t); EXPECT_EQ(buffer, p); installed = true; },
      [&](void*) { freed = true; });
  EXPECT_EQ(&transport, result);
  EXPECT_TRUE(installed);
  EXPECT_FALSE(freed);
}

TEST(ObserverInit, FailedRegistrationDestroysCandidateAndCanRetry) {
  int candidate;
  int* client = nullptr;
  int registrations = 0, destroys = 0;
  auto create = [&] { return &candidate; };
  auto register_events = [&](int* p) { EXPECT_EQ(&candidate, p); return ++registrations == 1 ? -7 : 0; };
  auto destroy = [&](int* p) { EXPECT_EQ(&candidate, p); ++destroys; };
  EXPECT_EQ(-7, initializeMqttClient(client, create, register_events, destroy, -2));
  EXPECT_EQ(nullptr, client);
  EXPECT_EQ(1, destroys);
  EXPECT_EQ(0, initializeMqttClient(client, create, register_events, destroy, -2));
  EXPECT_EQ(&candidate, client);
  EXPECT_EQ(1, destroys);
}

TEST(ObserverInit, FailedClientAllocationDoesNotRegisterOrDestroy) {
  int* client = nullptr;
  int effects = 0;
  EXPECT_EQ(-2, initializeMqttClient(client, []() -> int* { return nullptr; },
      [&](int*) { ++effects; return 0; }, [&](int*) { ++effects; }, -2));
  EXPECT_EQ(nullptr, client);
  EXPECT_EQ(0, effects);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
