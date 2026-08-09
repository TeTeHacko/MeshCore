
#define RADIOLIB_STATIC_ONLY 1
#include "RadioLibWrappers.h"

#define STATE_IDLE       0
#define STATE_RX         1
#define STATE_TX_WAIT    3
#define STATE_TX_DONE    4
#define STATE_INT_READY 16

#define NUM_NOISE_FLOOR_SAMPLES  64
#define SAMPLING_THRESHOLD  14

static volatile uint8_t state = STATE_IDLE;

// this function is called when a complete packet
// is transmitted by the module
static 
#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void setFlag(void) {
  // we sent a packet, set the flag
  state |= STATE_INT_READY;
}

void RadioLibWrapper::begin() {
  _radio->setPacketReceivedAction(setFlag);  // this is also SentComplete interrupt
  _preamble_sf = getSpreadingFactor();
  _radio->setPreambleLength(preambleLengthForSF(_preamble_sf)); // longer preamble for lower SF improves reliability
  state = STATE_IDLE;

  if (_board->getStartupReason() == BD_STARTUP_RX_PACKET) {  // received a LoRa packet (while in deep sleep)
    setFlag(); // LoRa packet is already received
  }

  _noise_floor = 0;
  _threshold = 0;
  _cad_enabled = false;

  // start average out some samples
  _num_floor_samples = 0;
  _floor_sample_sum = 0;
}

uint32_t RadioLibWrapper::getRngSeed() {
  return _radio->random(0x7FFFFFFF);
}

void RadioLibWrapper::setTxPower(int8_t dbm) {
  // CUSTOM (TeTeHacko): keep the status. The signature is an upstream callback
  // and has to stay void, but the return value must not be thrown away: SX1262
  // accepts only -9..+22 dBm and rejects the rest WITHOUT touching the PA, while
  // `set tx` clamps to -9..30, saves the pref and replies "OK". Without this,
  // "the radio refused" and "the radio changed" look identical from the console.
  _last_txpow_dbm = dbm;
  _last_txpow_status = _radio->setOutputPower(dbm);
}

void RadioLibWrapper::idle() {
  _radio->standby();
  state = STATE_IDLE;   // need another startReceive()
}

void RadioLibWrapper::triggerNoiseFloorCalibrate(int threshold) {
  _threshold = threshold;
  if (_num_floor_samples >= NUM_NOISE_FLOOR_SAMPLES) {  // ignore trigger if currently sampling
    _num_floor_samples = 0;
    _floor_sample_sum = 0;
  }
}

void RadioLibWrapper::doResetAGC() {
  _radio->sleep();  // warm sleep to reset analog frontend
}

void RadioLibWrapper::resetAGC() {
  // make sure we're not mid-receive of packet!
  if ((state & STATE_INT_READY) != 0 || isReceivingPacket()) return;

  doResetAGC();
  state = STATE_IDLE;   // trigger a startReceive()

  // Reset noise floor sampling so it reconverges from scratch.
  // Without this, a stuck _noise_floor of -120 makes the sampling threshold
  // too low (-106) to accept normal samples (~-105), self-reinforcing the
  // stuck value even after the receiver has recovered.
  _noise_floor = 0;
  _num_floor_samples = 0;
  _floor_sample_sum = 0;
}

#ifdef LORA_POLL_IRQ
// CUSTOM (TeTeHacko): salvage path for a node whose DIO1 interrupt line is dead.
//
// setPacketReceivedAction() above is the ONLY thing that ever sets
// STATE_INT_READY, so a broken DIO1 line means RxDone/TxDone never reach the
// MCU: TX times out and retries while the frames actually radiate (a witness
// node hears them), and received packets are never picked up. That is the exact
// signature that killed tth-x0 -- see nrf52-radio-fault-diagnosis.
//
// The chip latches those events in its IRQ status register regardless of whether
// they are mapped out to a DIO pin, so reading it over SPI bypasses the whole
// line -- and therefore works no matter WHICH end of it is broken (dead nRF52
// input, dead SX1262 output, or a cut trace). Rewiring DIO1 to a spare GPIO only
// helps in the first of those three cases, so this is the strictly stronger fix:
// if polling does not revive the node, no wire can either.
//
// Cost: one extra SPI read per Dispatcher pass (Dispatcher.cpp:72 -> loop()),
// alongside the isReceivingPacket() read that already happens in the same path,
// and completion is noticed on the next pass instead of instantly.
void RadioLibWrapper::pollIrq() {
  if (state != STATE_RX && state != STATE_TX_WAIT) return;  // nothing pending
  uint32_t mask = irqDoneMask();
  if (mask == 0) return;                                    // radio has no mask
  if (_radio->getIrqFlags() & mask) setFlag();
}
#endif

