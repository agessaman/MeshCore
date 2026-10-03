#pragma once
#include <stdint.h>
#include <string.h>

// OTA release-channel selector, persisted in NodePrefs::ota_channel.
enum OtaChannel : uint8_t {
  OTA_CH_NATIVE = 0,  // follow the channel this build was made for
  OTA_CH_STABLE = 1,  // production
  OTA_CH_DEV    = 2,  // beta
};

// Resolve the effective manifest base URL for a channel selector.
// build.sh injects the three bases as compile-time macros:
//   OTA_MANIFEST_BASE        = this build's native channel (defined on every OTA build)
//   OTA_MANIFEST_BASE_STABLE = stable (production) channel
//   OTA_MANIFEST_BASE_DEV    = dev (beta) channel
// stable/dev fall back to the native base when their macro is undefined (legacy/local
// builds that only define OTA_MANIFEST_BASE), so this never returns nullptr on an
// OTA-capable build. On a non-OTA build it returns nullptr.
//
// Each base is stored behind an "ota-base-<channel>:" tag so CI can read a binary's
// channels back with `strings` (scripts/verify_ota_channel.sh). Returning a pointer
// into the tagged array keeps the tag referenced, so the linker cannot drop it.
#if defined(OTA_MANIFEST_BASE)
#define OTA_BASE_TAG_NATIVE "ota-base-native:"
#define OTA_BASE_TAG_STABLE "ota-base-stable:"
#define OTA_BASE_TAG_DEV    "ota-base-dev:"
#if !defined(OTA_MANIFEST_BASE_STABLE)
#define OTA_MANIFEST_BASE_STABLE OTA_MANIFEST_BASE
#endif
#if !defined(OTA_MANIFEST_BASE_DEV)
#define OTA_MANIFEST_BASE_DEV OTA_MANIFEST_BASE
#endif
static const char ota_tagged_native[] = OTA_BASE_TAG_NATIVE OTA_MANIFEST_BASE;
static const char ota_tagged_stable[] = OTA_BASE_TAG_STABLE OTA_MANIFEST_BASE_STABLE;
static const char ota_tagged_dev[]    = OTA_BASE_TAG_DEV OTA_MANIFEST_BASE_DEV;
#endif

static inline const char* ota_resolve_base(uint8_t channel) {
#if defined(OTA_MANIFEST_BASE)
  switch (channel) {
    case OTA_CH_STABLE: return ota_tagged_stable + sizeof(OTA_BASE_TAG_STABLE) - 1;
    case OTA_CH_DEV:    return ota_tagged_dev + sizeof(OTA_BASE_TAG_DEV) - 1;
    case OTA_CH_NATIVE:
    default:            return ota_tagged_native + sizeof(OTA_BASE_TAG_NATIVE) - 1;
  }
#else
  (void)channel;
  return nullptr;
#endif
}

// Human label for a selector (for the `ota branch` report).
static inline const char* ota_channel_name(uint8_t channel) {
  switch (channel) {
    case OTA_CH_STABLE: return "prod";
    case OTA_CH_DEV:    return "beta";
    default:            return "default";
  }
}

// Label for the channel this build was made for, by matching its native base.
static inline const char* ota_native_channel_name() {
  const char* native = ota_resolve_base(OTA_CH_NATIVE);
  if (native == nullptr) return "none";
  if (strcmp(native, ota_resolve_base(OTA_CH_STABLE)) == 0) return "prod";
  if (strcmp(native, ota_resolve_base(OTA_CH_DEV)) == 0) return "beta";
  return "custom";
}

// Parse an `ota branch` argument. Returns true and sets *out on a known keyword
// (prod|stable, beta|dev, default); returns false and leaves *out untouched otherwise.
static inline bool ota_parse_channel(const char* arg, uint8_t* out) {
  if (strcmp(arg, "prod") == 0 || strcmp(arg, "stable") == 0) { *out = OTA_CH_STABLE; return true; }
  if (strcmp(arg, "beta") == 0 || strcmp(arg, "dev") == 0)    { *out = OTA_CH_DEV;    return true; }
  if (strcmp(arg, "default") == 0)                            { *out = OTA_CH_NATIVE; return true; }
  return false;
}
