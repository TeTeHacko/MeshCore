#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

// ===========================================================================
// CUSTOM (TeTeHacko): `blink [n]` -- make THIS board flash its LED on demand,
// so a bench full of identical boards can be told apart and labelled.
//
// The problem it solves: nothing else identifies a board in situ. The USB
// serial number is only readable over a data cable (and boards sharing power
// rails have none), the BLE MAC is not printed anywhere, and ADVERT_NAME is
// only as trustworthy as the last flash -- which is exactly what you are trying
// to confirm. Two boards here were both advertising "tth-x1-rpt".
//
// It deliberately does NOT use the status LED. On XIAO that is LED_BLUE
// (PIN_STATUS_LED), which Bluefruit drives as its connection indicator, so
// every BLE node blinks blue identically and a blink there would be lost in the
// noise. LED_GREEN is untouched by the library, so a green burst is unambiguous.
//
// Ticked from loop() rather than looping inline: a blocking blink would stall
// the mesh for the whole burst (up to ~7 s at the 30-blink cap) and disturb the
// LoRa RX/TX timing the test fleet exists to measure.
// ===========================================================================
// Pick an LED nothing else drives, in that order of preference:
//   LED_GREEN  -- XIAO (13). Free; LED_BLUE there is Bluefruit's connection LED.
//   LED_WHITE  -- SenseCap Solar (11). Only ever set LOW at boot and at
//                 powerOff(), so a burst on it is unambiguous.
//   LED_BUILTIN -- last resort. On SenseCap this is pin 12 = LED_BLUE = the LoRa
//                 TX indicator, so a blink there competes with live traffic;
//                 workable, but that is why it is last.
// A variant may DEFINE a colour it does not physically have by pointing it at
// PINS_COUNT (SenseCap does exactly that with LED_RED), so #ifdef alone is not
// enough -- the range check below is what actually proves the pin exists.
#ifndef RPT_IDENT_LED_PIN
  #if defined(LED_GREEN) && (LED_GREEN < PINS_COUNT)
    #define RPT_IDENT_LED_PIN   LED_GREEN
  #elif defined(LED_WHITE) && (LED_WHITE < PINS_COUNT)
    #define RPT_IDENT_LED_PIN   LED_WHITE
  #elif defined(LED_BUILTIN) && (LED_BUILTIN < PINS_COUNT)
    #define RPT_IDENT_LED_PIN   LED_BUILTIN
  #endif
#endif
#ifndef RPT_IDENT_LED_MS
  #define RPT_IDENT_LED_MS      120    // half-period; 120 ms reads as a clear flash
#endif

// `blink on` keeps going until `blink off`, because a fixed burst is a race
// against how long it takes to walk to the bench -- the first version capped at
// 30 blinks (~7 s) and that was not enough to find the board. Held blinking
// still expires on its own: a node left blinking would quietly eat a battery,
// and on a mounted node nobody would notice.
#ifndef RPT_IDENT_HOLD_MS
  #define RPT_IDENT_HOLD_MS   (10UL * 60UL * 1000UL)   // 10 min
#endif

#ifdef RPT_IDENT_LED_PIN
static uint16_t      ident_edges_left = 0;   // remaining on/off transitions
static bool          ident_led_on     = false;
static unsigned long ident_next_ms    = 0;
static bool          ident_pin_ready  = false;
static bool          ident_hold       = false;   // blink until told to stop
static unsigned long ident_hold_until = 0;

// LED_STATE_ON is 0 on XIAO (active low) and 1 elsewhere; go through it rather
// than hard-coding HIGH/LOW, or the "blink" is an unlit board on half the fleet.
static inline void identLedWrite(bool on) {
  digitalWrite(RPT_IDENT_LED_PIN, on ? LED_STATE_ON : !LED_STATE_ON);
}

static void identBegin() {
  if (!ident_pin_ready) {
    pinMode(RPT_IDENT_LED_PIN, OUTPUT);
    ident_pin_ready = true;
  }
  ident_led_on = true;
  identLedWrite(true);
  ident_next_ms = millis() + RPT_IDENT_LED_MS;
}

static void identStart(int blinks) {
  if (blinks < 1)   blinks = 1;
  if (blinks > 999) blinks = 999;
  ident_hold = false;
  ident_edges_left = (uint16_t)(blinks * 2);   // one on + one off per blink
  identBegin();
}

static void identHold(bool on) {
  ident_hold = on;
  ident_edges_left = 0;
  if (on) {
    ident_hold_until = millis() + RPT_IDENT_HOLD_MS;
    identBegin();
  } else if (ident_pin_ready) {
    identLedWrite(false);
  }
}

static void identStop() { identHold(false); }

static void identTick() {
  if (ident_hold) {
    if ((long)(millis() - ident_hold_until) >= 0) { identStop(); return; }
  } else if (ident_edges_left == 0) {
    return;
  }
  if ((long)(millis() - ident_next_ms) < 0) return;
  if (!ident_hold) {
    ident_edges_left--;
    if (ident_edges_left == 0) {
      identLedWrite(false);        // always finish dark, never leave it lit
      return;
    }
  }
  ident_led_on = !ident_led_on;
  identLedWrite(ident_led_on);
  ident_next_ms = millis() + RPT_IDENT_LED_MS;
}
#endif  // RPT_IDENT_LED_PIN

