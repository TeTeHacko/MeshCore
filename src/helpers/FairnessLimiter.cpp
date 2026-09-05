#include "FairnessLimiter.h"

#include <MeshCore.h>
#include <cstdio>
#include <cstring>

uint8_t FairnessLimiter::peekGroup(const uint8_t* hash) {
  auto i = group_idx(hash);
  if (i >= GROUP_MAP_SIZE) {
    return 0;
  }
  return group_map[i];
}

bool FairnessLimiter::takeGroup(const uint8_t* hash) {
  auto i = group_idx(hash);
  bool allow = false;
  if (i < GROUP_MAP_SIZE && group_map[i] > 0) {
    group_map[i]--;
    allow = true;
  } else {
    denied_group++;
    if (i < GROUP_MAP_SIZE && group_deny[i] != 0xFFFF) group_deny[i]++;
  }
  MESH_DEBUG_PRINTLN("FAIR, group=%02X allow=%u", (uint32_t)hash[0], (uint32_t)allow);
  return allow;
}

void FairnessLimiter::refillGroup() {
  for (int i = 0; i < GROUP_MAP_SIZE; i++) {
    if (group_map[i] < group_cap) {
      group_map[i]++;
    }
  }
}

uint8_t FairnessLimiter::peekSenderNormal(const uint8_t* hash) {
  // Always zero as 0x00 and 0xFF are reserved
  if (hash[0] == 0x00 || hash[0] == 0xFF) {
    return 0;
  }

  auto i = sender_normal_idx(hash);
  if (i >= SENDER_NORMAL_MAP_SIZE) {
    return 0;
  }
  return sender_normal_map[i];
}

bool FairnessLimiter::takeSenderNormal(const uint8_t* hash) {
  // Always false as 0x00 and 0xFF are reserved
  if (hash[0] == 0x00 || hash[0] == 0xFF) {
    return false;
  }

  auto i = sender_normal_idx(hash);
  bool allow = false;
  if (i < SENDER_NORMAL_MAP_SIZE && sender_normal_map[i] > 0) {
    sender_normal_map[i]--;
    allow = true;
  } else {
    denied_sender_normal++;
    if (i < SENDER_NORMAL_MAP_SIZE && sender_normal_deny[i] != 0xFFFF) sender_normal_deny[i]++;
  }
  MESH_DEBUG_PRINTLN("FAIR, sender=%02X prio=normal allow=%u", (uint32_t)hash[0], (uint32_t)allow);
  return allow;
}

void FairnessLimiter::refillSenderNormal() {
  for (int i = 0; i < SENDER_NORMAL_MAP_SIZE; i++) {
    if (sender_normal_map[i] < sender_normal_cap) {
      sender_normal_map[i]++;
    }
  }
}

uint8_t FairnessLimiter::peekSenderLow(const uint8_t* hash) {
  // Always zero as 0x00 and 0xFF are reserved
  if (hash[0] == 0x00 || hash[0] == 0xFF) {
    return 0;
  }

  auto i = sender_low_idx(hash);
  if (i >= SENDER_LOW_MAP_SIZE) {
    return 0;
  }
  return sender_low_map[i];
}

bool FairnessLimiter::takeSenderLow(const uint8_t* hash) {
  // Always false as 0x00 and 0xFF are reserved
  if (hash[0] == 0x00 || hash[0] == 0xFF) {
    return false;
  }

  auto i = sender_low_idx(hash);
  bool allow = false;
  if (i < SENDER_LOW_MAP_SIZE && sender_low_map[i] > 0) {
    sender_low_map[i]--;
    allow = true;
  } else {
    denied_sender_low++;
    if (i < SENDER_LOW_MAP_SIZE && sender_low_deny[i] != 0xFFFF) sender_low_deny[i]++;
  }
  MESH_DEBUG_PRINTLN("FAIR, sender=%02X prio=low allow=%u", (uint32_t)hash[0], (uint32_t)allow);
  return allow;
}

void FairnessLimiter::refillSenderLow() {
  for (int i = 0; i < SENDER_LOW_MAP_SIZE; i++) {
    if (sender_low_map[i] < sender_low_cap) {
      sender_low_map[i]++;
    }
  }
}

