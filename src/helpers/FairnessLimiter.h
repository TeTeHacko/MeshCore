#pragma once

// CUSTOM (TeTeHacko): port of Beanow's "fairness limiter" (upstream proposal
// meshcore-dev/MeshCore#1502, branch beanow/wp-repeat). Token-bucket rate
// limiting of FORWARDED packets, bucketed by the 1-byte identifiers that are
// already in the clear on the air -- so it needs no protocol change and no
// decryption. It is an approximation by design: buckets collide (masks below)
// and every key is sender-controlled, so this is containment of accidental or
// lazy floods, not security. DIRECT (routed) traffic is never limited.
//
// Deviations from Beanow's branch, on purpose:
//   * no RTCClock dependency -- it was only feeding log timestamps; logging
//     goes through MESH_DEBUG_PRINTLN like the rest of this tree, which also
//     lets the class run in native unit tests without a clock mock.
//   * allowPacket() bounds-checks payload_len before reading payload bytes.
//     The whole CI suite runs under ASAN because of earlier OOB parses; a
//     limiter keyed on payload[1] must not be the next one.

#include <Packet.h>
#include <stdint.h>

// For senders 0x00 and 0xFF are reserved (Identity.cpp rejects them as path
// hash prefixes). Group channel hashes are pure sha256, so no reserved values.
// Note: mask, cap and refill interval are highly connected parameters.
#define GROUP_MASK                       0b00011111 // 5-bit address
#define SENDER_NORMAL_MASK               0b00111111 // 6-bit address
#define SENDER_LOW_MASK                  0b01111111 // 7-bit address

// Refill intervals: ONE token per bucket per interval. Steady-state rate is
// 1/interval, cap bounds the burst. Overridable per env from platformio ini.
#ifndef FAIRNESS_GROUP_REFILL_MS
  #define FAIRNESS_GROUP_REFILL_MS         (3 * 60 * 1000)
#endif
#ifndef FAIRNESS_SENDER_NORMAL_REFILL_MS
  #define FAIRNESS_SENDER_NORMAL_REFILL_MS (4 * 60 * 1000)
#endif
#ifndef FAIRNESS_SENDER_LOW_REFILL_MS
  #define FAIRNESS_SENDER_LOW_REFILL_MS    (120 * 60 * 1000)
#endif

// Capacity (max burst) per bucket.
#ifndef FAIRNESS_GROUP_CAP
  #define FAIRNESS_GROUP_CAP               20
#endif
#ifndef FAIRNESS_SENDER_NORMAL_CAP
  #define FAIRNESS_SENDER_NORMAL_CAP       30
#endif
#ifndef FAIRNESS_SENDER_LOW_CAP
  #define FAIRNESS_SENDER_LOW_CAP          4
#endif

// Simply allow all direct messages?
#ifndef FAIRNESS_ALLOW_DIRECT
  #define FAIRNESS_ALLOW_DIRECT            1
#endif

// Derived from the masks, WONT fit in uint8 when full 0xFF mask.
#define GROUP_MAP_SIZE                   (GROUP_MASK + 1)
#define SENDER_NORMAL_MAP_SIZE           (SENDER_NORMAL_MASK + 1)
#define SENDER_LOW_MAP_SIZE              (SENDER_LOW_MASK + 1)

/**
 * A token bucket rate limiter, which attempts to limit by sender or group channel.
 *
 * It provides some mitigation against accidental spam, as well as DoS attacks.
 * However it's merely an approximation, as there will be ID collisions and easy
 * ways to game it.
 *
 * Until a V2 protocol provides new means of limiting, the goal is a "better than
 * nothing" reduction in wasteful repeats, freeing airtime for useful repeats.
 */
class FairnessLimiter {
  // Capacity of when a bucket is full. Runtime-settable (setGroupCap etc.) so
  // limits can be loosened over RF admin without a reflash -- 0 to a setter
  // resets to the build default below.
  uint8_t group_cap = FAIRNESS_GROUP_CAP, sender_normal_cap = FAIRNESS_SENDER_NORMAL_CAP,
          sender_low_cap = FAIRNESS_SENDER_LOW_CAP;

  // Deny stats
  uint32_t denied_group = 0, denied_sender_normal = 0, denied_sender_low = 0;
  // Token buckets
  uint8_t sender_normal_map[SENDER_NORMAL_MAP_SIZE] = {};
  uint8_t sender_low_map[SENDER_LOW_MAP_SIZE] = {};
  uint8_t group_map[GROUP_MAP_SIZE] = {};
  // Per-bucket deny counters -- the "grid": which hash buckets we are actually
  // shedding, so a shed is visible instead of a bare total. Saturating uint16.
  uint16_t group_deny[GROUP_MAP_SIZE] = {};
  uint16_t sender_normal_deny[SENDER_NORMAL_MAP_SIZE] = {};
  uint16_t sender_low_deny[SENDER_LOW_MAP_SIZE] = {};

public:
  FairnessLimiter() {}

  uint8_t peekGroup(const uint8_t* hash);
  void refillGroup();
  uint8_t peekSenderNormal(const uint8_t* hash);
  void refillSenderNormal();
  uint8_t peekSenderLow(const uint8_t* hash);
  void refillSenderLow();

  /**
   * \brief  Classify the packet and take a token from the matching bucket.
   *   CONSUMES a token when one is available -- call it only for a packet that
   *   would otherwise be forwarded, and only once per packet.
   * \returns true if the packet should be forwarded
   */
  bool allowPacket(const mesh::Packet* pkt);

  // Inspect stats
  uint32_t deniedSenderNormal() const { return denied_sender_normal; }
  uint32_t deniedSenderLow() const { return denied_sender_low; }
  uint32_t deniedGroup() const { return denied_group; }

  // Runtime cap tuning (0 = restore the build default for that bucket).
  void setGroupCap(uint8_t c) { group_cap = c ? c : FAIRNESS_GROUP_CAP; }
  void setSenderNormalCap(uint8_t c) { sender_normal_cap = c ? c : FAIRNESS_SENDER_NORMAL_CAP; }
  void setSenderLowCap(uint8_t c) { sender_low_cap = c ? c : FAIRNESS_SENDER_LOW_CAP; }
  uint8_t groupCap() const { return group_cap; }
  uint8_t senderNormalCap() const { return sender_normal_cap; }
  uint8_t senderLowCap() const { return sender_low_cap; }

  // The grid: append "<pfx>=<count>" for each bucket with denies, per category,
  // busiest first, into `out` (bounded by max_len). Returns chars written.
  // `pfx` is the low bits of the sender/channel hash -- cross-reference the
  // observer feed / CoreScope to map a hot bucket back to an actual node.
  int formatGrid(char* out, int max_len) const;

  static int sender_normal_idx(const uint8_t* hash) { return hash[0] & SENDER_NORMAL_MASK; }
  static int sender_low_idx(const uint8_t* hash) { return hash[0] & SENDER_LOW_MASK; }
  static int group_idx(const uint8_t* hash) { return hash[0] & GROUP_MASK; }

protected:
  bool takeSenderNormal(const uint8_t* hash);
  bool takeSenderLow(const uint8_t* hash);
  bool takeGroup(const uint8_t* hash);
};
