#include <Arduino.h>
#include "t1000e_sensors.h"
#include "target.h"
#include <helpers/sensors/MicroNMEALocationProvider.h>

T1000eBoard board;

RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, SPI);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock rtc_clock;
MicroNMEALocationProvider nmea = MicroNMEALocationProvider(Serial1, &rtc_clock);
T1000SensorManager sensors = T1000SensorManager(nmea);

#ifdef DISPLAY_CLASS
  NullDisplayDriver display;
#endif

#ifndef LORA_CR
  #define LORA_CR      5
#endif

#ifdef RF_SWITCH_TABLE
static const uint32_t rfswitch_dios[Module::RFSWITCH_MAX_PINS] = {
  RADIOLIB_LR11X0_DIO5,
  RADIOLIB_LR11X0_DIO6,
  RADIOLIB_LR11X0_DIO7,
  RADIOLIB_LR11X0_DIO8, 
  RADIOLIB_NC
};

static const Module::RfSwitchMode_t rfswitch_table[] = {
  // mode                 DIO5  DIO6  DIO7  DIO8
  { LR11x0::MODE_STBY,   {LOW,  LOW,  LOW,  LOW  }},  
  { LR11x0::MODE_RX,     {HIGH, LOW,  LOW,  HIGH }},
  { LR11x0::MODE_TX,     {HIGH, HIGH, LOW,  HIGH }},
  { LR11x0::MODE_TX_HP,  {LOW,  HIGH, LOW,  HIGH }},
  { LR11x0::MODE_TX_HF,  {LOW,  LOW,  LOW,  LOW  }}, 
  { LR11x0::MODE_GNSS,   {LOW,  LOW,  HIGH, LOW  }},
  { LR11x0::MODE_WIFI,   {LOW,  LOW,  LOW,  LOW  }},  
  END_OF_MODE_TABLE,
};
#endif

bool radio_init() {
  //rtc_clock.begin(Wire);
  
#ifdef LR11X0_DIO3_TCXO_VOLTAGE
  float tcxo = LR11X0_DIO3_TCXO_VOLTAGE;
#else
  float tcxo = 1.6f;
#endif

  SPI.setPins(P_LORA_MISO, P_LORA_SCLK, P_LORA_MOSI);
  SPI.begin();
  int status = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, RADIOLIB_LR11X0_LORA_SYNC_WORD_PRIVATE, LORA_TX_POWER, 16, tcxo);
  if (status != RADIOLIB_ERR_NONE) {
    Serial.print("ERROR: radio init failed: ");
    Serial.println(status);
    return false;  // fail
  }
  
  radio.setCRC(2);
  radio.explicitHeader();

#ifdef RF_SWITCH_TABLE
  radio.setRfSwitchTable(rfswitch_dios, rfswitch_table);
#endif
#ifdef RX_BOOSTED_GAIN
  radio.setRxBoostedGainMode(RX_BOOSTED_GAIN);
#endif

  return true;  // success
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}

void T1000SensorManager::start_gps() {
  gps_active = true;
  //_nmea->begin();
  // this init sequence should be better 
  // comes from seeed examples and deals with all gps pins
  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, HIGH);
  delay(10);
  pinMode(GPS_VRTC_EN, OUTPUT);
  digitalWrite(GPS_VRTC_EN, HIGH);
  delay(10);
       
  pinMode(GPS_RESET, OUTPUT);
  digitalWrite(GPS_RESET, HIGH);
  delay(10);
  digitalWrite(GPS_RESET, LOW);
       
  pinMode(GPS_SLEEP_INT, OUTPUT);
  digitalWrite(GPS_SLEEP_INT, HIGH);
  pinMode(GPS_RTC_INT, OUTPUT);
  digitalWrite(GPS_RTC_INT, LOW);
  pinMode(GPS_RESETB, INPUT_PULLUP);
}

void T1000SensorManager::sleep_gps() {
  gps_active = false;
  digitalWrite(GPS_VRTC_EN, HIGH);
  digitalWrite(GPS_EN, LOW);
  digitalWrite(GPS_RESET, HIGH);
  digitalWrite(GPS_SLEEP_INT, HIGH);
  digitalWrite(GPS_RTC_INT, LOW);
  pinMode(GPS_RESETB, OUTPUT);
  digitalWrite(GPS_RESETB, LOW);
  //_nmea->stop();
}

void T1000SensorManager::stop_gps() {
  gps_active = false;
  digitalWrite(GPS_VRTC_EN, LOW);
  digitalWrite(GPS_EN, LOW);
  digitalWrite(GPS_RESET, HIGH);
  digitalWrite(GPS_SLEEP_INT, HIGH);
  digitalWrite(GPS_RTC_INT, LOW);
  pinMode(GPS_RESETB, OUTPUT);
  digitalWrite(GPS_RESETB, LOW);
  //_nmea->stop();
}


