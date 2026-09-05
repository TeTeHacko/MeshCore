#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <RTClib.h>
#include <target.h>

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
#elif defined(ESP32)
  #include <SPIFFS.h>
  using File = fs::File;
#endif

#ifdef WITH_RS232_BRIDGE
#include "helpers/bridges/RS232Bridge.h"
#define WITH_BRIDGE
#endif

#ifdef WITH_ESPNOW_BRIDGE
#include "helpers/bridges/ESPNowBridge.h"
#define WITH_BRIDGE
#endif

#include <helpers/AdvertDataHelpers.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/ClientACL.h>
#include <helpers/CommonCLI.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/FairnessLimiter.h>
#include <helpers/RegionMap.h>
#include <helpers/RoutingPolicy.h>
#include "RateLimiter.h"

#ifdef WITH_BRIDGE
extern AbstractBridge* bridge;
#endif

struct RepeaterStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;                // was 'n_full_events'
  int16_t  last_snr;   // x 4
  uint16_t n_direct_dups, n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
  // CUSTOM (TeTeHacko): appended at the END on purpose. Clients parse this
  // struct by fixed offsets, so fields added here are simply invisible to older
  // ones instead of shifting everything after them -- the same way upstream
  // grew its companion stats frame (docs/stats_binary_frames.md documents a
  // 26-byte "legacy" and a 30-byte frame that appends recv_errors).
  //
  // These two come from the Dispatcher requeue counters. Without them a
  // repeater that keeps failing to get its transmits out looks perfectly
  // healthy in every other field: sent/airtime just stop growing, and there is
  // nothing in the status that says why.
  uint32_t n_tx_start_fail, n_tx_timeout;
};

// This struct goes on the air byte-for-byte, so its size is part of the
// protocol: 56 bytes up to n_recv_errors (what upstream clients expect and
// gate on -- meshcore-py's parse_status() reads recv_errors only
// `if len(data) >= offset + 56`), plus the 8 appended here. Reordering a field
// or letting the compiler insert padding would silently shift every offset
// after it, so pin the number instead of trusting review.
static_assert(sizeof(RepeaterStats) == 64,
              "RepeaterStats is a wire format: append only, keep it padding-free");

// MAX_CLIENTS zrusen tady mergem origin/dev (e0031870, "centralise max clients"):
// od te doby ho definuje src/helpers/ClientACL.h, ktery se sem stejne dostane.

struct NeighbourInfo {
  mesh::Identity id;
  uint32_t advert_timestamp;
  uint32_t heard_timestamp;
  int8_t snr; // multiplied by 4, user should divide to get float value
};

// ---------------------------------------------------------------------------
// CUSTOM (TeTeHacko): passive "mesh analyzer" data. Two independent, RAM-only
// tables (both compiled out unless the build sets a non-zero size), fed from
// the normal RX path and pulled out over the text console (serial / BLE) for a
// map/bridge to consume:
//   * heard_nodes[] -- one row per DISTINCT REPEATER / ROOM SERVER whose
//     (verified) advert we heard (chat clients are intentionally skipped for
//     privacy): pubkey prefix, name, type, position, last SNR/RSSI and the path
//     the advert arrived by.  -> map dots + topology edges + link quality.
//     Exposed by the `nodes [offset]` CLI command.
//   * rxlog_ring[]  -- a circular firehose of EVERY parsed frame we hear:
//     capture seq, time, packet hash, header (route|type), SNR/RSSI and the hop
//     path.  -> live traffic / analyzer view.
//     Exposed by the `rxlog [cursor]` CLI command.
//   * pktfeed_ring[] -- like rxlog but with the FULL RAW FRAME BYTES, for the
//     community observer feed (meshcoretomqtt-compatible: the bridge derives
//     type/route/payload and the SHA256 packet hash from the raw bytes).
//     Smaller ring -- raw frames are fat (~268 B/record).
//     Exposed by the `pktlog [cursor]` CLI command.
// Enable per-variant, e.g.:  -D MAX_HEARD_NODES=48 -D RXLOG_SIZE=96 -D PKTFEED_SIZE=32
// ---------------------------------------------------------------------------
#ifndef MAX_HEARD_NODES
  #define MAX_HEARD_NODES 0
