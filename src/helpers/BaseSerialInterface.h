#pragma once

#include <Arduino.h>

#define MAX_FRAME_SIZE  176   // +4 for transport codes (region scoping)

class BaseSerialInterface {
protected:
  BaseSerialInterface() { }

public:
  virtual void enable() = 0;
  virtual void disable() = 0;
  virtual bool isEnabled() const = 0;

  virtual bool isConnected() const = 0;

  virtual bool isWriteBusy() const = 0;
  virtual size_t writeFrame(const uint8_t src[], size_t len) = 0;
  virtual size_t checkRecvFrame(uint8_t dest[]) = 0;

  // Whether the transport carrying the current / most-recent frame is
  // authenticated. A single-transport interface is trusted by definition
  // (physically-connected USB, or a MITM-paired BLE link), so the default is
  // true. Multi-transport wrappers (e.g. SerialDualInterface) override this to
  // distinguish an unauthenticated local channel (USB) from a paired one, so
  // callers can gate sensitive commands (key export, PIN read/set).
  virtual bool isConnectionSecure() const { return true; }
};
