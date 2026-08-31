#pragma once

#include <CayenneLPP.h>
#include "sensors/LocationProvider.h"

#define TELEM_PERM_BASE         0x01   // 'base' permission includes battery
#define TELEM_PERM_LOCATION     0x02
#define TELEM_PERM_ENVIRONMENT  0x04   // permission to access environment sensors

#define TELEM_CHANNEL_SELF   1   // LPP data channel for 'self' device

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

class SensorManager {
public:
  double node_lat, node_lon;  // modify these, if you want to affect Advert location
  double node_altitude;       // altitude in meters

  SensorManager() { node_lat = 0; node_lon = 0; node_altitude = 0; }
  virtual bool begin() { return false; }
  virtual bool querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) { return false; }
  virtual void loop() { }
  virtual int getNumSettings() const { return 0; }
  virtual const char* getSettingName(int i) const { return NULL; }
  virtual const char* getSettingValue(int i) const { return NULL; }
  virtual bool setSettingValue(const char* name, const char* value) { return false; }
  virtual LocationProvider* getLocationProvider() { return NULL; }

  // Asked for when someone with LOCATION permission requests telemetry, so a
  // duty-cycled receiver does not answer with a position that is up to
  // GPS_DUTY_SYNC_INTERVAL_SECS old for the rest of that window.
  //
  // It deliberately does NOT wait for the fix -- the reply is assembled and
  // sent synchronously (BaseChatMesh.cpp), while a warm fix measured on a
  // T1000-E took ~65 s. So this request answers with what is known and pulls
  // the next window forward; the FOLLOWING request gets the fresh position.
  // With a periodic collector that costs one cycle and needs no scheduling.
  virtual void requestLocationRefresh() { }

  // Live I2C scan, written into dest as text. Returns the number of devices
  // that answered, or -1 when the platform has no bus to scan.
  //
  // begin() already scans the bus, but only into a MESH_DEBUG log nobody has on
  // a deployed node -- so "is that sensor actually there?" was answerable only
  // by inference from telemetry, and inference is what produced a build flag
  // pointing INA226 at 0x40 to dodge a false detect at 0x44 (SHT41's address).
  virtual int scanI2C(char* dest, size_t max_len) { (void)dest; (void)max_len; return -1; }

  // Helper functions to manage setting by keys (useful in many places ...)
  const char* getSettingByKey(const char* key) {
    int num = getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(getSettingName(i), key) == 0) {
        return getSettingValue(i);
      }
    }
    return NULL;
  }
};