#endif
#ifndef RXLOG_SIZE
  #define RXLOG_SIZE 0
#endif
#ifndef PKTFEED_SIZE
  #define PKTFEED_SIZE 0
#endif
#ifndef HEARD_NODE_NAME_LEN
  #define HEARD_NODE_NAME_LEN 24
#endif
#ifndef HEARD_NODE_PREFIX
  #define HEARD_NODE_PREFIX 6      // bytes of pub_key kept (resolve path hashes + registry cross-ref)
#endif
#ifndef ANALYZER_PATH_LEN
  #define ANALYZER_PATH_LEN 16     // max hop-hash bytes kept per record
#endif
#ifndef HEARD_DIRECT_TTL_SECS
  #define HEARD_DIRECT_TTL_SECS 3600  // keep showing a node as direct (0-hop) this long after last hearing it direct
#endif

#if MAX_HEARD_NODES
struct HeardNode {
  uint8_t  pub_prefix[HEARD_NODE_PREFIX];
  char     name[HEARD_NODE_NAME_LEN];
  int32_t  lat, lon;               // degrees x 1e6, 0 = unknown
  uint32_t heard_timestamp;        // epoch when last heard (0 = empty slot)
  uint32_t advert_timestamp;       // advert's own timestamp (newest wins)
  uint32_t pkt_hash;               // FNV of the latest advert (type||payload) -- JOIN KEY:
                                   // group rxlog records by this to get every path the
                                   // advert arrived by (rxlog logs all copies pre-dedup)
  uint32_t direct_heard;           // epoch we last heard this node 0-hop (0 = never)
  int8_t   snr;                    // x4
  int8_t   rssi;                   // dBm
  uint8_t  type;                   // ADV_TYPE_*
  uint8_t  path_len;               // bit-packed (hash size|count); representative/best path
  uint8_t  path[ANALYZER_PATH_LEN];
};
#endif

#if RXLOG_SIZE
struct RxLogRec {
  uint32_t seq;                    // monotonic capture sequence (0 = empty slot; also the paging cursor)
  uint32_t when;                   // epoch
  uint32_t pkt_hash;               // FNV-1a of (type||payload) -- cheap dedup / cross-observer id (NOT the SHA packet-hash)
  int8_t   snr;                    // x4
  int8_t   rssi;                   // dBm
  uint8_t  header;                 // raw header byte (route|type|ver)
  uint8_t  path_len;               // bit-packed (hash size|count)
  uint8_t  path[ANALYZER_PATH_LEN];
};
#endif

#if PKTFEED_SIZE
struct PktFeedRec {
  uint32_t seq;                    // monotonic capture sequence (0 = empty slot; also the paging cursor)
  uint32_t when;                   // epoch
  int8_t   snr;                    // x4
  int8_t   rssi;                   // dBm
  uint8_t  len;                    // raw frame length in bytes
  uint8_t  raw[MAX_TRANS_UNIT];    // full wire frame (header|[transport]|path_len|path|payload)
};
#endif

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "14 Aug 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.17.1"
#endif

#define FIRMWARE_ROLE "repeater"

#define PACKET_LOG_FILE  "/packet_log"

