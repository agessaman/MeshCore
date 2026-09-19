#pragma once

#include <stddef.h>
#include <stdint.h>

namespace NetworkHostname {

// ESP-IDF documents a 32-byte hostname limit. Keep one byte for the trailing
// NUL so the same buffer is safe through Arduino and lwIP APIs.
static constexpr size_t kMaxLength = 31;
static constexpr size_t kBufferSize = kMaxLength + 1;

static inline bool isAsciiAlphaNumeric(uint8_t ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9');
}

static inline char asciiLower(uint8_t ch) {
  return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A'))
                                : static_cast<char>(ch);
}

/**
 * Build a DHCP-safe hostname from the MeshCore node name.
 *
 * The result is lowercase, starts with "meshcore-", contains only letters,
 * digits and hyphens, and never exceeds ESP-IDF's practical 31-character
 * payload limit. Six hex digits from the stable node identity are retained
 * whenever non-ASCII bytes are removed, a fallback is needed, or the readable
 * name must be shortened. This keeps lossy sanitization from assigning the
 * same DHCP identity to unrelated nodes.
 */
static inline bool build(char* dest, size_t dest_size, const char* node_name,
                         const uint8_t* stable_id, size_t stable_id_size) {
  if (!dest || dest_size == 0) return false;
  dest[0] = '\0';

  static constexpr char kPrefix[] = "meshcore-";
  static constexpr char kFallback[] = "node";
  static constexpr char kHex[] = "0123456789abcdef";
  static constexpr size_t kPrefixLength = sizeof(kPrefix) - 1;
  static constexpr size_t kSuffixLength = 7;  // '-' plus six hex digits.

  // NodePrefs::node_name currently holds at most 31 bytes. A larger scratch
  // buffer keeps this helper safe and independently testable with other input.
  char slug[64];
  size_t slug_length = 0;
  bool separator_pending = false;
  bool removed_non_ascii = false;
  if (node_name) {
    for (size_t i = 0; node_name[i] != '\0' && slug_length < sizeof(slug) - 1;
         ++i) {
      const uint8_t ch = static_cast<uint8_t>(node_name[i]);
      if (isAsciiAlphaNumeric(ch)) {
        if (separator_pending && slug_length > 0 &&
            slug_length < sizeof(slug) - 1) {
          slug[slug_length++] = '-';
        }
        if (slug_length < sizeof(slug) - 1) {
          slug[slug_length++] = asciiLower(ch);
        }
        separator_pending = false;
      } else {
        if (ch >= 0x80) removed_non_ascii = true;
        if (slug_length > 0) separator_pending = true;
      }
    }
  }

  const bool used_fallback = slug_length == 0;
  if (slug_length == 0) {
    for (size_t i = 0; i < sizeof(kFallback) - 1; ++i) {
      slug[slug_length++] = kFallback[i];
    }
  }
  slug[slug_length] = '\0';

  size_t max_length = dest_size - 1;
  if (max_length > kMaxLength) max_length = kMaxLength;
  if (max_length == 0) return false;

  const bool needs_truncation = kPrefixLength + slug_length > max_length;
  const bool needs_identity = needs_truncation || removed_non_ascii || used_fallback;
  const bool can_add_identity = needs_identity && stable_id &&
      stable_id_size >= 3 && max_length > kPrefixLength + kSuffixLength;
  const size_t suffix_length = can_add_identity ? kSuffixLength : 0;
  const size_t prefix_length = kPrefixLength < max_length
      ? kPrefixLength : max_length;
  size_t slug_budget = max_length - prefix_length - suffix_length;
  if (slug_budget > slug_length) slug_budget = slug_length;
  while (slug_budget > 0 && slug[slug_budget - 1] == '-') --slug_budget;

  size_t out = 0;
  for (size_t i = 0; i < prefix_length; ++i) dest[out++] = kPrefix[i];
  for (size_t i = 0; i < slug_budget; ++i) dest[out++] = slug[i];
  if (can_add_identity) {
    dest[out++] = '-';
    for (size_t i = 0; i < 3; ++i) {
      dest[out++] = kHex[(stable_id[i] >> 4) & 0x0f];
      dest[out++] = kHex[stable_id[i] & 0x0f];
    }
  }
  dest[out] = '\0';
  return true;
}

}  // namespace NetworkHostname