bool FairnessLimiter::allowPacket(const mesh::Packet* pkt) {
  // Don't limit versions we don't understand.
  if (pkt->getPayloadVer() != PAYLOAD_VER_1) {
    return true;
  }

#if FAIRNESS_ALLOW_DIRECT
  // Don't limit direct
  if (!pkt->isRouteFlood()) {
    return true;
  }
#endif

  uint8_t hash[1] = { 0x00 };
  auto type = pkt->getPayloadType();

  // Packets by group channel hash
  if (type == PAYLOAD_TYPE_GRP_TXT || type == PAYLOAD_TYPE_GRP_DATA) {
    if (pkt->payload_len < 1) return true;   // malformed, not ours to judge
    hash[0] = pkt->payload[0];
    return takeGroup(hash);
  }

  // Packets with a plaintext 1-byte src hash at payload[1] (see Mesh.cpp,
  // PAYLOAD_TYPE_PATH/REQ/RESPONSE/TXT_MSG parse: dest_hash, then src_hash)
  if (type == PAYLOAD_TYPE_PATH || type == PAYLOAD_TYPE_REQ || type == PAYLOAD_TYPE_RESPONSE ||
      type == PAYLOAD_TYPE_TXT_MSG || type == PAYLOAD_TYPE_ANON_REQ) {
    if (pkt->payload_len < 2) return true;   // malformed, not ours to judge
    hash[0] = pkt->payload[1];
    return takeSenderNormal(hash);
  }

  if (type == PAYLOAD_TYPE_ADVERT) {
    if (pkt->payload_len < 1) return true;   // malformed, not ours to judge
    hash[0] = pkt->payload[0];               // first byte of sender pub key
    return takeSenderLow(hash);
  }

  // Not classified (allowed through):
  // PAYLOAD_TYPE_ACK, PAYLOAD_TYPE_TRACE, PAYLOAD_TYPE_MULTIPART,
  // PAYLOAD_TYPE_CONTROL, PAYLOAD_TYPE_RAW_CUSTOM
  return true;
}

// Append the busiest deny buckets of one category to `out`. Repeated max-scan
// over a small array (<=128) -- picks up to `top` entries, busiest first, and
// skips a bucket once emitted by nulling a scratch copy. Returns chars written.
static int fmt_deny_cat(char* out, int max_len, const char* label,
                        uint16_t* deny, int size, uint8_t cap, int top) {
  int w = snprintf(out, max_len, "%s(cap %u):", label, (unsigned)cap);
  if (w < 0 || w >= max_len) return (w < 0) ? 0 : max_len - 1;
  bool any = false;
  for (int n = 0; n < top; n++) {
    int best = -1;
    uint16_t best_v = 0;
    for (int i = 0; i < size; i++) {
      if (deny[i] > best_v) { best_v = deny[i]; best = i; }
    }
    if (best < 0) break;   // no non-zero left
    int adv = snprintf(out + w, max_len - w, " %02x=%u", (unsigned)best, (unsigned)best_v);
    if (adv < 0 || adv >= max_len - w) { out[w] = 0; break; }   // no room, stop clean
    w += adv;
    any = true;
    deny[best] = 0;   // consumed for the next pass; `deny` is formatGrid's scratch copy
  }
  if (!any) {
    int adv = snprintf(out + w, max_len - w, " -");
    if (adv > 0 && adv < max_len - w) w += adv;
  }
  return w;
}

int FairnessLimiter::formatGrid(char* out, int max_len) const {
  // Work on scratch copies so the busiest-first selection can null entries as
  // it consumes them without disturbing the live counters.
  static uint16_t g[GROUP_MAP_SIZE], sn[SENDER_NORMAL_MAP_SIZE], sl[SENDER_LOW_MAP_SIZE];
  memcpy(g, group_deny, sizeof(g));
  memcpy(sn, sender_normal_deny, sizeof(sn));
  memcpy(sl, sender_low_deny, sizeof(sl));

  int w = 0;
  w += fmt_deny_cat(out + w, max_len - w, "group", g, GROUP_MAP_SIZE, group_cap, 6);
  if (w < max_len - 2) { int a = snprintf(out + w, max_len - w, " | "); if (a > 0) w += a; }
  w += fmt_deny_cat(out + w, max_len - w, "sender", sn, SENDER_NORMAL_MAP_SIZE, sender_normal_cap, 6);
  if (w < max_len - 2) { int a = snprintf(out + w, max_len - w, " | "); if (a > 0) w += a; }
  w += fmt_deny_cat(out + w, max_len - w, "advert", sl, SENDER_LOW_MAP_SIZE, sender_low_cap, 4);
  return w;
}
