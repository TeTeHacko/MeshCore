#pragma once

// CUSTOM (TeTeHacko): channel bot -- not part of upstream simple_repeater.
// Included at the bottom of MyMesh.cpp, after ChannelCommon.h (which owns the
// channel registry, the PSK parsing, searchChannelsByHash() and the
// onGroupDataRecv() dispatcher that calls botOnChannelText() here), so these
// are plain MyMesh methods with full access to the analyzer state.
//
// WHY THIS LIVES IN THE REPEATER/ANALYZER BUILD
//
// The obvious place for a bot is companion_radio (it already has channels and
// contacts), but a companion build has no text CLI and no analyzer rings, so the
// observer node would stop feeding CoreScope. The reverse -- teaching this build
// to read one channel -- turned out to be small, because everything needed is
// already virtual in mesh::Mesh:
//
//   * Mesh::onRecvPacket() calls searchChannelsByHash() + onGroupDataRecv() and
//     THEN routeRecvPacket(), so decrypting a channel is independent of
//     forwarding it. Both defaults are no-ops (Mesh.cpp:37, Mesh.h:160), which
//     is the only reason a repeater is normally deaf to channel traffic.
//   * allowPacketForward() is consulted only for RECEIVED packets, so a node
//     with `repeat off` can still originate its own reply. That matters here:
//     x3 sits a few metres from tth-ltm and must never be a second repeater,
//     but it does have to be able to answer.
//   * createGroupDatagram() is in mesh::Mesh, not BaseChatMesh, and
//     sendFloodScoped() is a plain alias for sendFlood(), so no part of the
//     chat/contact machinery is needed to send.
//
// Written against the mesh::Mesh API only (no BaseChatMesh, no ClientACL, no
// DataStore) so it can be dropped into simple_room_server unchanged -- that
// example derives from the same base.

#ifdef BOT_CHANNEL_PSK

// Comma-separated, case-insensitive substrings; "*" answers anything, "" never.
#ifndef BOT_TRIGGER
  #define BOT_TRIGGER "ping,test"
#endif
// Senders whose messages are skipped outright, comma-separated node names.
// This is what keeps two bots on one channel from feeding each other: with
// TTH-L1 (Solo firmware) periodically putting "Ping" on #tth-test, an
// unfiltered trigger list made this node answer every one of them and burn the
// cooldown, so a human asking a question got silence.
#ifndef BOT_IGNORE_SENDERS
  #define BOT_IGNORE_SENDERS ""
#endif
// Whitelist of `!commands` this node answers, comma-separated, "" = all.
// The point is complementarity: two bots answering !path means two packets and
// one duplicate. Give each node the commands only it can answer well.
#ifndef BOT_COMMANDS
  #define BOT_COMMANDS ""
#endif
// Throttle, PER SENDER. Measured reason it is not global: with one global
// window, two people asking within it meant the second got silence -- observed
// twice, and it reads as "the bot is broken" rather than "wait 10 s".
#ifndef BOT_REPLY_COOLDOWN_MS
  #define BOT_REPLY_COOLDOWN_MS 10000
#endif
// Floor between ANY two replies, whoever asked. This is what still bounds the
// node's airtime on a shared community mesh once the main window went
// per-sender: N senders can no longer produce N simultaneous replies.
#ifndef BOT_MIN_GAP_MS
  #define BOT_MIN_GAP_MS 2500
#endif
// Senders tracked for the per-sender window. Wraps LRU; overflowing just means
// the oldest sender's window is forgotten early, i.e. one extra reply.
#ifndef BOT_PEER_SLOTS
  #define BOT_PEER_SLOTS 8
#endif

#define BOT_REPLY_LEN     120   // well under the 184 B packet payload
#define BOT_PATH_STR_LEN   64   // "3f:a1:c8..." -- caps how many hops we print
// One !command's answer. Sized for the WORST CASE of !link, which is the longest:
// botFormatPath() caps the path string at 62 chars (21 hops x 1 B, or 9 x 3 B, or
// 12 x 2 B -- all land just under BOT_PATH_STR_LEN), and the numbers in front of it
// add 36 ("SNR -20.5 dB, RSSI -128 dBm, 63 hop "). 98 + NUL.
//
// It was 72, sized against the longest COMMAND rather than the longest PATH, and
// that held only because !link had never been asked from further than 2 hops away.
// Measured 7. 8. 2026: an 11-hop request wanted 88 chars, so the reply truncated at
// 71 -- and with the old field order the part that got cut was exactly the SNR/RSSI
// the command exists to report ("... 209f:34fa:ed9c, SNR 4.0 ").
#define BOT_FRAG_LEN      100