// CUSTOM (TeTeHacko): `dfu [uf2|serial|ota]` -- reboot straight into the
// bootloader, deterministically.
//
// This is what the 1200-baud touch was a bad approximation of. The Adafruit
// bootloader picks its mode from GPREGRET at boot, and the core exposes exactly
// that (cores/nRF5/wiring.c): reset_mcu() writes the magic and calls
// NVIC_SystemReset().
//   uf2    0x57  mass-storage drive, drop a .uf2 on it        <- the usual one
//   serial 0x4e  CDC only, for adafruit-nrfutil; no drive appears
//   ota    0xA8  BLE DFU
// The touch only ever requested the *serial* mode, and on this build it worked
// about half the time: twice it wedged a board out of USB and BLE entirely, with
// no self-recovery (400 s) and no way back but a physical replug. This has no
// such failure mode -- it is the same call the core makes, from a command.
//
// LOCAL ONLY. Over the mesh this would strand a node in a bootloader that waits
// forever, on a mast, with no way back but a visit. sender_timestamp == 0 is the
// same "physically present" test CommonCLI uses for `erase` and `set freq`.
#if defined(NRF52_PLATFORM)
extern "C" {
  void enterUf2Dfu(void);
  void enterSerialDfu(void);
  void enterOTADfu(void);
}
static unsigned long dfu_pending_at = 0;   // 0 = nothing armed
static uint8_t       dfu_pending_mode = 0; // 0=uf2 1=serial 2=ota

// Deferred so the reply reaches the console first -- reset_mcu() never returns,
// and over BLE the notification still has to be pumped out of the TX ring.
static void dfuTick() {
  if (dfu_pending_at == 0 || (long)(millis() - dfu_pending_at) < 0) return;
  dfu_pending_at = 0;
  if (dfu_pending_mode == 1)      enterSerialDfu();
  else if (dfu_pending_mode == 2) enterOTADfu();
  else                            enterUf2Dfu();
}
#endif

bool dfuHandleCommand(uint32_t sender_timestamp, const char* command, char* reply) {
  if (memcmp(command, "dfu", 3) != 0 || (command[3] != 0 && command[3] != ' ')) return false;
#if defined(NRF52_PLATFORM)
  if (sender_timestamp != 0) {
    strcpy(reply, "ERR: dfu is local-only (would strand the node in the bootloader)");
    return true;
  }
  const char* arg = (command[3] == ' ') ? &command[4] : "";
  while (*arg == ' ') arg++;
  // Reply first: the reset below never returns, so anything printed after it is
  // lost -- and over BLE even this one needs the pump to have run, hence the
  // small delay.
  if (strcmp(arg, "serial") == 0) {
    strcpy(reply, "OK - rebooting into serial DFU (CDC only, no drive)");
  } else if (strcmp(arg, "ota") == 0) {
    strcpy(reply, "OK - rebooting into BLE OTA DFU");
  } else if (arg[0] == 0 || strcmp(arg, "uf2") == 0) {
    strcpy(reply, "OK - rebooting into UF2 (drive appears in a few seconds)");
  } else {
    strcpy(reply, "ERR: dfu [uf2|serial|ota]");
    return true;
  }
  dfu_pending_at = millis() + 700;
  dfu_pending_mode = (strcmp(arg, "serial") == 0) ? 1 : (strcmp(arg, "ota") == 0 ? 2 : 0);
#else
  strcpy(reply, "ERR: not an nRF52 board");
#endif
  return true;
}

// Reachable from serial, the BLE console AND the mesh admin CLI, so a node that
// is already mounted can still be asked to identify itself.
bool identHandleCommand(const char* command, char* reply) {
  if (memcmp(command, "blink", 5) != 0 || (command[5] != 0 && command[5] != ' ')) return false;
#ifdef RPT_IDENT_LED_PIN
  const char* arg = (command[5] == ' ') ? &command[6] : "";
  while (*arg == ' ') arg++;
  if (strcmp(arg, "on") == 0) {
    identHold(true);
    sprintf(reply, "OK - blinking until `blink off` (or %lu min)",
            (unsigned long)(RPT_IDENT_HOLD_MS / 60000UL));
  } else if (strcmp(arg, "off") == 0) {
    identStop();
    strcpy(reply, "OK - blinking off");
  } else {
    int n = arg[0] ? atoi(arg) : 6;
    if (n < 1)   n = 6;
    if (n > 999) n = 999;
    identStart(n);
    sprintf(reply, "OK - blinking %d times", n);
  }
#else
  strcpy(reply, "ERR: no spare LED on this board");
#endif
  return true;
}

