#pragma once

#define RADIOLIB_STATIC_ONLY 1
#include <RadioLib.h>
#include <helpers/radiolib/RadioLibWrappers.h>
#include "T1000eBoard.h"
#include <helpers/radiolib/CustomLR1110Wrapper.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/SensorManager.h>
#include <helpers/sensors/LocationProvider.h>
#ifdef DISPLAY_CLASS
  #include "NullDisplayDriver.h"
#endif

class T1000SensorManager: public SensorManager {
  bool gps_active = false;
  LocationProvider * _nmea;

  // Duty-cycle state, mirroring EnvironmentSensorManager: `gps_duty` is the
  // MODE, `gps_active` stays the truth about the receiver's power right now.
  // This board has its own manager, so the state machine cannot simply be
  // inherited -- see gpsDutyLoop() for the one T1000-specific difference.
  bool     gps_duty = false;
  uint32_t gps_wake_at = 0;      // millis of the next sync attempt
  uint32_t gps_awake_until = 0;  // millis the current attempt gives up at

  void start_gps();
  void sleep_gps();
  void stop_gps();
  void gpsDutyLoop();
public:
  T1000SensorManager(LocationProvider &nmea): _nmea(&nmea) { }
  bool begin() override;
  bool querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) override;
  void loop() override;
  int getNumSettings() const override;
  const char* getSettingName(int i) const override;
  const char* getSettingValue(int i) const override;
  bool setSettingValue(const char* name, const char* value) override;
  LocationProvider* getLocationProvider() { return _nmea; }
};

#ifdef DISPLAY_CLASS
  extern NullDisplayDriver display;
#endif

extern T1000eBoard board;
extern WRAPPER_CLASS radio_driver;
extern VolatileRTCClock rtc_clock;
extern T1000SensorManager sensors;

bool radio_init();
mesh::LocalIdentity radio_new_identity();