// Registers the bot channel in the shared registry (ChannelCommon.h does the
// hex parse and hash derivation) and resets bot state. Called by channelsInit().
void MyMesh::botInit() {
  bot_next_reply_at = 0;
  bot_replies_sent = 0;
  bot_ignored = 0;
  bot_throttled = 0;
  memset(bot_peers, 0, sizeof(bot_peers));
  bot_last_reply_secs = 0;

  const char* psk = BOT_CHANNEL_PSK;
  bot_channel_idx = (int8_t)channelAddFromPsk(psk, strlen(psk), CHAN_FLAG_BOT);
}

// Colon-separated per-hop hashes, the same self-describing convention the
// analyzer's pktlog uses (analyzer_path_hex): the hash width is uniform per
// packet but invisible in the hex, so ':' marks where one hop ends. Width comes
// from the packet, NOT a constant -- one node on this mesh runs 3-byte hashes
// deliberately. `out` must hold BOT_PATH_STR_LEN bytes.
void MyMesh::botFormatPath(char* out, const mesh::Packet* pkt) const {
  static const char hexd[] = "0123456789abcdef";
  uint8_t cnt = pkt->getPathHashCount();
  uint8_t sz = pkt->getPathHashSize();
  if (cnt == 0 || sz == 0) { strcpy(out, "direct"); return; }

  uint8_t max_hops = (BOT_PATH_STR_LEN - 1) / (sz * 2 + 1);
  char* p = out;
  uint8_t off = 0;
  for (uint8_t h = 0; h < cnt && h < max_hops && off + sz <= MAX_PATH_SIZE; h++) {
    if (h) *p++ = ':';
    for (uint8_t b = 0; b < sz; b++, off++) {
      *p++ = hexd[pkt->path[off] >> 4];
      *p++ = hexd[pkt->path[off] & 0x0F];
    }
  }
  *p = 0;
}

// NOTE: exact-membership matching lives in chan_list_has() (ChannelCommon.h);
// it is distinct from botTriggerMatches(), which is a SUBSTRING search -- an
// ignore list or a command whitelist matching on substrings would be a trap
// ("path" enabling "!pathx", a node called "TTH" silencing "TTH-L1").

// One `!command` -> one text fragment. Returns chars written, 0 for unknown.
// The interesting ones are the analyzer's: a companion bot cannot answer these
// at all, because it has no rxlog and no heard registry.
int MyMesh::botCommandReply(char* out, int max_len, const char* cmd, const mesh::Packet* pkt) {
  // An empty BOT_COMMANDS means "all"; otherwise only what is listed.
  if (BOT_COMMANDS[0] != 0 && !chan_list_has(BOT_COMMANDS, cmd)) return 0;

  if (strcmp(cmd, "noise") == 0) {
    // Noise floor is the analyzer's own measurement -- a companion bot has no
    // way to answer this, which makes it worth owning on a shared channel.
    return snprintf(out, max_len, "noise floor %d dBm",
                    (int)(int16_t)_radio->getNoiseFloor());
  }
  if (strcmp(cmd, "ping") == 0) {
    return snprintf(out, max_len, "pong");
  }
  if (strcmp(cmd, "path") == 0) {
    char path[BOT_PATH_STR_LEN];
    botFormatPath(path, pkt);
    return snprintf(out, max_len, "path %s (%d hop)", path, (int)pkt->getPathHashCount());
  }
  if (strcmp(cmd, "link") == 0) {
    // "Jaky mam link domu?" v jednom prikazu. Zamerne slouceny path+snr: tohle
    // se pta clovek v aute, ktery nema chut psat dva prikazy -- a jde o cestu
    // JEHO paketu k NAM, cili presne ten smer, ktery si na svem uzlu overit
    // neumi (tam vidi jen to, co prijima).
    // The path goes LAST on purpose. It is the only unbounded part of this reply
    // (0 to 62 chars, and the sender picks the hash width), so it is also the only
    // part that can push the answer over BOT_FRAG_LEN. Putting it after the numbers
    // means a truncation eats hops off the tail instead of eating the SNR/RSSI --
    // and a partial path is still useful, a missing link budget is not.
    char path[BOT_PATH_STR_LEN];
    botFormatPath(path, pkt);
    return snprintf(out, max_len, "SNR %.1f dB, RSSI %d dBm, %d hop %s",
                    pkt->getSNR(), pkt->getRSSI(), (int)pkt->getPathHashCount(), path);
  }
  if (strcmp(cmd, "snr") == 0) {
    // Straight off the frame that carried the request -- the same numbers
    // captureRx() files into the rxlog ring.
    return snprintf(out, max_len, "SNR %.1f dB / RSSI %d dBm", pkt->getSNR(), pkt->getRSSI());
  }
#if MAX_HEARD_NODES
  if (strcmp(cmd, "heard") == 0) {
    int n = 0;
    for (int i = 0; i < MAX_HEARD_NODES; i++) {
      if (heard_nodes[i].heard_timestamp > 0) n++;
    }
    return snprintf(out, max_len, "heard %d nodes", n);
  }
#endif
  if (strcmp(cmd, "help") == 0) {
    return snprintf(out, max_len, "!ping !path !snr"
#if MAX_HEARD_NODES
                                  " !heard"
#endif
    );
  }
  return 0;
}