#if defined(BLE_PIN_CODE) && defined(NRF52_PLATFORM)
  // CUSTOM (TeTeHacko): exposes the repeater text console over BLE Nordic-UART
  // so the node can be queried wirelessly (advert, neighbor list with SNR)
  // without USB. Enabled by the BLE_PIN_CODE build flag; otherwise no change.
  #include <bluefruit.h>
  static BLEUart bleuart;
  static BLEDfu  bledfu;
  #define _MC_STR2(x) #x
  #define _MC_STR(x)  _MC_STR2(x)
  #ifndef BLE_DEVICE_NAME
    #define BLE_DEVICE_NAME ADVERT_NAME
  #endif
  // CUSTOM (TeTeHacko): reliable send over BLE UART, without ever blocking the
  // main loop. Bluefruit bleuart.write() is non-blocking: when the SoftDevice
  // notification queue is full it returns 0 and silently DROPS the rest, so
  // long replies (nodes/rxlog/stats-*) got truncated; the earlier retry loop
  // fixed truncation but stalled loop() up to ~0.9 s per call — enough to
  // disturb LoRa RX/TX timing while a client streams the console. Now
  // bleWriteAll() only copies into a ring buffer and bleTxPump() (called every
  // loop() pass) hands the SoftDevice as much as it will take right now, never
  // waiting. On overflow the NEWEST bytes are cut (a torn page keeps its 'E,'
  // trailer framing wrong -> the bridge just re-requests the same cursor).
  // WARNING: do NOT call bleuart.flushTXD() — without bufferTXD(true) _tx_fifo
  // is NULL and flushTXD() dereferences it → memory corruption/HardFault (this
  // was the observed lockup). In unbuffered mode write() notifies immediately,
  // no flush needed.
  #define BLE_TXRING_SIZE 4096   // power of 2; >= "  -> " + reply[1032] + CRLF with slack
  static uint8_t ble_txring[BLE_TXRING_SIZE];
  static size_t ble_tx_head = 0, ble_tx_tail = 0;   // enqueue + pump both run in loop(): no locking

  static void bleWriteAll(const char* s) {
    if (!Bluefruit.connected()) return;
    size_t used = (ble_tx_head - ble_tx_tail) & (BLE_TXRING_SIZE - 1);
    size_t space = BLE_TXRING_SIZE - 1 - used;
    size_t n = strlen(s);
    if (n > space) n = space;   // overflow -> truncate newest (see comment above)
    for (size_t i = 0; i < n; i++) {
      ble_txring[ble_tx_head] = (uint8_t)s[i];
      ble_tx_head = (ble_tx_head + 1) & (BLE_TXRING_SIZE - 1);
    }
  }

  static void bleTxPump() {
    if (ble_tx_tail == ble_tx_head) return;
    if (!Bluefruit.connected()) { ble_tx_tail = ble_tx_head; return; }   // client gone -> drop
    // chunk = ATT payload (MTU-3); bridge negotiates MTU 247 -> 244 B chunks.
    // Fallback 20 B when MTU stayed at the 23 B default (unpaired phone etc.).
    size_t maxchunk = 20;
    BLEConnection* conn = Bluefruit.Connection(Bluefruit.connHandle());
    if (conn && conn->getMtu() > 23) maxchunk = conn->getMtu() - 3;
    while (ble_tx_tail != ble_tx_head) {
      size_t avail = (ble_tx_head - ble_tx_tail) & (BLE_TXRING_SIZE - 1);
      size_t chunk = avail;
      if (chunk > maxchunk) chunk = maxchunk;
      size_t run = BLE_TXRING_SIZE - ble_tx_tail;   // stop at ring wrap; rest goes next write
      if (chunk > run) chunk = run;
      size_t w = bleuart.write(&ble_txring[ble_tx_tail], chunk);
      if (w == 0) break;   // SoftDevice queue full -> retry next loop() pass
      ble_tx_tail = (ble_tx_tail + w) & (BLE_TXRING_SIZE - 1);
    }
  }

  // CUSTOM (TeTeHacko): BLE connection tuning to stop the reconnect churn. The
  // Bluefruit default supervision timeout is only 2 s, and THIS node also relays
  // heavy LoRa flood traffic: measured behaviour is a periodic (~60 s) multi-
  // second stall (advert TX airtime / SoftDevice-LoRa coexistence). 2 s dropped
  // every 15-30 s, 6 s every ~60 s -> the stall occasionally exceeds 6 s. Widen
  // the supervision timeout to 16 s so those stalls are tolerated; a genuine
  // freeze is still caught by the 20 s hardware watchdog (16 s < 20 s). Like the
  // companion, request the params explicitly on 'secured' (some centrals ignore
  // the advertised PPCP).
  // Interval 60-80 ms (was 15-30 ms): a controlled A/B test (21 Jul 2026) showed
  // ~40 % of DIRECT-route LoRa forwards silently lost with an IDLE BLE
  // connection at 15-30 ms — the SoftDevice's per-event preemption disturbs the
  // SX126x TX path (startTransmit fail / send-complete timeout). Fewer
  // connection events = proportionally fewer collisions; the Dispatcher TX
  // retry (MESH_TX_RETRIES) covers the rest. Console throughput drops ~3x,
  // which the duty-cycled bridge tolerates fine.
  // Overridable so the pre-fix 15-30 ms interval (12/24 in 1.25 ms units) can be
  // rebuilt as a POSITIVE CONTROL: a measurement that reads zero in both the
  // treatment and the control has not shown it can detect anything. Reproducing
  // the known-bad interval proves the harness is sensitive before "no loss on the
  // fixed build" is allowed to mean anything.
  #ifndef RPT_BLE_MIN_CONN_INTERVAL
  #define RPT_BLE_MIN_CONN_INTERVAL   48    // 60 ms  (1.25 ms units)
  #endif
  #ifndef RPT_BLE_MAX_CONN_INTERVAL
  #define RPT_BLE_MAX_CONN_INTERVAL   64    // 80 ms
  #endif
  // latency 0 (not the companion's 4): the mast link is marginal (RSSI -75..-90)
  // and the disconnects are HCI reason 0x3E (lost sync), not supervision timeout.
  // With latency>0 the node may skip connection events -> the central loses sync
  // sooner -> 0x3E. Responding every event maximises sync on a weak/busy link.
  #define RPT_BLE_SLAVE_LATENCY        0
  #define RPT_BLE_CONN_SUP_TIMEOUT  1600    // 16000 ms (10 ms units); < 20 s HW watchdog

  // CUSTOM (TeTeHacko): TX power of the CONNECTION, which is a separate knob
  // from the Bluefruit.setTxPower(8) in setup(). That one only stores a value
  // that BLEAdvertising::start() feeds to sd_ble_gap_tx_power_set() with
  // BLE_GAP_TX_POWER_ROLE_ADV -- the CONN role is never set anywhere in the
  // library, so once the central connects the link drops to the SoftDevice
  // default of 0 dBm. The +8 buys discovery range across the balcony and
  // nothing else. Set it per connection, on every 'secured' (the handle is new
  // each time), and keep the default at 0 so behaviour is unchanged.
  //
  // Runtime-settable via `blepwr` because it is the reproduction lever for the
  // BLE/LoRa TX-loss bug: the effect needs the marginal link the mast has
  // (RSSI -75..-90, see the latency note above) and the bench link is -57 dBm.
  //
  // Weakening OUR side is the direction that costs us radio time, and the
  // central's would not. A connection event ends as soon as either side misses
  // a packet, so: if the CENTRAL misses our PDU, we still have to re-send it at
  // the next anchor -- an extra slave TX. If WE miss the central's PDU, we never
  // transmit in that event at all, so the SoftDevice actually uses LESS radio.
  // More slave TX = more high-priority SoftDevice work in the way of the app's
  // DIO1 handling, which is the suspected mechanism. Far enough down the ladder
  // the link drops outright and Bluefruit re-advertises (at +8 dBm, 3 channels)
  // -- also radio time, and the same reconnect churn the mast shows as 0x3E.
  //
  // Runtime and not a build flag: reflashing the XIAO costs a physical
  // double-tap + UF2 drop (BLE DFU cannot reach its bootloader, same MAC), and
  // a dose-response sweep needs many points.
  #ifndef RPT_BLE_CONN_TX_POWER
  #define RPT_BLE_CONN_TX_POWER        0    // dBm; = SoftDevice default
  #endif
  // Auto-revert, because a weakened link locks you out of the console you would
  // need to undo it. Measured on the bench: at -20 dBm the node's replies stop
  // arriving, and at -40 dBm a connection cannot be established at all -- and
  // since the value is re-applied on every 'secured', reconnecting does not
  // help. On the mast the only way back would be `reboot` over the mesh admin
  // CLI. So any weakening reverts on its own unless refreshed; `blepwr <p> hold`
  // opts out. Not persisted, so a reboot always comes up at the default anyway.
  #ifndef RPT_BLE_PWR_REVERT_MS
  #define RPT_BLE_PWR_REVERT_MS   300000    // 5 min
  #endif
  static int8_t ble_conn_tx_power = RPT_BLE_CONN_TX_POWER;
  static unsigned long ble_pwr_revert_at = 0;   // 0 = no revert armed

  // CUSTOM (TeTeHacko): last connection parameters actually in force, latched
  // from loop() so they can be read over USB after the central has gone --
  // reading them over the BLE console while it is connected would collide on
  // the shared command[160] buffer.
  //
  // This is the control for everything above: bleOnSecured() only REQUESTS
  // 15-30/60-80 ms, and the central is free to refuse. Without reading back
  // what was negotiated, "the pre-fix interval showed no loss either" cannot
  // be distinguished from "the pre-fix interval was never actually in force".
  static uint16_t ble_seen_interval = 0;   // 1.25 ms units
  static uint16_t ble_seen_latency  = 0;
  static uint16_t ble_seen_timeout  = 0;   // 10 ms units

  // returns how many live connections accepted the new power (nRF52840 only
  // takes -40,-20,-16,-12,-8,-4,0,2..8; setTxPower() rejects the rest)
  static int bleApplyConnTxPower(int8_t p, int* live_out) {
    int ok = 0, live = 0;
    for (uint16_t h = 0; h < BLE_MAX_CONNECTION; h++) {
      BLEConnection* c = Bluefruit.Connection(h);
      if (c && c->connected()) { live++; if (c->setTxPower(p)) ok++; }
    }
    if (live_out) *live_out = live;
    return ok;
  }

  static void bleOnSecured(uint16_t conn_handle) {
    ble_gap_conn_params_t cp;
    cp.min_conn_interval = RPT_BLE_MIN_CONN_INTERVAL;
    cp.max_conn_interval = RPT_BLE_MAX_CONN_INTERVAL;
    cp.slave_latency     = RPT_BLE_SLAVE_LATENCY;
    cp.conn_sup_timeout  = RPT_BLE_CONN_SUP_TIMEOUT;
    sd_ble_gap_conn_param_update(conn_handle, &cp);
    BLEConnection* c = Bluefruit.Connection(conn_handle);
    if (c) c->setTxPower(ble_conn_tx_power);
  }

  // CUSTOM (TeTeHacko): runtime BLE on/off via `ble on|off|ble` command
  // (works from serial, the BLE console AND REMOTELY from the MC app — admin
  // remote CLI). State is persistent (marker file in InternalFS), so BLE stays
  // off even across watchdog reboots on the mast; `ble on` sent over MC wakes
  // it up for OTA.
  #include <InternalFileSystem.h>
  #define BLE_OFF_MARKER "/ble_off"
  static bool ble_enabled = true;
  static bool ble_toggle_target = true;
  static unsigned long ble_toggle_at = 0;   // deferred apply so the reply can go out first

  static void blePersist(bool on) {
    if (on) {
      InternalFS.remove(BLE_OFF_MARKER);
    } else {
      auto f = InternalFS.open(BLE_OFF_MARKER, Adafruit_LittleFS_Namespace::FILE_O_WRITE);
      if (f) { f.write((uint8_t)'1'); f.close(); }
    }
  }

  static void bleApply(bool on) {
    if (on) {
      Bluefruit.Advertising.restartOnDisconnect(true);
      if (!Bluefruit.Advertising.isRunning()) Bluefruit.Advertising.start(0);
    } else {
      Bluefruit.Advertising.restartOnDisconnect(false);
      if (Bluefruit.Advertising.isRunning()) Bluefruit.Advertising.stop();
      for (uint16_t h = 0; h < BLE_MAX_CONNECTION; h++) {
        BLEConnection* c = Bluefruit.Connection(h);
        if (c && c->connected()) c->disconnect();
      }
    }
  }

  // called from MyMesh::handleCommand (serial, BLE console and mesh admin CLI)
  bool bleConsoleHandleCommand(const char* command, char* reply) {
    if (strcmp(command, "ble on") == 0) {
      ble_enabled = true; blePersist(true);
      ble_toggle_target = true; ble_toggle_at = millis() + 1500;
      strcpy(reply, "OK - BLE on (persistent)");
      return true;
    }
    if (strcmp(command, "ble off") == 0) {
      ble_enabled = false; blePersist(false);
      ble_toggle_target = false; ble_toggle_at = millis() + 1500;
      strcpy(reply, "OK - BLE off (persistent)");
      return true;
    }
    if (strcmp(command, "ble") == 0) {
      sprintf(reply, "BLE: %s%s", ble_enabled ? "on" : "off",
              Bluefruit.connected() ? " (connected)"
                : (Bluefruit.Advertising.isRunning() ? " (advertising)" : ""));
      return true;
    }
    // CUSTOM (TeTeHacko): `blepwr [dBm]` -- report or set the CONNECTION tx
    // power (see RPT_BLE_CONN_TX_POWER). Applied to every live connection AND
    // remembered for the next 'secured', so a reconnect keeps the setting.
    // The nRF52840 only accepts -40,-20,-16,-12,-8,-4,0,2..8; setTxPower()
    // rejects anything else, hence the applied/failed count in the reply.
    if (memcmp(command, "blepwr", 6) == 0 && (command[6] == 0 || command[6] == ' ')) {
      if (command[6] == ' ') {
        int8_t p = (int8_t) atoi(&command[7]);
        bool hold = strstr(&command[7], "hold") != NULL;
        int live = 0;
        int ok = bleApplyConnTxPower(p, &live);
        // only latch a value the radio actually accepted, otherwise the next
        // reconnect would silently re-apply a bogus one
        bool applied = (live == 0 || ok > 0);
        if (applied) {
          ble_conn_tx_power = p;
          ble_pwr_revert_at = (p < RPT_BLE_CONN_TX_POWER && !hold)
                            ? millis() + RPT_BLE_PWR_REVERT_MS : 0;
        }
        sprintf(reply, "%s - BLE conn tx %d dBm (%d/%d conn)%s",
                applied ? "OK" : "ERR", (int) p, ok, live,
                ble_pwr_revert_at ? ", auto-revert armed" : "");
      } else {
        sprintf(reply, "BLE conn tx: %d dBm, adv tx: %d dBm%s",
                (int) ble_conn_tx_power, (int) Bluefruit.getTxPower(),
                ble_pwr_revert_at ? " (auto-revert armed)" : "");
      }
      return true;
    }
    // CUSTOM (TeTeHacko): what the central actually agreed to, not what we asked
    // for. Values are from the last sample taken while connected.
    if (strcmp(command, "bleconn") == 0) {
      if (ble_seen_interval == 0) {
        strcpy(reply, "BLE conn: nothing seen yet");
      } else {
        sprintf(reply, "BLE conn: interval %d.%02d ms (req %d-%d), latency %d, "
                       "sup timeout %d ms%s",
                (ble_seen_interval * 125) / 100, (ble_seen_interval * 125) % 100,
                (RPT_BLE_MIN_CONN_INTERVAL * 125) / 100,
                (RPT_BLE_MAX_CONN_INTERVAL * 125) / 100,
                (int) ble_seen_latency, (int) ble_seen_timeout * 10,
                Bluefruit.connected() ? " [live]" : " [last]");
      }
      return true;
    }
    return false;
  }
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