class MyMesh : public mesh::Mesh, public CommonCLICallbacks {
  FILESYSTEM* _fs;
  uint32_t last_millis;
  uint64_t uptime_millis;
  unsigned long next_local_advert, next_flood_advert;
  bool _logging;
  NodePrefs _prefs;
  ClientACL  acl;
  CommonCLI _cli;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  uint8_t reply_path[MAX_PATH_SIZE];
  uint8_t reply_path_len;
  TransportKeyStore key_store;
  RegionMap region_map, temp_map;
  RegionEntry* load_stack[8];
  RegionEntry* recv_pkt_region;
  TransportKey default_scope;
  RateLimiter discover_limiter, anon_limiter;
  // CUSTOM (TeTeHacko): token-bucket limiter over FORWARDED traffic (port of
  // upstream proposal #1502). Kill switch: _prefs.fairness_enabled.
  FairnessLimiter fairness_limiter;
  unsigned long next_fair_sender_normal_refill, next_fair_sender_low_refill, next_fair_group_refill;
  uint32_t pending_discover_tag;
  unsigned long pending_discover_until;
  bool region_load_active;
  unsigned long dirty_contacts_expiry;
#if MAX_NEIGHBOURS
  NeighbourInfo neighbours[MAX_NEIGHBOURS];
#endif
#if MAX_HEARD_NODES
  HeardNode heard_nodes[MAX_HEARD_NODES];
#endif
#if RXLOG_SIZE
  RxLogRec rxlog_ring[RXLOG_SIZE];
  uint16_t rxlog_head;      // next slot to write
  uint32_t rxlog_next_seq;  // next capture sequence to assign (starts at 1)
#endif
#if PKTFEED_SIZE
  PktFeedRec pktfeed_ring[PKTFEED_SIZE];
  uint16_t pktfeed_head;      // next slot to write
  uint32_t pktfeed_next_seq;  // next capture sequence to assign (starts at 1)
  // raw bytes staged in logRxRaw (pre-parse, the only place they exist) and
  // committed to the ring in logRx (fires only for frames that parsed OK, so
  // radio garbage never enters the feed)
  uint8_t pktfeed_stage[MAX_TRANS_UNIT];
  uint8_t pktfeed_stage_len;  // 0 = nothing staged
#endif
#if defined(BOT_CHANNEL_PSK) || defined(FILTER_CHANNEL_PSKS)
  // CUSTOM (TeTeHacko): registry of channels this node can DECRYPT (the bot
  // channel and/or the filter channels). Shared by ChannelBot.h and
  // ChannelFilter.h; fed to searchChannelsByHash(), whose API caps matches at 4.
  #define MAX_KNOWN_CHANNELS 4
  #define CHAN_FLAG_BOT     0x01   // ChannelBot answers on this channel
  #define CHAN_FLAG_FILTER  0x02   // deny list applies to this channel
  mesh::GroupChannel known_channels[MAX_KNOWN_CHANNELS];
  uint8_t known_channel_flags[MAX_KNOWN_CHANNELS];
  uint8_t num_known_channels;
#endif
#ifdef FILTER_CHANNEL_PSKS
  uint32_t filter_dropped;        // forwards vetoed by the deny list
#endif
#ifdef BOT_CHANNEL_PSK
  // Default zde, ne az v ChannelBot.h -- ten se includuje na konci MyMesh.cpp,
  // tedy dlouho po teto deklaraci pole.
  #ifndef BOT_PEER_SLOTS
    #define BOT_PEER_SLOTS 8
  #endif
  int8_t bot_channel_idx;           // index into known_channels, -1 = PSK malformed, bot silently off
  unsigned long bot_next_reply_at;  // cooldown gate, 0 = not armed
  uint32_t bot_replies_sent;
  uint32_t bot_ignored;           // zprav zahozenych podle BOT_IGNORE_SENDERS
  uint32_t bot_throttled;         // odpovedi potlacenych cooldownem
  struct BotPeer { uint32_t name_hash; unsigned long next_at; };
  BotPeer bot_peers[BOT_PEER_SLOTS];   // per-odesilatel okno, LRU
  uint32_t bot_last_reply_secs;     // RTC time of the last reply, 0 = never
#endif
  CayenneLPP telemetry;
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;
  int  matching_peer_indexes[MAX_CLIENTS];
#if defined(WITH_RS232_BRIDGE)
  RS232Bridge bridge;
#elif defined(WITH_ESPNOW_BRIDGE)
  ESPNowBridge bridge;
#endif