// Scan the message for `!word` tokens and join their answers with " | ", so
// "!path !snr" costs one packet instead of two. Returns total chars, 0 if the
// message held no recognised command.
int MyMesh::botScanCommands(char* out, int max_len, const char* text, const mesh::Packet* pkt) {
  int written = 0;
  int found = 0;
  out[0] = 0;

  for (const char* p = text; *p; ) {
    if (*p != '!') { p++; continue; }
    p++;
    char word[12];
    int wl = 0;
    while (*p && wl < (int)sizeof(word) - 1 && isalpha((unsigned char)*p)) {
      word[wl++] = tolower((unsigned char)*p);
      p++;
    }
    word[wl] = 0;
    if (wl == 0) continue;

    char frag[BOT_FRAG_LEN];
    if (botCommandReply(frag, sizeof(frag), word, pkt) <= 0) continue;

    int room = max_len - written;
    if (room <= 1) break;
    int adv = snprintf(out + written, room, "%s%s", found ? " | " : "", frag);
    if (adv < 0) break;
    if (adv >= room) { written = max_len - 1; found++; break; }   // truncated, stop
    written += adv;
    found++;
  }
  return found ? written : 0;
}

// Comma-separated list, case-insensitive substring match. "*" answers anything.
bool MyMesh::botTriggerMatches(const char* text) const {
  const char* seg = BOT_TRIGGER;
  if (*seg == 0) return false;
  if (seg[0] == '*' && seg[1] == 0) return true;

  while (*seg) {
    const char* comma = strchr(seg, ',');
    int len = comma ? (int)(comma - seg) : (int)strlen(seg);
    while (len > 0 && *seg == ' ') { seg++; len--; }
    while (len > 0 && seg[len - 1] == ' ') len--;

    if (len > 0) {
      for (const char* t = text; *t; t++) {
        int i = 0;
        while (i < len && t[i] && tolower((unsigned char)t[i]) == tolower((unsigned char)seg[i])) i++;
        if (i == len) return true;
      }
    }
    if (!comma) break;
    seg = comma + 1;
  }
  return false;
}

void MyMesh::botSendReply(const mesh::Packet* pkt, const char* text) {
  // Payload layout copied from BaseChatMesh::sendGroupMessage(): 4 B timestamp
  // (which is also what keeps the packet hash unique), 1 B text type, then
  // "<name>: <msg>" -- the prefix is how every client shows the sender, since a
  // channel packet carries no identity.
  uint8_t temp[5 + BOT_REPLY_LEN];
  uint32_t timestamp = getRTCClock()->getCurrentTimeUnique();
  memcpy(temp, &timestamp, 4);
  temp[4] = TXT_TYPE_PLAIN;

  int n = snprintf((char*)&temp[5], BOT_REPLY_LEN, "%s: %s", _prefs.node_name, text);
  if (n < 0) return;
  if (n > BOT_REPLY_LEN - 1) n = BOT_REPLY_LEN - 1;   // snprintf reports untruncated length

  auto reply = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, known_channels[bot_channel_idx], temp, 5 + n);
  if (reply == NULL) {
    MESH_DEBUG_PRINTLN("botSendReply: packet pool empty");
    return;
  }
  // Mirror the request's path-hash width and transport scope, exactly like every
  // other reply this firmware sends (see the sendFloodReply call sites).
  sendFloodReply(reply, SERVER_RESPONSE_DELAY, pkt->getPathHashSize());
  bot_replies_sent++;
  bot_last_reply_secs = getRTCClock()->getCurrentTime();
}