// CUSTOM (TeTeHacko): `txpwr [dBm]` -- set LoRa output power and report what
// RadioLib actually did with the request.
//
// `set tx` cannot answer that. It clamps to -9..30, saves the pref and replies
// "OK", but SX1262 accepts only -9..+22 and rejects anything outside that range
// WITHOUT touching the PA -- so `get tx` can report a power the radio never took.
// A power sweep built on that is measuring nothing, and an earlier sweep here was
// in exactly that position: +2 dBm and -9 dBm both produced RSSI -44.1/-44.0 at a
// witness, with no way to tell "the radio ignored me" from "the link does not
// care". This is the missing instrument for that measurement.
//
// Deliberately does NOT write the pref, so a sweep cannot leave a node booting
// at some experimental power; use `set tx` when the value should persist.
// Status 0 = RADIOLIB_ERR_NONE, -13 = RADIOLIB_ERR_INVALID_OUTPUT_POWER.
bool txPwrHandleCommand(const char* command, char* reply) {
  if (memcmp(command, "txpwr", 5) != 0 || (command[5] != 0 && command[5] != ' ')) return false;
  if (command[5] == ' ') {
    int dbm = atoi(&command[6]);
    radio_driver.setTxPower((int8_t) dbm);
    int16_t st = radio_driver.getLastTxPowerStatus();
    sprintf(reply, "%s - txpwr %d dBm, RadioLib status %d%s", st == RADIOLIB_ERR_NONE ? "OK" : "ERR",
            dbm, (int) st,
            st == RADIOLIB_ERR_INVALID_OUTPUT_POWER ? " (out of range, PA UNCHANGED)" : "");
  } else {
    sprintf(reply, "txpwr: last request %d dBm, status %d (SX1262 accepts -9..22)",
            (int) radio_driver.getLastTxPowerRequested(),
            (int) radio_driver.getLastTxPowerStatus());
  }
  return true;
}

