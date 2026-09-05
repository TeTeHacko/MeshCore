#include "FairnessLimiter.h"

#include <MeshCore.h>

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