void RadioLibWrapper::loop() {
#ifdef LORA_POLL_IRQ
  pollIrq();
#endif

  if (state == STATE_RX && _num_floor_samples < NUM_NOISE_FLOOR_SAMPLES) {
    if (!isReceivingPacket()) {
      int rssi = getCurrentRSSI();
      if (rssi < _noise_floor + SAMPLING_THRESHOLD) {  // only consider samples below current floor + sampling THRESHOLD
        _num_floor_samples++;
        _floor_sample_sum += rssi;
      }
    }
  } else if (_num_floor_samples >= NUM_NOISE_FLOOR_SAMPLES && _floor_sample_sum != 0) {
    _noise_floor = _floor_sample_sum / NUM_NOISE_FLOOR_SAMPLES;
    if (_noise_floor < -120) {
      _noise_floor = -120;    // clamp to lower bound of -120dBi
    }
    _floor_sample_sum = 0;

    MESH_DEBUG_PRINTLN("RadioLibWrapper: noise_floor = %d", (int)_noise_floor);
  }
}

void RadioLibWrapper::startRecv() {
  #if defined(USE_LR2021)
  _radio->standby(); // without this LR2021 can throw -706 when calling startReceive after hardware CAD when side detectors are enabled
  #endif
  int err = _radio->startReceive();
  if (err == RADIOLIB_ERR_NONE) {
    state = STATE_RX;
  } else {
    MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startReceive(%d)", err);
  }
}

bool RadioLibWrapper::isInRecvMode() const {
  return (state & ~STATE_INT_READY) == STATE_RX;
}

int RadioLibWrapper::recvRaw(uint8_t* bytes, int sz) {
  int len = 0;
  if (state & STATE_INT_READY) {
    len = _radio->getPacketLength();
    if (len > 0) {
      if (len > sz) { len = sz; }
      int err = _radio->readData(bytes, len);
      if (err != RADIOLIB_ERR_NONE) {
        MESH_DEBUG_PRINTLN("RadioLibWrapper: error: readData(%d)", err);
        len = 0;
        n_recv_errors++;
      } else {
      //  Serial.print("  readData() -> "); Serial.println(len);
        n_recv++;
      }
    }
    #if defined(USE_LR2021)
    state = STATE_RX;     // LR2021 stays in Rx after readData, calling startReceive while still in Rx throws -706 errors
    #else
    state = STATE_IDLE;   // need another startReceive()
    #endif
  }

  if (state != STATE_RX) {
    int err = _radio->startReceive();
    if (err == RADIOLIB_ERR_NONE) {
      state = STATE_RX;
    } else {
      MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startReceive(%d)", err);
    }
  }
  return len;
}

uint32_t RadioLibWrapper::getEstAirtimeFor(int len_bytes) {
  return _radio->getTimeOnAir(len_bytes) / 1000;
}

bool RadioLibWrapper::startSendRaw(const uint8_t* bytes, int len) {
  _board->onBeforeTransmit();
  int err = _radio->startTransmit((uint8_t *) bytes, len);
  if (err == RADIOLIB_ERR_NONE) {
    state = STATE_TX_WAIT;
    return true;
  }
  MESH_DEBUG_PRINTLN("RadioLibWrapper: error: startTransmit(%d)", err);
  idle();   // trigger another startRecv()
  _board->onAfterTransmit();
  return false;
}

bool RadioLibWrapper::isSendComplete() {
  if (state & STATE_INT_READY) {
    state = STATE_IDLE;
    n_sent++;
    return true;
  }
  return false;
}

void RadioLibWrapper::onSendFinished() {
  _radio->finishTransmit();
  _board->onAfterTransmit();
  state = STATE_IDLE;
}

int16_t RadioLibWrapper::performChannelScan() {
  return _radio->scanChannel();
}