#ifdef PIN_DIAG
// CUSTOM (TeTeHacko): `dio1` -- salvage diagnostic for a node whose radio
// transmits (a witness hears the frames) but never reports completion, i.e. the
// classic dead-DIO1 signature: `sent 0`, tx_timeout climbing, `recv 0`.
//
// The board itself becomes the instrument. probeDio1() has the SX1262 generate a
// TX_DONE and then compares two things that a working line keeps in step: the
// chip's IRQ status register (over SPI, so it is true regardless of the pin) and
// the level on the pin. Where they disagree is where the break is.
bool pinDiagHandleCommand(const char* command, char* reply) {
  if (memcmp(command, "dio1", 4) != 0 || command[4] != 0) return false;

  uint32_t irq = 0;
  int lvl = -1, probe = -1;
  int st = radio_driver.probeDio1(&irq, &lvl, &probe);

  if (st == -1) { strcpy(reply, "ERR - busy transmitting, try again"); return true; }
  if (st == -3) { strcpy(reply, "ERR - this radio exposes no IRQ mask"); return true; }
  if (st == -2) {
    sprintf(reply, "dio1: NO TX_DONE within 2s (irq=0x%04lx) - the fault is NOT the DIO1 line "
                   "(SPI/BUSY/PA); the chip never finished the transmit",
            (unsigned long) irq);
    return true;
  }
  if (st != 0) { sprintf(reply, "ERR - startTransmit %d", st); return true; }

  const char* verdict;
  if (lvl == 1)        verdict = "line OK - DIO1 rose with the IRQ, look elsewhere";
  else if (probe == 1) verdict = "MCU input pad DEAD (probe wire sees the signal) -> rewiring FIXES it";
  else if (probe == 0) verdict = "net dead at the SX1262 end -> rewiring will NOT help";
  else                 verdict = "DIO1 stayed LOW - fit a probe wire (PIN_DIAG_PROBE) to tell the ends apart";
  sprintf(reply, "dio1: irq=0x%04lx TX_DONE set, pin=%d probe=%d - %s",
          (unsigned long) irq, lvl, probe, verdict);
  return true;
}
#endif