// Powers the receiver only around a clock sync. Same cadence and the same
// reasoning as EnvironmentSensorManager::gpsDutyLoop() -- the GPS is by far the
// biggest consumer on the node (46 mA measured), and a node that runs it only
// to keep its RTC set does not need a continuous fix.
//
// One deliberate difference from the environment manager: the off-state here is
// sleep_gps(), not stop_gps(). Sleep keeps GPS_VRTC_EN asserted, so the receiver
// retains its almanac and the next window gets a warm fix; stop cuts VRTC too and
// would buy a cold start every 30 minutes -- which is exactly what duty cycling
// is trying not to pay for.
void T1000SensorManager::gpsDutyLoop() {
  if (!gps_duty) return;

  // int32_t deltas, so the 49-day millis() wrap does not park GPS on (or off)
  // forever.
  if (!gps_active) {
    if ((int32_t)(millis() - gps_wake_at) >= 0) {
      start_gps();
      _nmea->syncTime();  // clears the parser + arms the sync request
      gps_awake_until = millis() + (uint32_t)GPS_DUTY_MAX_AWAKE_SECS * 1000;
      MESH_DEBUG_PRINTLN("gps duty: awake, waiting for fix");
    }
    return;
  }

  if (!_nmea->waitingTimeSync()) {   // RTC got set -> nothing left to stay on for
    sleep_gps();
    gps_wake_at = millis() + (uint32_t)GPS_DUTY_SYNC_INTERVAL_SECS * 1000;
    MESH_DEBUG_PRINTLN("gps duty: synced, off for %d s", (int)GPS_DUTY_SYNC_INTERVAL_SECS);
  } else if ((int32_t)(millis() - gps_awake_until) >= 0) {
    sleep_gps();
    gps_wake_at = millis() + (uint32_t)GPS_DUTY_RETRY_SECS * 1000;
    MESH_DEBUG_PRINTLN("gps duty: no fix in window, retry in %d s", (int)GPS_DUTY_RETRY_SECS);
  }
}

bool T1000SensorManager::begin() {
  // init GPS
  Serial1.begin(115200);
  return true;
}

bool T1000SensorManager::querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) {
  if (requester_permissions & TELEM_PERM_LOCATION) {   // does requester have permission?
    telemetry.addGPS(TELEM_CHANNEL_SELF, node_lat, node_lon, node_altitude);
  }
  if (requester_permissions & TELEM_PERM_ENVIRONMENT) {
    // Firmware reports light as a 0-100 % scale, but expose it via Luminosity so app labels it "Luminosity".
    telemetry.addLuminosity(TELEM_CHANNEL_SELF, t1000e_get_light());
    telemetry.addTemperature(TELEM_CHANNEL_SELF, t1000e_get_temperature());
  }
  return true;
}

void T1000SensorManager::loop() {
  static long next_gps_update = 0;

  _nmea->loop();
  gpsDutyLoop();

  if (millis() > next_gps_update) {
    if (gps_active && _nmea->isValid()) {
      node_lat = ((double)_nmea->getLatitude())/1000000.;
      node_lon = ((double)_nmea->getLongitude())/1000000.;
      node_altitude = ((double)_nmea->getAltitude()) / 1000.0;
      //Serial.printf("lat %f lon %f\r\n", _lat, _lon);
    }
    next_gps_update = millis() + 1000;
  }
}

int T1000SensorManager::getNumSettings() const { return 1; }  // just one supported: "gps" (power switch)

const char* T1000SensorManager::getSettingName(int i) const {
  return i == 0 ? "gps" : NULL;
}
const char* T1000SensorManager::getSettingValue(int i) const {
  if (i == 0) {
    // In duty mode report the mode, not the momentary power state: asleep
    // between syncs is not the same thing as `gps off`, and something that
    // round-trips this value must not silently downgrade the mode.
    if (gps_duty) return "2";
    return gps_active ? "1" : "0";
  }
  return NULL;
}
bool T1000SensorManager::setSettingValue(const char* name, const char* value) {
  if (strcmp(name, "gps") == 0) {
    if (strcmp(value, "0") == 0) {
      gps_duty = false;
      sleep_gps(); // sleep for faster fix !
    } else if (strcmp(value, "2") == 0) {
      // Duty cycle: power up now (the clock is volatile, so the first sync is
      // wanted immediately at boot) and let gpsDutyLoop() cut power once the
      // RTC is set. The awake deadline MUST be armed here as well as in the
      // loop -- left at 0 it is already in the past, and the first loop pass
      // would cut GPS before it ever saw a satellite and back off for an hour.
      gps_duty = true;
      gps_wake_at = millis();
      gps_awake_until = millis() + (uint32_t)GPS_DUTY_MAX_AWAKE_SECS * 1000;
      start_gps();
    } else {
      gps_duty = false;
      start_gps();
    }
    return true;
  }
  return false;  // not supported
}
