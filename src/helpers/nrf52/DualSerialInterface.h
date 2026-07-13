#pragma once

#include "../BaseSerialInterface.h"
#include "../ArduinoSerialInterface.h"
#include "SerialBLEInterface.h"

// Wraps BLE + USB serial interfaces: BLE takes priority when connected,
// USB is always ready as a fallback.
// enable()/disable() control BLE only — USB is always on.
// BLE state machine is only pumped when BLE is enabled; USB is not read while BLE is connected.
class DualSerialInterface : public BaseSerialInterface {
  SerialBLEInterface _ble;
  ArduinoSerialInterface _usb;
  uint8_t _ble_buf[MAX_FRAME_SIZE];
  uint8_t _usb_buf[MAX_FRAME_SIZE];
  bool _ble_enabled;
  bool _ble_was_connected;
  bool _docked_quiet;
  bool _last_frame_from_ble;

public:
  DualSerialInterface() : _ble_enabled(false), _ble_was_connected(false),
      _docked_quiet(false), _last_frame_from_ble(false) {}

  void begin(const char* ble_prefix, char* node_name, uint32_t pin_code, Stream& usb_stream) {
    _ble.begin(ble_prefix, node_name, pin_code);
    _usb.begin(usb_stream);
    _usb.enable();  // USB is always on
  }

  void enable() override  { _ble.enable(); _ble_enabled = true; _docked_quiet = false; }
  void disable() override { _ble.disable(); _ble_enabled = false; _docked_quiet = false; }
  // Reports the user's intent (UI Bluetooth toggle), not the dock-quiet state.
  bool isEnabled() const override { return _ble_enabled; }

  // Only frames received over the MITM-paired BLE link are authenticated;
  // frames received over USB are unauthenticated (physical presence only).
  bool isConnectionSecure() const override { return _last_frame_from_ble; }

  // Always true — USB is always available as fallback, so the mesh can send.
  bool isConnected() const override { return true; }
  // True only when a BLE companion app is paired and connected.
  bool isBLEConnected() const override { return _ble_enabled && _ble.isConnected(); }
  // A companion app is actually connected if BLE is bonded OR a host holds the
  // USB-CDC port open. (bool)Serial == tud_cdc_n_connected() (DTR asserted) — a
  // serial monitor counts too, but charging-only (no host) does not.
  bool isClientConnected() const override { return isBLEConnected() || (bool)Serial; }

  bool isWriteBusy() const override {
    return (_ble_enabled && _ble.isConnected()) ? _ble.isWriteBusy() : _usb.isWriteBusy();
  }

  size_t writeFrame(const uint8_t src[], size_t len) override {
    return (_ble_enabled && _ble.isConnected()) ? _ble.writeFrame(src, len) : _usb.writeFrame(src, len);
  }

  size_t checkRecvFrame(uint8_t dest[]) override {
    if (_ble_enabled) {
      size_t ble_len = _ble.checkRecvFrame(_ble_buf);
      bool ble_now = _ble.isConnected();

      if (ble_now) {
        _ble_was_connected = true;
        if (ble_len > 0) { memcpy(dest, _ble_buf, ble_len); _last_frame_from_ble = true; return ble_len; }
        return 0;  // BLE active — don't read USB to keep its state machine clean
      }

      if (_ble_was_connected) {
        // BLE just disconnected — reset USB state machine so stale partial frames don't block it
        _ble_was_connected = false;
        _usb.enable();
        return 0;
      }

      // Dock behaviour: while a USB host holds the CDC port open (card sitting in
      // a dock), stop BLE advertising so the node stays quiet on 2.4 GHz and the
      // app talks over USB; resume advertising once undocked. Only advertising is
      // gated, and only while no BLE link exists — an established BLE connection
      // is never force-dropped (SerialBLEInterface::disable() would disconnect).
      bool docked = (bool)Serial;
      if (docked && !_docked_quiet) {
        _ble.disable();
        _docked_quiet = true;
      } else if (!docked && _docked_quiet) {
        _ble.enable();
        _docked_quiet = false;
      }
    }

    size_t usb_len = _usb.checkRecvFrame(_usb_buf);
    if (usb_len > 0) { memcpy(dest, _usb_buf, usb_len); _last_frame_from_ble = false; return usb_len; }
    return 0;
  }
};
