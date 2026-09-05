#pragma once

// CUSTOM (TeTeHacko): shared channel plumbing for ChannelBot.h and
// ChannelFilter.h -- not part of upstream simple_repeater. Included at the
// bottom of MyMesh.cpp (before the other two), so these are plain MyMesh
// methods with full access to state.
//
// A repeater is normally deaf to channel traffic only because the two virtuals
// below are no-ops in mesh::Mesh (Mesh.h:160, Mesh.cpp:37). Overriding them
// decrypts matching GRP_TXT packets on the side; forwarding is untouched --
// Mesh::onRecvPacket() calls routeRecvPacket() either way (Mesh.cpp:237-259).
// The ONE deliberate exception is the ChannelFilter veto in onChannelText(),
// which uses packet->markDoNotRetransmit() -- the exact mechanism the ANON
// branch uses at Mesh.cpp:229 -- to stop THIS node re-airing a matched packet.
//
// Registry slots are MAX_KNOWN_CHANNELS(4) because searchChannelsByHash()
// callers cap matches at 4 (Mesh.cpp:247). A channel named by both the bot and
// the filter shares one slot (flags are OR-ed).

#if defined(BOT_CHANNEL_PSK) || defined(FILTER_CHANNEL_PSKS)

#include "ChanListUtil.h"

// Inbound text, after decrypt: 4 B timestamp + 1 B type + "sender: msg".
#define CHAN_TEXT_LEN  160

// The PSK is given as HEX, which is exactly the form `meshcore-cli get_channel
// <n>` prints -- no base64 round trip by hand, and no base64.hpp dependency in
// this build (only companion_radio pulls that in). 32 hex chars = 128-bit key
// (what the phone app generates), 64 = 256-bit. Shorter keys are zero-padded in
// the 32-byte secret, same as BaseChatMesh::addChannel().
// Returns the registry index, or -1 (bad hex/length, or registry full).
int MyMesh::channelAddFromPsk(const char* psk_hex, int psk_len, uint8_t flag) {
  if (psk_len != 32 && psk_len != 64) {
    MESH_DEBUG_PRINTLN("channelAddFromPsk: PSK must be 32 or 64 hex chars, got %d", psk_len);
    return -1;
  }
  int key_len = psk_len / 2;
  uint8_t secret[PUB_KEY_SIZE];
  memset(secret, 0, sizeof(secret));
  for (int i = 0; i < key_len; i++) {
    int hi = chan_hex_nibble(psk_hex[i * 2]);
    int lo = chan_hex_nibble(psk_hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      MESH_DEBUG_PRINTLN("channelAddFromPsk: PSK is not hex");
      return -1;
    }
    secret[i] = (uint8_t)((hi << 4) | lo);
  }

  // one slot per channel: a PSK named twice (bot + filter) just ORs its flags
  for (int i = 0; i < num_known_channels; i++) {
    if (memcmp(known_channels[i].secret, secret, sizeof(secret)) == 0) {
      known_channel_flags[i] |= flag;
      return i;
    }
  }
  if (num_known_channels >= MAX_KNOWN_CHANNELS) {
    MESH_DEBUG_PRINTLN("channelAddFromPsk: registry full (%d)", MAX_KNOWN_CHANNELS);
    return -1;
  }
  int idx = num_known_channels++;
  memcpy(known_channels[idx].secret, secret, sizeof(secret));
  // Same derivation as BaseChatMesh::addChannel() (hash over the REAL key
  // length, not the padded 32), so the hash matches what every other node
  // computes. Cross-checked live: the 128-bit #tth-test key hashes to 0x75.
  mesh::Utils::sha256(known_channels[idx].hash, sizeof(known_channels[idx].hash),
                      known_channels[idx].secret, key_len);
  known_channel_flags[idx] = flag;
  return idx;
}

void MyMesh::channelsInit() {
  num_known_channels = 0;
  memset(known_channels, 0, sizeof(known_channels));
  memset(known_channel_flags, 0, sizeof(known_channel_flags));
#ifdef FILTER_CHANNEL_PSKS
  filter_dropped = 0;
  {
    // comma-separated hex PSKs
    const char* seg = FILTER_CHANNEL_PSKS;
    while (*seg) {
      const char* comma = strchr(seg, ',');
      int len = comma ? (int)(comma - seg) : (int)strlen(seg);
      channelAddFromPsk(seg, len, CHAN_FLAG_FILTER);
      if (!comma) break;
      seg = comma + 1;
    }
  }
#endif
#ifdef BOT_CHANNEL_PSK
  botInit();
#endif
}

// Mirrors BaseChatMesh::searchChannelsByHash() over the registry. Matching on
// hash[0] alone is upstream's design: PATH_HASH_SIZE is 1, so collisions are
// expected and resolved by MACThenDecrypt() failing in the caller.
int MyMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  int n = 0;
  for (int i = 0; i < num_known_channels && n < max_matches; i++) {
    if (known_channels[i].hash[0] == hash[0]) {
      channels[n++] = known_channels[i];
    }
  }
  return n;
}

void MyMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel,
                             uint8_t* data, size_t len) {
  if (type != PAYLOAD_TYPE_GRP_TXT) return;   // GRP_DATA (telemetry etc.) is not ours
  if (len < 5) return;
  if ((data[4] >> 2) != 0) return;            // only plain text, per BaseChatMesh.cpp:374

  // which registry entry decrypted this? (the caller passes a COPY, not our slot)
  int chan_idx = -1;
  for (int i = 0; i < num_known_channels; i++) {
    if (memcmp(known_channels[i].secret, channel.secret, sizeof(channel.secret)) == 0) {
      chan_idx = i;
      break;
    }
  }
  if (chan_idx < 0) return;

  // Work on a bounded copy. Upstream NUL-terminates the caller's buffer in place
  // (data[len] = 0), which is fine there but relies on headroom this function
  // cannot verify -- and every other parse in this fork is bounds-checked.
  char text[CHAN_TEXT_LEN];
  size_t body = len - 5;
  if (body >= sizeof(text)) body = sizeof(text) - 1;
  memcpy(text, &data[5], body);
  text[body] = 0;

  // "<sender>: <msg>". A channel packet carries no identity; this prefix is the
  // only sender there is (and therefore spoofable -- see ChannelFilter.h).
  const char* sender = "";
  const char* msg = text;
  char* sep = strstr(text, ": ");
  if (sep) {
    *sep = 0;
    sender = text;
    msg = sep + 2;
  }
  onChannelText(packet, chan_idx, sender, msg);
}

void MyMesh::onChannelText(mesh::Packet* packet, int chan_idx, const char* sender, const char* msg) {
#ifdef FILTER_CHANNEL_PSKS
  if ((known_channel_flags[chan_idx] & CHAN_FLAG_FILTER)
      && sender[0] != 0 && filterDenied(sender)) {
    // Veto the forward of THIS packet -- routeRecvPacket() checks the mark
    // (Mesh.cpp:357). Local processing already happened (rxlog/pktfeed capture
    // on receive), so the observer evidence trail is intact; the node just
    // refuses to re-air it. And the bot must not answer a denied sender either.
    packet->markDoNotRetransmit();
    filter_dropped++;
    MESH_DEBUG_PRINTLN("filter: drop fwd, sender na deny listu");
    return;
  }
#endif
#ifdef BOT_CHANNEL_PSK
  if (chan_idx == bot_channel_idx) {
    botOnChannelText(packet, sender, msg);
  }
#endif
}

#endif   // BOT_CHANNEL_PSK || FILTER_CHANNEL_PSKS