// Per-sender cooldown over a tiny LRU. Returns true and CLAIMS the window when
// this sender may be answered now.
bool MyMesh::botPeerAllowed(const char* name) {
  uint32_t h = 2166136261u;                      // FNV-1a over the lowercased name
  for (const char* p = name; *p; p++) h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
  if (h == 0) h = 1;                             // 0 marks a free slot

  int free_slot = -1, oldest = 0;
  for (int i = 0; i < BOT_PEER_SLOTS; i++) {
    if (bot_peers[i].name_hash == h) {
      if (!millisHasNowPassed(bot_peers[i].next_at)) return false;   // still cooling
      bot_peers[i].next_at = futureMillis(BOT_REPLY_COOLDOWN_MS);
      return true;
    }
    if (bot_peers[i].name_hash == 0 && free_slot < 0) free_slot = i;
    // signed compare = wraparound-safe "which window expires soonest"
    if ((long)(bot_peers[i].next_at - bot_peers[oldest].next_at) < 0) oldest = i;
  }
  int slot = (free_slot >= 0) ? free_slot : oldest;
  bot_peers[slot].name_hash = h;
  bot_peers[slot].next_at = futureMillis(BOT_REPLY_COOLDOWN_MS);
  return true;
}

// Called from onChannelText() (ChannelCommon.h) for messages on the bot
// channel only -- the "<sender>: <msg>" split already happened there, and a
// sender on the ChannelFilter deny list never reaches this point.
void MyMesh::botOnChannelText(mesh::Packet* packet, const char* sender, const char* msg) {
  // Bail out if the sender is us: our own reply comes back through the mesh and
  // would otherwise ping-pong forever. The cooldown alone would not stop that,
  // it would only slow it down.
  if (sender[0] != 0) {
    if (strcmp(sender, _prefs.node_name) == 0) return;
    // Peer bots: skip before any trigger or command matching. Measured need --
    // TTH-L1 puts "Ping" on this channel by itself, and answering those ate the
    // cooldown, so a human question landed in a closed window.
    if (BOT_IGNORE_SENDERS[0] != 0 && chan_list_has(BOT_IGNORE_SENDERS, sender)) {
      bot_ignored++;
      MESH_DEBUG_PRINTLN("bot: ignoruji odesilatele z BOT_IGNORE_SENDERS");
      return;
    }
  }

  char reply[BOT_REPLY_LEN];
  if (botScanCommands(reply, sizeof(reply), msg, packet) <= 0) {
    if (!botTriggerMatches(msg)) return;
    char path[BOT_PATH_STR_LEN];
    botFormatPath(path, packet);
    snprintf(reply, sizeof(reply), "pong (%s)", path);
  }

  // Global floor first: it is the airtime guarantee and must hold regardless of
  // how many distinct senders are asking.
  if (bot_next_reply_at != 0 && !millisHasNowPassed(bot_next_reply_at)) {
    MESH_DEBUG_PRINTLN("bot: reply suppressed, global gap");
    bot_throttled++;
    return;
  }
  // Then this sender's own window. Name-based, so spoofable -- fine, this is a
  // politeness throttle and channel messages carry no identity anyway.
  if (!botPeerAllowed(sender)) {
    MESH_DEBUG_PRINTLN("bot: reply suppressed, sender cooldown");
    bot_throttled++;
    return;
  }
  bot_next_reply_at = futureMillis(BOT_MIN_GAP_MS);
  if (bot_next_reply_at == 0) bot_next_reply_at = 1;   // 0 means "not armed"

  botSendReply(packet, reply);
}

// `bot` -- read-only diagnostics. Trigger and reply text stay build-time, so
// there is nothing here to set: this is for answering "is it actually armed,
// and has it ever spoken?" without a client on the channel.
bool MyMesh::botHandleCommand(const char* command, char* reply) {
  if (memcmp(command, "bot", 3) != 0 || (command[3] != 0 && command[3] != ' ')) return false;

  if (bot_channel_idx < 0) {
    strcpy(reply, "bot: DISABLED (bad BOT_CHANNEL_PSK)");
    return true;
  }
  sprintf(reply, "bot: chan %02X, trigger '%s', cmds '%s', ignore '%s', cooldown %d ms/odesilatel + %d ms floor, replies %u, throttled %u, ignored %u, last %u",
          (uint32_t)known_channels[bot_channel_idx].hash[0], BOT_TRIGGER,
          BOT_COMMANDS[0] ? BOT_COMMANDS : "(vse)",
          BOT_IGNORE_SENDERS[0] ? BOT_IGNORE_SENDERS : "(nikdo)",
          (int)BOT_REPLY_COOLDOWN_MS, (int)BOT_MIN_GAP_MS,
          (uint32_t)bot_replies_sent, (uint32_t)bot_throttled,
          (uint32_t)bot_ignored, (uint32_t)bot_last_reply_secs);
  return true;
}

#endif   // BOT_CHANNEL_PSK
