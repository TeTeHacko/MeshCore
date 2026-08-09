#pragma once

#include <Mesh.h>
#include <RadioLib.h>

#ifdef USE_CC310_HW_CRYPTO
#include <Adafruit_nRFCrypto.h>
#endif
struct PacketMillis {
  uint32_t preambleMillis;  // preamble-detect -> header-valid deadline
  uint32_t payloadMillis;   // header-valid   -> rx-done deadline
};

class RadioLibWrapper : public mesh::Radio {
protected:
  PhysicalLayer* _radio;
  mesh::MainBoard* _board;
  uint32_t n_recv, n_sent, n_recv_errors;
  int16_t _noise_floor, _threshold;
  bool _cad_enabled;
  uint16_t _num_floor_samples;
  int32_t _floor_sample_sum;
  uint8_t _preamble_sf;
  int16_t _last_txpow_status;   // CUSTOM (TeTeHacko): see getLastTxPowerStatus()
  int8_t  _last_txpow_dbm;

  void idle();
  void startRecv();
#ifdef LORA_POLL_IRQ
  void pollIrq();   // CUSTOM (TeTeHacko): DIO1-less completion detection, see .cpp
#endif
  float packetScoreInt(float snr, int sf, int packet_len);
  virtual bool isReceivingPacket() =0;
  virtual void doResetAGC();

public:
  RadioLibWrapper(PhysicalLayer& radio, mesh::MainBoard& board) : _radio(&radio), _board(&board), _preamble_sf(0),
      _last_txpow_status(0), _last_txpow_dbm(0) { n_recv = n_sent = 0; }

  void begin() override;
  virtual void powerOff() { _radio->sleep(); }
  int recvRaw(uint8_t* bytes, int sz) override;
  uint32_t getEstAirtimeFor(int len_bytes) override;
  bool startSendRaw(const uint8_t* bytes, int len) override;
  bool isSendComplete() override;
  void onSendFinished() override;
  bool isInRecvMode() const override;
  bool isChannelActive();

  bool isReceiving() override {
    if (isReceivingPacket()) return true;

    return isChannelActive();
  }

  virtual void setParams(float freq, float bw, uint8_t sf, uint8_t cr) = 0;
  uint32_t getRngSeed();
  void setTxPower(int8_t dbm);
  // CUSTOM (TeTeHacko): RadioLib status of the LAST setTxPower() call.
  // setTxPower() has to stay void (it implements an upstream callback), but
  // discarding the status made a failed change indistinguishable from a
  // successful one: SX1262 only accepts -9..+22 dBm and REJECTS anything else
  // without touching the PA, while `set tx` clamps to -9..30, saves the pref and
  // answers "OK". The node then reports a tx power the radio never took.
  // 0 = RADIOLIB_ERR_NONE, -13 = RADIOLIB_ERR_INVALID_OUTPUT_POWER.
  int16_t getLastTxPowerStatus() const { return _last_txpow_status; }
  int8_t  getLastTxPowerRequested() const { return _last_txpow_dbm; }

  virtual float getCurrentRSSI() =0;
  virtual uint8_t getSpreadingFactor() const { return LORA_SF; }
  static uint16_t preambleLengthForSF(uint8_t sf) { return sf <= 8 ? 32 : 16; }
  void updatePreamble(uint8_t sf) { _preamble_sf = sf; _radio->setPreambleLength(preambleLengthForSF(sf)); }
  PacketMillis calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols);
  virtual int16_t performChannelScan();

  int getNoiseFloor() const override { return _noise_floor; }
  void triggerNoiseFloorCalibrate(int threshold) override;
  void setCADEnabled(bool enable) override { _cad_enabled = enable; }
  void resetAGC() override;

  void loop() override;

  uint32_t getPacketsRecv() const { return n_recv; }
  uint32_t getPacketsRecvErrors() const { return n_recv_errors; }
  uint32_t getPacketsSent() const { return n_sent; }
  void resetStats() { n_recv = n_sent = n_recv_errors = 0; }

  virtual float getLastRSSI() const override;
  virtual float getLastSNR() const override;

  float packetScore(float snr, int packet_len) override { return packetScoreInt(snr, 10, packet_len); }  // assume sf=10

  virtual bool setRxBoostedGainMode(bool) { return false; }
  virtual bool getRxBoostedGainMode() const { return false; }

  virtual bool configSideDetectors(const uint8_t sideDetSFs[], uint8_t num, float bw) { return false; }

#if defined(LORA_POLL_IRQ) || defined(PIN_DIAG)
  // CUSTOM (TeTeHacko): chip-native IRQ status bits that mean "the operation the
  // driver is waiting for has finished" -- i.e. exactly what the DIO1 interrupt
  // would have told us. getIrqFlags() returns the RAW chip register (SX126x
  // returns its own bitmask, not RadioLib's generic RADIOLIB_IRQ_* indices), so
  // the mask has to come from the chip-specific wrapper. Returning 0 disables
  // both the polling fallback and the DIO1 probe for radios that don't override.
  virtual uint32_t irqDoneMask() const { return 0; }
#endif

#ifdef PIN_DIAG
  // CUSTOM (TeTeHacko): salvage diagnostics -- see `dio1` in simple_repeater.
  int probeDio1(uint32_t* irq_out, int* dio1_level, int* probe_level);
#endif
};

/**
 * \brief  an RNG impl using the noise from the LoRa radio as entropy.
 *         NOTE: this is VERY SLOW!  Use only for things like creating new LocalIdentity
*/
class RadioNoiseListener : public mesh::RNG {
  PhysicalLayer* _radio;
public:
  RadioNoiseListener(PhysicalLayer& radio): _radio(&radio) { }

  void random(uint8_t* dest, size_t sz) override {
#ifdef USE_CC310_HW_CRYPTO
    // CC310 TRNG is higher quality and environment-independent vs radio RSSI noise.
    nRFCrypto.begin();
    nRFCrypto.Random.generate(dest, (uint16_t)sz);
    nRFCrypto.end();
#else
    for (int i = 0; i < sz; i++) {
      dest[i] = _radio->randomByte() ^ (::random(0, 256) & 0xFF);
    }
#endif
  }
};