void halt() {
  while (1) ;
}

static char command[160];

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#elif defined(BOOT_DIAG_DELAY_MS) && defined(NRF52_PLATFORM)
  // CUSTOM (TeTeHacko): the same settle window, but WITHOUT MESH_DEBUG. The
  // hardfault record below is printed once and then cleared, so with only the
  // delay(1000) above it is unobservable: USB CDC has not finished enumerating,
  // the printf goes to a closed port and is dropped, and hf[0]=0 destroys the
  // evidence. MESH_DEBUG would open that window but also logs inside the mesh
  // path -- unusable on a node whose RX timing is the thing being measured.
  delay(BOOT_DIAG_DELAY_MS);
#endif

#if defined(NRF52_PLATFORM)
  // CUSTOM (TeTeHacko diagnostics): print the hardfault record saved by the
  // patched HardFault_Handler (framework debug.cpp) before the last reset.
  // 16 B at the end of RAM, reserved in nrf52840_s140_v7_extrafs.ld.
  // NOTE: needs MESH_DEBUG or BOOT_DIAG_DELAY_MS to be readable at all (above).
  {
    volatile uint32_t* hf = (volatile uint32_t*)0x2003FFF0;
    if (hf[0] == 0xFA010DEB) {
      Serial.printf("!!! HARDFAULT before last reboot: PC=%08lX LR=%08lX PSR=%08lX\n",
                    (unsigned long)hf[1], (unsigned long)hf[2], (unsigned long)hf[3]);
      hf[0] = 0;
    }
  }
#endif