#ifdef PIN_DIAG
// CUSTOM (TeTeHacko): make the radio itself assert DIO1, then report the chip's
// IRQ status register (read over SPI, independent of the pin) NEXT TO the actual
// level on the pin. That pair is what a multimeter would otherwise have to tell
// us, and it says which END of a dead DIO1 line is broken:
//
//   irq has TX_DONE, pin HIGH  -> the line is fine, look elsewhere
//   irq has TX_DONE, pin LOW   -> line dead. If a probe wire is fitted from the
//                                 DIO1 net to PIN_DIAG_PROBE and THAT reads HIGH,
//                                 the SX1262 still drives and it is the nRF52's
//                                 input pad that died -- the one case a rewire
//                                 to a spare GPIO actually fixes.
//   no TX_DONE at all          -> the chip never finished the transmit; the
//                                 fault is not the IRQ line (SPI/BUSY/PA).
//
// A 1-byte transmit is used as the stimulus because TX_DONE is unconditional --
// it needs no peer, no antenna match and no RX activity. It DOES put a frame on
// the air, so run it on the bench band.
int RadioLibWrapper::probeDio1(uint32_t* irq_out, int* dio1_level, int* probe_level) {
  if (state == STATE_TX_WAIT) return -1;   // mid-transmit, don't clobber it

  uint32_t mask = irqDoneMask();
  if (mask == 0) return -3;                // radio has no chip mask -> can't tell

  uint8_t dummy = 0;
  _radio->standby();
  _board->onBeforeTransmit();
  int err = _radio->startTransmit(&dummy, 1);
  if (err != RADIOLIB_ERR_NONE) {
    _board->onAfterTransmit();
    idle();
    return err;
  }

  uint32_t irq = 0;
  unsigned long deadline = millis() + 2000;
  while (millis() < deadline) {
    irq = _radio->getIrqFlags();
    if (irq & mask) break;
  }

  // Levels have to be sampled while the IRQ is still asserted: DIO1 stays high
  // until the status register is cleared, and finishTransmit() clears it.
  // Runtime compare, not #if: P_LORA_DIO_1 is usually a variant constant like D1
  // (a static const, invisible to the preprocessor, which would silently read 0).
  uint32_t dio1_pin = (uint32_t) P_LORA_DIO_1;
  *dio1_level = (dio1_pin == RADIOLIB_NC) ? -1 : digitalRead(dio1_pin);
#ifdef PIN_DIAG_PROBE
  pinMode(PIN_DIAG_PROBE, INPUT);
  *probe_level = digitalRead(PIN_DIAG_PROBE);
#else
  *probe_level = -1;
#endif
  *irq_out = irq;

  _radio->finishTransmit();
  _board->onAfterTransmit();
  state = STATE_IDLE;        // Dispatcher will startReceive() again
  return (irq & mask) ? 0 : -2;
}
#endif

bool RadioLibWrapper::isChannelActive() {
  // int.thresh: RSSI-based interference detection (relative to noise floor)
  if (_threshold != 0 && getCurrentRSSI() > _noise_floor + _threshold) return true;

  // cad: hardware channel activity detection
  if (_cad_enabled) {
    int16_t result = performChannelScan();
    // scanChannel() triggers DIO interrupt (CAD done) which sets STATE_INT_READY
    // via setFlag() ISR. Clear it before restarting RX so recvRaw() doesn't
    // try to read a non-existent packet and count a spurious recv error.
    state = STATE_IDLE;
    startRecv();
    if (result != RADIOLIB_CHANNEL_FREE) return true;
  }

  return false;
}

float RadioLibWrapper::getLastRSSI() const {
  return _radio->getRSSI();
}
float RadioLibWrapper::getLastSNR() const {
  return _radio->getSNR();
}

// Approximate SNR threshold per SF for successful reception (based on Semtech datasheets)
static float snr_threshold[] = {
    -7.5,  // SF7 needs at least -7.5 dB SNR
    -10,   // SF8 needs at least -10 dB SNR
    -12.5, // SF9 needs at least -12.5 dB SNR
    -15,  // SF10 needs at least -15 dB SNR
    -17.5,// SF11 needs at least -17.5 dB SNR
    -20   // SF12 needs at least -20 dB SNR
};
  
float RadioLibWrapper::packetScoreInt(float snr, int sf, int packet_len) {
  if (sf < 7) return 0.0f;
  
  if (snr < snr_threshold[sf - 7]) return 0.0f;    // Below threshold, no chance of success

  auto success_rate_based_on_snr = (snr - snr_threshold[sf - 7]) / 10.0;
  auto collision_penalty = 1 - (packet_len / 256.0);   // Assuming max packet of 256 bytes

  return max(0.0, min(1.0, success_rate_based_on_snr * collision_penalty));
}

PacketMillis RadioLibWrapper::calcMaxPacketMillis(uint8_t sf, float bw, uint8_t cr, uint8_t preambleSymbols) {
  // based on RadioLib's calculateTimeOnAir()
  uint32_t tsym_us = ((uint32_t)10000 << sf) / (bw * 10);
  uint32_t sfCoeff1_x4 = (sf == 5 || sf == 6) ? 25 : 17; // 6.25 : 4.25, semtech magic numbers to account for sync word + sfd

  // preamble + syncword + sfd + header
  uint32_t preamble_us = (((preambleSymbols + 8) * 4 + sfCoeff1_x4) * tsym_us) / 4;
  
  // airtime for max packet at current radio settings
  uint32_t total_us   = _radio->getTimeOnAir(MAX_TRANS_UNIT);
  // airtime for payload only (no preamble, header or SOF)
  uint32_t payload_us = total_us > preamble_us ? total_us - preamble_us : 4000 - preamble_us; // fallback to 4 secs at worst case
  // rescale payload_us for max possible CR
  if (cr >= 5 && cr < 8) { payload_us = (payload_us * 8) / cr; }

  return PacketMillis {(preamble_us + 999) / 1000, (payload_us + 999) / 1000};
}