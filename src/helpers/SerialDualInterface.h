#pragma once

// Upstream backport (TeTeHacko): https://github.com/meshcore-dev/MeshCore/pull/1779
// Makes the BLE companion usable over USB while BLE is disconnected.
// Once the PR is merged upstream, this file just takes the upstream version on rebase.

#include "ArduinoSerialInterface.h"

template <typename BLEInterface>
class SerialDualInterface : public BaseSerialInterface {
  BLEInterface _ble;
  ArduinoSerialInterface _usb;
  bool _bleWasConnected;
  bool _lastFrameFromBle;

public:
  SerialDualInterface() : _bleWasConnected(false), _lastFrameFromBle(false) { }

  void begin(const char* prefix, char* name, uint32_t pin_code, Stream& serial) {
    _ble.begin(prefix, name, pin_code);
    _usb.begin(serial);
  }

  // enable/disable only control BLE, USB is always available
  void enable() override {
    _ble.enable();
  }

  void disable() override {
    _ble.disable();
  }

  bool isEnabled() const override {
    return _ble.isEnabled() || _usb.isEnabled();
  }

  bool isConnected() const override {
    return _ble.isConnected() || _usb.isConnected();
  }

  // Only frames received over the MITM-paired BLE link are authenticated;
  // frames received over USB are unauthenticated (physical presence only).
  bool isConnectionSecure() const override {
    return _lastFrameFromBle;
  }

  bool isWriteBusy() const override {
    if (_ble.isConnected()) return _ble.isWriteBusy();
    return _usb.isWriteBusy();
  }

  size_t writeFrame(const uint8_t src[], size_t len) override {
    if (_ble.isConnected()) return _ble.writeFrame(src, len);
    return _usb.writeFrame(src, len);
  }

  size_t checkRecvFrame(uint8_t dest[]) override {
    // Always call BLE first, it needs polling for send queue draining
    // and advertising watchdog, even when USB is the active transport
    size_t n = _ble.checkRecvFrame(dest);

    bool bleNow = _ble.isConnected();
    if (bleNow != _bleWasConnected) {
      if (bleNow) {
        // BLE just connected, wait for any pending USB write to finish
        while (_usb.isWriteBusy()) { }
        _usb.disable();
      } else {
        // BLE just disconnected, re-enable USB (resets state machine,
        // discarding any stale bytes from the previous session)
        _usb.enable();
      }
      _bleWasConnected = bleNow;
    }

    // Dock behaviour: while a USB host is connected (card sitting in the dock),
    // stop BLE advertising so the node stays quiet on 2.4 GHz and the app talks
    // over USB; resume advertising once undocked. Only advertising is gated --
    // an already-established BLE link is left untouched (never force-disconnect).
    if (!bleNow) {
      bool docked = _usb.isConnected();
      if (docked && _ble.isEnabled()) {
        _ble.disable();
      } else if (!docked && !_ble.isEnabled()) {
        _ble.enable();
      }
    }

    if (n > 0) {  // got a BLE frame
      _lastFrameFromBle = true;
      return n;
    }

    // Only check USB when BLE is not connected
    if (!bleNow) {
      n = _usb.checkRecvFrame(dest);
      if (n > 0) _lastFrameFromBle = false;
      return n;
    }
    return 0;
  }
};
