#pragma once

#include <Mesh.h>
#include <helpers/SensorManager.h>
#include <helpers/sensors/LocationProvider.h>

// GPS duty cycle (`gps duty`, gps_enabled == 2). A repeater that runs GPS only
// for the clock does not need a continuous fix: the receiver is powered just
// long enough to sync the RTC, then cut. On a solar node this is the single
// biggest lever there is -- an L76K draws 25-35 mA, an order of magnitude more
// than the whole radio + MCU budget it is being spent next to.
#ifndef GPS_DUTY_SYNC_INTERVAL_SECS
  // Same cadence as MicroNMEALocationProvider::TIME_SYNC_INTERVAL, so clock
  // accuracy is unchanged from `gps on` -- only the power in between differs.
  #define GPS_DUTY_SYNC_INTERVAL_SECS   1800
#endif
#ifndef GPS_DUTY_MAX_AWAKE_SECS
  // Cap on one sync attempt: a blocked sky must not turn duty cycle back into
  // "always on". A cold fix is tens of seconds, so this is deliberately loose.
  #define GPS_DUTY_MAX_AWAKE_SECS       300
#endif
#ifndef GPS_DUTY_RETRY_SECS
  // Backoff after a window that produced no fix (antenna, snow, indoors).
  #define GPS_DUTY_RETRY_SECS           3600
#endif

class EnvironmentSensorManager : public SensorManager {
protected:
  static const int MAX_ACTIVE_SENSORS = 16;

  // Query function pointer + sub-channel index (for multi-channel sensors like INA3221).
  // Sub-channel is 0 for all single-output sensors.
  struct ActiveSensor {
    void    (*query)(uint8_t channel, uint8_t sub_channel, CayenneLPP& telemetry);
    uint8_t   sub_channel;
  };

  ActiveSensor _active_sensors[MAX_ACTIVE_SENSORS];
  int          _active_sensor_count = 0;
  uint8_t      next_available_channel = TELEM_CHANNEL_SELF + 1;

  bool     gps_detected = false;
  bool     gps_active = false;
  uint32_t gps_update_interval_sec = 1;

  #if ENV_INCLUDE_GPS
  // Duty-cycle state. `gps_duty` is the mode; `gps_active` stays the truth
  // about the receiver being powered, so everything that already reads it
  // (telemetry, settings) keeps working unchanged.
  bool     gps_duty = false;
  uint32_t gps_wake_at = 0;      // millis of the next sync attempt
  uint32_t gps_awake_until = 0;  // millis the current attempt gives up at

  LocationProvider* _location;
  void start_gps();
  void stop_gps();
  void gpsDutyLoop();
  void initBasicGPS();
  #ifdef RAK_BOARD
  void rakGPSInit();
  bool gpsIsAwake(uint8_t ioPin);
  #endif
  #endif

public:
  #if ENV_INCLUDE_GPS
  EnvironmentSensorManager(LocationProvider &location): _location(&location){};
  LocationProvider* getLocationProvider() { return _location; }
  #else
  EnvironmentSensorManager(){};
  #endif
  bool begin() override;
  bool querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) override;
  #if ENV_INCLUDE_GPS || defined(ENV_INCLUDE_BME680_BSEC)
  void loop() override;
  #endif
  int getNumSettings() const override;
  const char* getSettingName(int i) const override;
  const char* getSettingValue(int i) const override;
  bool setSettingValue(const char* name, const char* value) override;
  int scanI2C(char* dest, size_t max_len) override;
};