#if defined(PIN_DIAG) && defined(XIAO_NRF52)
  // CUSTOM (TeTeHacko): header GPIO integrity sweep, for salvaging a board whose
  // pins were abused (tth-x0 took GND on D1 and 5 V on D0 in a breadboard).
  //
  // Deliberately placed BEFORE radio_init(), because that halt()s when the radio
  // does not answer -- so this is the only diagnostic that still runs with the
  // XIAO UNPLUGGED from the Wio-SX1262 module. Unplugged is also the only state
  // in which the D1 result means anything: attached, the SX1262 drives DIO1 low
  // and a perfectly healthy board reads 0 too.
  //
  // The internal pull resistor IS the ohmmeter (~13 kOhm): a floating pin follows
  // it, a pin shorted to a rail does not. Run the same build on a known-good
  // board as the paired control.
  //
  // What the pull test CANNOT see: a lifted pad or broken bond. The resistor sits
  // on the die, so an open pad still reads 1/0 exactly like a healthy one while
  // the outside world is disconnected. Only the loopback below catches that.
  {
    static const uint8_t diag_pins[] = { D0, D1, D2, D3, D4, D5, D6, D7, D8, D9, D10 };
    static const char* diag_use[]    = { "-", "DIO1", "RESET", "BUSY", "NSS", "RXEN",
                                         "SCL", "SDA", "SCK", "MISO", "MOSI" };
    Serial.println("PIN_DIAG: unplug the Wio module first, otherwise the radio drives these pins");
    Serial.println("PIN_DIAG: pin  use    pu pd  verdict");
    for (uint8_t i = 0; i < sizeof(diag_pins); i++) {
      uint8_t p = diag_pins[i];
      pinMode(p, INPUT_PULLUP);   delay(2);  int pu = digitalRead(p);
      pinMode(p, INPUT_PULLDOWN); delay(2);  int pd = digitalRead(p);
      pinMode(p, INPUT);
      const char* v = (pu == 1 && pd == 0) ? "follows pulls (pad healthy OR open)"
                    : (pu == 0 && pd == 0) ? "STUCK LOW  <- shorted to GND?"
                    : (pu == 1 && pd == 1) ? "STUCK HIGH <- shorted to VDD?"
                                           : "inverted?!";
      Serial.printf("PIN_DIAG: D%-2d  %-5s  %d  %d  %s\n", (int) i, diag_use[i], pu, pd, v);
    }

  #if defined(PIN_DIAG_LOOP_A) && defined(PIN_DIAG_LOOP_B)
    // Loopback over ONE jumper wire between two pins -- the only test that can
    // see a lifted pad, because it drives from outside the die. Each read pulls
    // AGAINST the driver, so a pass proves the driver actually wins the pin.
    //
    // DANGER: this DRIVES pins. Never enable it with the Wio module attached.
    // Driving DIO1 against the SX1262's own output is the very fight that is
    // suspected of having killed this board.
    {
      const uint8_t pair[2] = { PIN_DIAG_LOOP_A, PIN_DIAG_LOOP_B };
      int ok = 0;
      for (int dir = 0; dir < 2; dir++) {
        uint8_t drv = pair[dir], rd = pair[1 - dir];
        pinMode(drv, OUTPUT);
        for (int lvl = 0; lvl < 2; lvl++) {
          digitalWrite(drv, lvl);
          pinMode(rd, lvl ? INPUT_PULLDOWN : INPUT_PULLUP);   // pull against it
          delay(2);
          int got = digitalRead(rd);
          if (got == lvl) ok++;
          Serial.printf("PIN_DIAG: loop pin%d=%d -> pin%d reads %d  %s\n",
                        (int) drv, lvl, (int) rd, got, got == lvl ? "ok" : "FAIL");
        }
        pinMode(drv, INPUT);
      }
      Serial.printf("PIN_DIAG: loopback %d/4 - 4 means both pads drive AND sense through the wire\n", ok);
    }
  #endif

    // The sweep took the I2C pins away from the TWI peripheral that board.begin()
    // configured. Nothing here depends on I2C (no RTC, no sensors) and the clock
    // falls back to VolatileRTCClock, but say so rather than leave it silent.
    Serial.println("PIN_DIAG: done (I2C pin config was clobbered by the sweep)");
  }
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    MESH_DEBUG_PRINTLN("Radio init failed!");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif

#if defined(BLE_PIN_CODE) && defined(NRF52_PLATFORM)
  // BLE UART console (PIN pairing). Runs alongside normal repeater duty.
  Bluefruit.begin();
  // +8 dBm = nRF52840 max (range to the dongle across the balcony). NOTE: this
  // is ADVERTISING power only -- Bluefruit stores it and BLEAdvertising::start()
  // hands it to sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_ADV). Nothing in
  // the library ever sets the CONN role, so an established link runs at the
  // SoftDevice default 0 dBm. See RPT_BLE_CONN_TX_POWER / `blepwr` for that half.
  Bluefruit.setTxPower(8);
  // CUSTOM (TeTeHacko): advertise the node's ACTUAL name, not the build-time
  // constant. BLE_DEVICE_NAME defaults to ADVERT_NAME, which only ever seeds a
  // fresh install -- so once several boards share one image, they all advertise
  // the same string no matter what `set name` says, and a BLE scan cannot tell
  // them apart. Four boards here advertised "tth-x1-rpt" at once, including the
  // one with a dead radio. The prefs name is the one that was deliberately set,
  // so that is the one to broadcast; the build constant stays the fallback for a
  // node that has never been named.
  {
    const char* nm = the_mesh.getNodePrefs()->node_name;
    Bluefruit.setName((nm && nm[0]) ? nm : BLE_DEVICE_NAME);
  }
  Bluefruit.Security.setPIN(_MC_STR(BLE_PIN_CODE));
  // widen the BLE connection params so the busy LoRa loop can't trip the
  // (default 2 s) supervision timeout -> no more reconnect churn. PPCP is
  // advertised AND requested explicitly on 'secured' (bleOnSecured).
  Bluefruit.Periph.setConnInterval(RPT_BLE_MIN_CONN_INTERVAL, RPT_BLE_MAX_CONN_INTERVAL);
  Bluefruit.Periph.setConnSlaveLatency(RPT_BLE_SLAVE_LATENCY);
  Bluefruit.Periph.setConnSupervisionTimeout(RPT_BLE_CONN_SUP_TIMEOUT);
  Bluefruit.Security.setSecuredCallback(bleOnSecured);
  // Without setPermission the characteristics stay SECMODE_OPEN and the PIN is
  // never enforced -> the admin console would be open to anyone in range. Same
  // pattern as SerialBLEInterface.cpp (companion).
  bledfu.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
  bledfu.begin();
  bleuart.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_ENC_WITH_MITM);
  bleuart.begin();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleuart);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  // persistent BLE state: with the marker present keep BLE dark (see `ble on|off`)
  ble_enabled = !InternalFS.exists(BLE_OFF_MARKER);
  if (ble_enabled) {
    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.start(0);
    Serial.println("BLE UART console started");
  } else {
    Bluefruit.Advertising.restartOnDisconnect(false);
    Serial.println("BLE UART console disabled (ble off)");
  }
