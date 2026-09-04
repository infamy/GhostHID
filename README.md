# GhostHID 👻

Wireless USB HID bridge. An ESP32-S2/S3 plugs into a target computer, appears
as an ordinary USB keyboard and mouse, and relays input sent to it over Wi-Fi.
The target needs no software, drivers, agents or network access.

See [PLAN.md](PLAN.md) for the full design.

**Status: Phase 2 — Wi-Fi command receiver.** Enumerates as a composite USB
keyboard + mouse, serves a WebSocket over its own access point, and is driven
by a Python client.

## Build

The toolchain runs in a container, so nothing is installed on your machine.
You need Docker and `make`.

```bash
make build      # compile (first run downloads the toolchain, ~6 min)
make help       # all targets
```

Output is a single flashable image at `firmware/.pio/build/esp32-s2-key/ghosthid-merged.bin`.

## Flash

Flashing runs on the host: Docker Desktop on macOS cannot pass a USB serial
port into a Linux container.

An ESP32-S2 whose only USB is the native port will **not** enumerate until it
has firmware that brings up USB CDC. On a blank board you must enter the ROM
bootloader by hand:

> Hold **BOOT** (GPIO0) → tap **RESET** (or plug the cable in) → release **BOOT**.

The board then appears as an Espressif device (`0x303A`) and gets a
`/dev/cu.usbmodem*` port.

```bash
make ports                                # find it
make flash PORT=/dev/cu.usbmodemXXX
make monitor PORT=/dev/cu.usbmodemXXX     # USB CDC console, 115200
```

After flashing, unplug and replug. From then on the board enumerates on its
own — as a keyboard, a mouse, and a serial console on the one cable.

## Sending keystrokes

Once flashed, GhostHID brings up its own WPA2 access point:

```
SSID:     GhostHID-XXXX        (XXXX derived from the board's MAC)
Password: ghosthid-setup       (change it - see below)
Device:   ws://192.168.4.1/ws
Token:    ghosthid
```

Join that network from your controller machine, then:

```bash
cd controller
pip install -e .

ghosthid type "hello world"
ghosthid key ENTER
ghosthid key CTRL+ALT+DELETE
ghosthid move 100 50
ghosthid click left
ghosthid scroll 3
```

Or from Python:

```python
from ghosthid import GhostHID

with GhostHID("192.168.4.1", token="ghosthid") as g:
    g.type("hello world")
    g.key("ENTER")
    g.chord("CTRL", "ALT", "DELETE")
    g.mouse_move(100, 50)
    g.click("left")
```

Use the context manager. On exit it releases everything held, so an exception
in your script cannot leave a modifier stuck on the target.

### Changing the credentials

The defaults are for bench use. Override at build time:

```bash
docker run --rm -v "$PWD/firmware":/project -v ghosthid-pio-cache:/pio \
  -e PLATFORMIO_CORE_DIR=/pio ghosthid/build \
  pio run -e esp32-s2-key \
  --project-option='build_flags=-DGHOSTHID_AP_PASSWORD=\"your-wpa2-pass\" -DGHOSTHID_AUTH_TOKEN=\"your-token\"'
```

The AP is WPA2 rather than open on purpose: on an open network every keystroke
crosses the air in cleartext to anyone in range, and the application-layer
token alone is replayable.

## Stuck-key safety

Three independent layers, because a modifier stuck down on the target is the
worst failure this device can produce:

1. **Clean disconnect** — the WebSocket close handler releases everything
   immediately.
2. **Watchdog** — if the controller holds input and then goes silent for
   750 ms (abrupt power loss, out of range, no close event), the device
   releases everything itself. The client pings every 250 ms to stay clear of
   this.
3. **BOOT button** — a physical panic release that needs no network at all.

## Porting

Board specifics live in [`firmware/include/board_config.h`](firmware/include/board_config.h)
and the `[env:...]` sections of [`firmware/platformio.ini`](firmware/platformio.ini).
An ESP32-S3 target is kept building as a portability check:

```bash
make build ENV=esp32-s3
```

## Layout

```
firmware/
  include/board_config.h    board/HAL configuration
  src/hid/HidDevice.{h,cpp} the only code that touches USB
  src/main.cpp              Phase 1 self-test
  scripts/                  PlatformIO post-build (image merge)
  Dockerfile                build environment
controller/                 Python client (Phase 4)
docs/
```

`HidDevice` is the seam the whole design rests on: transports call into it and
never touch USB, and it owns the held-key state that makes "release everything
on disconnect" possible.

## Licence

TBD — intended to be permissive (MIT or Apache-2.0).
