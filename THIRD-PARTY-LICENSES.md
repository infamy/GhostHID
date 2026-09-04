# Third-party licences

GhostHID's own source is MIT (see [LICENSE](LICENSE)). The **compiled
firmware binary** is a different matter: it statically links libraries under
the LGPL, and those terms travel with the binary you distribute.

This file exists so that anyone shipping a built `.bin` knows what they are
obliged to do.

## What is linked into the firmware

| Component | Licence | Notes |
|---|---|---|
| [arduino-esp32](https://github.com/espressif/arduino-esp32) core | LGPL-2.1-or-later | Arduino framework, `USBHIDKeyboard` / `USBHIDMouse` |
| [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) | **LGPL-3.0** | HTTP + WebSocket server |
| [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) | **LGPL-3.0** | async TCP backing the above |
| [ArduinoJson](https://github.com/bblanchon/ArduinoJson) | MIT | protocol parsing |
| [TinyUSB](https://github.com/hathach/tinyusb) (via ESP-IDF) | MIT | USB device stack |
| ESP-IDF components | Apache-2.0 | underlying SDK |

The Python controller in `controller/` links nothing copyleft: its only
dependency is [`websockets`](https://github.com/python-websockets/websockets)
(BSD-3-Clause).

## What that means if you distribute a built binary

LGPL-3.0 §4 requires that a recipient of the combined work be able to relink
it against a **modified** version of the LGPL library. Handing someone only a
flashed `.bin` does not satisfy that.

GhostHID satisfies it by construction, and this is the reason the build is set
up the way it is:

* the complete source is here;
* `firmware/platformio.ini` pins exact dependency versions;
* `firmware/Dockerfile` pins the toolchain, so `make build` reproduces the
  binary on any machine.

Anyone can therefore drop in their own AsyncTCP or ESPAsyncWebServer and
rebuild. **If you redistribute GhostHID firmware, ship it with a pointer to
this repository (or your fork of it) so recipients retain that ability.**

Distributing a `.bin` on its own, with no route to the corresponding source
and build tooling, would not comply.

## Replacing the LGPL parts

If you need a firmware binary with no LGPL components, the two LGPL-3.0
libraries are both replaceable: `ESPAsyncWebServer` + `AsyncTCP` provide the
HTTP/WebSocket transport only, and `Network.cpp` is the sole file that touches
them. ESP-IDF's own `esp_http_server` (Apache-2.0) covers the same ground.
Nothing in the HID or protocol layers depends on either library.
