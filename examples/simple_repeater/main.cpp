#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

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
  // CUSTOM (TeTeHacko): reliable send over BLE UART.
  // When the SoftDevice notification queue is full, Bluefruit bleuart.write()
  // returns fewer bytes and silently DROPS the rest (non-blocking), so long
  // replies spanning multiple notifications (neighbors/stats-*) get truncated.
  // Fix: send in <=20 B chunks (1 notification) + retry unwritten bytes with a
  // short pause. The stall guard is bounded so this can NEVER block the main
  // loop for long (watchdog would reset otherwise).
  // WARNING: do NOT call bleuart.flushTXD() — without bufferTXD(true) _tx_fifo
  // is NULL and flushTXD() dereferences it → memory corruption/HardFault (this
  // was the observed lockup). In unbuffered mode write() notifies immediately,
  // no flush needed.
  static void bleWriteAll(const char* s) {
    size_t n = strlen(s), i = 0; uint32_t stall = 0;
    while (i < n && Bluefruit.connected() && stall < 300) {   // max ~0.9 s stall → bail
      size_t chunk = n - i; if (chunk > 20) chunk = 20;
      size_t w = bleuart.write((const uint8_t*)(s + i), chunk);
      if (w > 0) { i += w; stall = 0; }
      else { stall++; delay(3); }   // queue full -> wait a moment and retry
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
  #define RPT_BLE_MIN_CONN_INTERVAL   12    // 15 ms  (1.25 ms units)
  #define RPT_BLE_MAX_CONN_INTERVAL   24    // 30 ms
  #define RPT_BLE_SLAVE_LATENCY        4
  #define RPT_BLE_CONN_SUP_TIMEOUT  1600    // 16000 ms (10 ms units); < 20 s HW watchdog
  static void bleOnSecured(uint16_t conn_handle) {
    ble_gap_conn_params_t cp;
    cp.min_conn_interval = RPT_BLE_MIN_CONN_INTERVAL;
    cp.max_conn_interval = RPT_BLE_MAX_CONN_INTERVAL;
    cp.slave_latency     = RPT_BLE_SLAVE_LATENCY;
    cp.conn_sup_timeout  = RPT_BLE_CONN_SUP_TIMEOUT;
    sd_ble_gap_conn_param_update(conn_handle, &cp);
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
#endif

#if defined(NRF52_PLATFORM)
  // CUSTOM (TeTeHacko diagnostics): print the hardfault record saved by the
  // patched HardFault_Handler (framework debug.cpp) before the last reset.
  // 16 B at the end of RAM, reserved in nrf52840_s140_v7_extrafs.ld.
  {
    volatile uint32_t* hf = (volatile uint32_t*)0x2003FFF0;
    if (hf[0] == 0xFA010DEB) {
      Serial.printf("!!! HARDFAULT before last reboot: PC=%08lX LR=%08lX PSR=%08lX\n",
                    (unsigned long)hf[1], (unsigned long)hf[2], (unsigned long)hf[3]);
      hf[0] = 0;
    }
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
  Bluefruit.setTxPower(8);   // +8 dBm = nRF52840 max (range to the dongle across the balcony)
  Bluefruit.setName(BLE_DEVICE_NAME);
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
#if defined(BLE_PIN_CODE) && defined(NRF52_PLATFORM)
  // CUSTOM (TeTeHacko): deferred apply of `ble on|off` — the reply still goes
  // out over the old path, only then advertising/connections get switched.
  if (ble_toggle_at && (long)(millis() - ble_toggle_at) >= 0) {
    ble_toggle_at = 0;
    bleApply(ble_toggle_target);
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
    char reply[160];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
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