  void putNeighbour(const mesh::Identity& id, uint32_t timestamp, float snr);
#if MAX_HEARD_NODES
  void putHeardNode(const mesh::Identity& id, uint32_t advert_timestamp, uint8_t type,
                    int32_t lat, int32_t lon, const char* name,
                    int8_t snr, int8_t rssi, const mesh::Packet* pkt);
  void formatNodesReply(char* reply, int max_len, uint16_t offset);
#endif
#if RXLOG_SIZE
  void captureRx(const mesh::Packet* pkt);
  void formatRxLogReply(char* reply, int max_len, uint32_t cursor);
#endif
#if PKTFEED_SIZE
  void stagePktFeed(const uint8_t raw[], int len);
  void commitPktFeed(const mesh::Packet* pkt);
  void formatPktFeedReply(char* reply, int max_len, uint32_t cursor);
#endif
#if defined(BOT_CHANNEL_PSK) || defined(FILTER_CHANNEL_PSKS)
  // CUSTOM (TeTeHacko): shared channel plumbing. Definitions in ChannelCommon.h,
  // included at the bottom of MyMesh.cpp.
  void channelsInit();
  int  channelAddFromPsk(const char* psk_hex, int psk_len, uint8_t flag);
  void onChannelText(mesh::Packet* packet, int chan_idx, const char* sender, const char* msg);
#endif
#ifdef FILTER_CHANNEL_PSKS
  // CUSTOM (TeTeHacko): deny-list forwarding filter. Definitions in ChannelFilter.h.
  bool filterDenied(const char* sender) const;
  bool filterHandleCommand(const char* command, char* reply, int reply_max);
#endif
#ifdef BOT_CHANNEL_PSK
  // CUSTOM (TeTeHacko): channel bot. Definitions in ChannelBot.h, included at
  // the bottom of MyMesh.cpp. See that file for why this lives in the analyzer
  // build rather than in companion_radio.
  void botInit();
  void botOnChannelText(mesh::Packet* packet, const char* sender, const char* msg);
  bool botTriggerMatches(const char* text) const;
  void botFormatPath(char* out, const mesh::Packet* pkt) const;
  int  botCommandReply(char* out, int max_len, const char* cmd, const mesh::Packet* pkt);
  int  botScanCommands(char* out, int max_len, const char* text, const mesh::Packet* pkt);
  void botSendReply(const mesh::Packet* pkt, const char* text);
  bool botPeerAllowed(const char* name);
  bool botHandleCommand(const char* command, char* reply);
#endif
  // CUSTOM (TeTeHacko): `fairness [on|off]` CLI. Defined in MyMesh.cpp.
  bool fairnessHandleCommand(const char* command, char* reply);
  uint8_t handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood);
  uint8_t handleAnonRegionsReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonOwnerReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  uint8_t handleAnonClockReq(const mesh::Identity& sender, uint32_t sender_timestamp, const uint8_t* data);
  int handleRequest(ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);
  mesh::Packet* createSelfAdvert();

  File openAppend(const char* fname);
  bool isLooped(const mesh::Packet* packet, const uint8_t max_counters[]);

protected:
  float getAirtimeBudgetFactor() const override {
    return _prefs.airtime_factor;
  }

  bool allowPacketForward(const mesh::Packet* packet) override;

  // Šířku path hashe volí ODESÍLATEL, takže musí přijít z prefs tohohle uzlu.
  // Díky tomuhle overridu ji dostanou i volání send*(), která ji nepředávají
  // explicitně (ACKy, PATH-return, zerohop odpovědi sousedovi).
  uint8_t getSelfPathHashSize() const override { return _prefs.path_hash_mode + 1; }
