# Changelog

## 0.3.0

First release that is genuinely usable rather than a proof of concept. Everything
below was verified on hardware unless stated otherwise.

### Added

* **Absolute pointer positioning** (report ID 7, 16-bit axes over 0..32767)
  alongside the existing relative mouse. Relative deltas are rescaled by the
  target's pointer acceleration, so a controller drifts out of sync and can never
  know where the pointer is; absolute reports are not, so the pointer lands
  exactly where asked. This also unblocks edge-crossing and Deskflow-style
  clients, whose protocols carry absolute coordinates.
* **Over-the-air updates.** `POST /api/ota`, the Settings tab, or
  `make ota IP=... TOKEN=...`. Writes land in the spare flash slot and only take
  effect once the image validates, so an interrupted upload leaves the running
  firmware untouched.
* **Runtime configuration in NVS**, replacing compile-time credentials. Editable
  from the web UI or the serial console; survives reflashing. Secrets are
  write-only over the API - the device reports whether a password is set, never
  its value.
* **Serial setup console** on the USB cable you flashed with, so a fresh device
  can be configured without first joining its access point.
* **Station mode**, concurrent with the access point. The AP always comes up, so
  a mistyped SSID cannot lock you out. `ap fallback` drops it while a network is
  joined and restores it if that connection is lost.
* **Web UI** as the only client - touch-friendly, with live typing through the on-screen keyboard,
  sticky modifiers for chords a phone keyboard cannot produce, a trackpad with
  absolute and relative modes, settings, and on-device API documentation.
* **CI** (Gitea Actions) building the firmware, checking the web UI, and
  publishing flashable images with instructions. A `v*` tag also creates a
  Gitea Release with those images attached.

### Changed

* **Input latency**: median round trip 9.6ms -> 7.5ms, p99 155.8ms -> 91.2ms,
  absorbing input at 1.08ms per event. The wins were disabling Wi-Fi modem sleep,
  removing a pointless 2ms delay after every HID report (the USB stack already
  blocks until the host collects it), TCP_NODELAY, and coalescing pointer motion
  to one message per animation frame.
* **Heartbeat timeout 2s -> 750ms.** A modifier stuck for two seconds has already
  autorepeated on the target.
* The access point is WPA2 rather than open, and an invalid passphrase is
  rejected instead of silently bringing the AP up unsecured.

### Fixed

* `make flash` erased stored settings on every write: the merged image spans the
  NVS partition with 0xFF padding. Flashing now writes components at their own
  offsets and preserves settings; `make flash-factory` wipes deliberately.
* HID commands were silently discarded when the target had not enumerated the
  device, which looked identical to success. They now return an explicit error,
  and the UI shows USB state.
* USB identity was ignored: with CDC enabled at boot the USB stack starts before
  `setup()`, so runtime VID/PID calls do nothing. Set at build time instead.
* Mouse deltas beyond +/-127 wrapped; they are now split across multiple reports.
* Serial output was staircased - `printf` emitted a bare LF where `println`
  emits CRLF.

### Known gaps

See `PLAN.md`. The notable ones: hosts increasingly block newly-attached HID
devices at lock and boot screens; there is no OTA rollback, so an image that
boots but breaks networking needs physical recovery; and keystrokes cross the
network in cleartext in station mode.

### Removed

* The Python client. The browser UI covers the same ground without anything to
  install, and the WebSocket protocol is documented on the device itself for
  anyone scripting against it.
* The ESP32-S3 build target. Only the ESP32-S2 is being tested, and an
  unexercised second target is a maintenance cost rather than a portability
  guarantee. The board abstraction that made it cheap is still in place.