#endif

  board.onBootComplete();

#if defined(NRF52_PLATFORM)
  // CUSTOM (TeTeHacko): hardware watchdog. If the main loop freezes COMPLETELY
  // (observed with BLE/SoftDevice — even serial dies, only reset helps), the
  // chip resets itself after ~20 s and the repeater comes back — no physical
  // reset needed (it lives on a mast).
  //   SLEEP=Pause -> WDT only counts while the CPU runs -> no resets during
  //   power-saving sleep (no false positives). A freeze = CPU spinning/fault.
  //   HALT=Pause -> does not get in the debugger's way. The Adafruit bootloader
  //   feeds the WDT during DFU -> OTA stays safe.
  NRF_WDT->CONFIG = (WDT_CONFIG_HALT_Pause  << WDT_CONFIG_HALT_Pos)
                  | (WDT_CONFIG_SLEEP_Pause << WDT_CONFIG_SLEEP_Pos);
  NRF_WDT->CRV = 20 * 32768 - 1;                              // ~20 s timeout (LFCLK 32.768 kHz)
  NRF_WDT->RREN = (WDT_RREN_RR0_Enabled << WDT_RREN_RR0_Pos); // reload register 0
  NRF_WDT->TASKS_START = 1;
#endif
}

void loop() {
#if defined(NRF52_PLATFORM)
  NRF_WDT->RR[0] = WDT_RR_RR_Reload;   // feed the HW watchdog (setup); if the loop freezes the chip resets in ~20 s
#endif
#ifdef RPT_IDENT_LED_PIN
  identTick();   // CUSTOM (TeTeHacko): drive `blink` without blocking the mesh
#endif
#if defined(NRF52_PLATFORM)
  dfuTick();     // CUSTOM (TeTeHacko): deferred `dfu` reboot, after the reply went out
#endif
#if defined(BLE_PIN_CODE) && defined(NRF52_PLATFORM)
  // CUSTOM (TeTeHacko): deferred apply of `ble on|off` — the reply still goes
  // out over the old path, only then advertising/connections get switched.
  if (ble_toggle_at && (long)(millis() - ble_toggle_at) >= 0) {
    ble_toggle_at = 0;
    bleApply(ble_toggle_target);
  }
  // CUSTOM (TeTeHacko): give back the BLE tx power a `blepwr` took away, so a
  // weakened link cannot outlive the console you would need to fix it.
  if (ble_pwr_revert_at && (long)(millis() - ble_pwr_revert_at) >= 0) {
    ble_pwr_revert_at = 0;
    ble_conn_tx_power = RPT_BLE_CONN_TX_POWER;
    bleApplyConnTxPower(ble_conn_tx_power, NULL);
  }
  // CUSTOM (TeTeHacko): latch the live connection parameters once a second, so
  // `bleconn` can report them after the central is gone (see ble_seen_*).
  {
    static unsigned long _lastParamChk = 0;
    if ((unsigned long)(millis() - _lastParamChk) >= 1000) {
      _lastParamChk = millis();
      for (uint16_t h = 0; h < BLE_MAX_CONNECTION; h++) {
        BLEConnection* c = Bluefruit.Connection(h);
        if (c && c->connected()) {
          ble_seen_interval = c->getConnectionInterval();
          ble_seen_latency  = c->getSlaveLatency();
          ble_seen_timeout  = c->getSupervisionTimeout();
          break;
        }
      }
    }
  }
  // CUSTOM (TeTeHacko): BLE watchdog. Observed: the repeater stops advertising
  // after a while (SoftDevice/LoRa coexistence) and only a reset helps. Every
  // 5 s check: if not connected and not advertising, restart advertising —
  // without needing a physical reset. Respects `ble off`.
  {
    static unsigned long _lastAdvChk = 0;
    if ((unsigned long)(millis() - _lastAdvChk) >= 5000) {
      _lastAdvChk = millis();
      if (ble_enabled && ble_toggle_at == 0
          && Bluefruit.connected() == 0 && !Bluefruit.Advertising.isRunning()) {
        Bluefruit.Advertising.start(0);
      }
    }
  }
  bleTxPump();   // drain queued console output, never blocks (see bleWriteAll)
#endif
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
#if defined(BLE_PIN_CODE) && defined(NRF52_PLATFORM)
  // same console from BLE UART too (commands share the buffer)
  while (bleuart.available() && len < (int)sizeof(command)-1) {
    char c = (char) bleuart.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
    }
    if (c == '\r') break;
  }
#endif
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    static char reply[1032];   // big pages for paged analyzer replies (nodes/rxlog); BSS, ne stack
    the_mesh.handleCommand(0, command, reply, (int)sizeof(reply) - 32);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
#if defined(BLE_PIN_CODE) && defined(NRF52_PLATFORM)
      bleWriteAll("  -> "); bleWriteAll(reply); bleWriteAll("\r\n");
#endif
    }

    command[0] = 0;  // reset command buffer
  }

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
}