#if defined(BOT_CHANNEL_PSK) || defined(FILTER_CHANNEL_PSKS)
  // CUSTOM (TeTeHacko): both are no-op virtuals in mesh::Mesh, which is the only
  // reason a repeater is normally deaf to channel traffic. Overriding them does
  // NOT affect forwarding by itself -- Mesh::onRecvPacket() calls routeRecvPacket()
  // either way (Mesh.cpp:237-259). The ChannelFilter DOES veto forwarding of
  // matched packets, via packet->markDoNotRetransmit() (same mechanism the ANON
  // branch uses at Mesh.cpp:229).
  int  searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel,
                       uint8_t* data, size_t len) override;
#endif
  // CUSTOM (TeTeHacko): FairnessLimiter veto. mesh::Mesh consults this as the
  // LAST condition of every forward decision, so a token is only taken for a
  // packet that would otherwise transmit. Runtime kill switch in prefs.
  bool takeForwardingRateLimit(const mesh::Packet* packet) override {
    return !_prefs.fairness_enabled || fairness_limiter.allowPacket(packet);
  }
  const char* getLogDateTime() override;
  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;

  void logRx(mesh::Packet* pkt, int len, float score) override;
  void logTx(mesh::Packet* pkt, int len) override;
  void logTxFail(mesh::Packet* pkt, int len) override;
  int calcRxDelay(float score, uint32_t air_time) const override;

  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;

  int getInterferenceThreshold() const override {
    return _prefs.interference_threshold;
  }
  bool getCADEnabled() const override {
    return _prefs.cad_enabled;
  }
  int getAGCResetInterval() const override {
    return ((int)_prefs.agc_reset_interval) * 4000;   // milliseconds
  }
  uint8_t getExtraAckTransmitCount() const override {
    return _prefs.multi_acks;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    // 0 off / 1 on / 2 duty cycle -- pass the mode through, not a boolean, or
    // a duty-cycled node comes back from a reboot with GPS powered forever.
    const char* mode = _prefs.gps_enabled == 2 ? "2" : (_prefs.gps_enabled ? "1" : "0");
    sensors.setSettingValue("gps", mode);
  }
#endif

  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override;

  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len);
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;

  void sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size);

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);
  void sendNodeDiscoverReq();
  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  NodePrefs* getNodePrefs() {
    return &_prefs;
  }

  void savePrefs() override {
    _cli.savePrefs(_fs);
  }

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size);

  // CommonCLICallbacks
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;

  void setLoggingOn(bool enable) override { _logging = enable; }

  void eraseLogFile() override {
    _fs->remove(PACKET_LOG_FILE);
  }

  void dumpLogFile() override;
  void setTxPower(int8_t power_dbm) override;
  void formatNeighborsReply(char *reply) override;
  void removeNeighbor(const uint8_t* pubkey, int key_len) override;
  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;
  void startRegionsLoad() override;
  bool saveRegions() override;
  void onDefaultRegionChanged(const RegionEntry* r) override;

  mesh::LocalIdentity& getSelfId() override { return self_id; }

  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;

  // reply_max = usable reply capacity. Default 150 keeps the mesh admin CLI reply
  // inside one LoRa packet; the local console (USB/BLE, big buffer) passes more so
  // the paged analyzer replies (nodes/rxlog) can use big pages. Explicit parameter
  // on purpose — a forged sender_timestamp must never be able to grow the reply.
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply, int reply_max = 150);
  void loop();

#if defined(WITH_BRIDGE)
  void setBridgeState(bool enable) override {
    if (enable == bridge.isRunning()) return;
    if (enable)
    {
      bridge.begin();
    }
    else 
    {
      bridge.end();
    }
  }

  void restartBridge() override {
    if (!bridge.isRunning()) return;
    bridge.end();
    bridge.begin();
  }
#endif

  // To check if there is pending work
  bool hasPendingWork() const;

  bool setRxBoostedGain(bool enable) override;

  #if defined(USE_LR2021)
  virtual bool configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) override;
  #endif

};
