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

### First-time setup over serial

The quickest way to configure a fresh device is the cable you just flashed it
with — no need to join its access point first:

```bash
make monitor PORT=/dev/cu.usbmodemXXX
```

```
> help
> wifi MyNetwork
> wifipass hunter2
> token something-private
> appass a-good-ap-password
> show
> reboot
```

Values are taken verbatim to the end of the line, so spaces in an SSID or
password are fine. Settings save immediately and apply on reboot. `show`
reports whether each password is set but never prints one, and `reset` erases
everything back to the build-time defaults.

### Configuration

Settings live in **NVS on the device**, not in the firmware image, and are
edited from the web UI's **Settings** tab:

* Wi-Fi station SSID + password (blank SSID disables station mode)
* AP password (WPA2, 8-63 characters)
* Pairing token
* Device name, used for mDNS

They survive a reflash, because writing the app image does not touch the NVS
partition. Changes apply on reboot; there is a Reboot button next to Save.

Passwords are **write-only over the API**: the device reports whether each is
set, never its value. Otherwise anyone holding the token could read your Wi-Fi
password out of the device.

The device's own AP always comes up, whatever the station settings say, so a
mistyped SSID can never lock you out — join `GhostHID-XXXX` and fix it.

The build flags are now only **first-boot defaults**, used until something is
stored in NVS:

```bash
make build WIFI_SSID="YourNet" WIFI_PASS="…" AP_PASS="…" TOKEN="…"
```

Handy for flashing a device that should come up already on your network, but
not required — a device flashed with no flags at all brings up
`GhostHID-XXXX` / `ghosthid-setup`, and you configure it from there.

## Updating over the air

After the first USB flash, updates go over the network:

```bash
make build
make ota IP=192.168.7.113 TOKEN=your-token
```

Or drag `firmware.bin` into the web UI's Settings tab.

Upload **`firmware.bin`**, not `ghosthid-merged.bin` — the merged image includes
the bootloader and partition table at absolute offsets and is for USB flashing
only. Both the browser and the device check the image's magic byte and reject
the wrong one, so the mistake costs you an error message rather than a brick.

Why it is safe to interrupt: the partition table is dual-slot
(`app0`/`app1` + `otadata`). The incoming image is written to the *inactive*
slot and `otadata` only switches once it validates, so a dropped Wi-Fi link or
a power cut mid-upload leaves the running firmware untouched. Retry and nothing
is lost.

**The endpoint requires the pairing token.** It installs arbitrary code on a
device that types into your computer, so leaving the token at its default while
the device sits on a shared network is a genuinely bad idea.

Not covered: an image that boots but breaks networking still needs USB
recovery. Arduino does not enable ESP-IDF's rollback, so there is no automatic
revert. See PLAN.md "Known Gaps".

## Using it from a phone

The web UI is built for touch. On iOS the software keyboard does not deliver
usable `keydown`/`keyup` events (it reports `keyCode 229`) and has no Ctrl, Alt
or Esc at all, so a pure key-capture approach cannot work there. Instead:

* **Live typing** field — opens the native keyboard and forwards each character
  as you type, via `beforeinput` rather than key events.
* **Sticky modifiers** — tap `Ctrl`, then press `c`, to send Ctrl+C. Double-tap
  a modifier to lock it, tap again to clear.
* **Trackpad** — drag to move, tap to click, two-finger drag to scroll.
* Buttons are sized to Apple's 44px touch target, and inputs use 16px text so
  iOS does not zoom when they take focus.

The **API** tab documents the full protocol on the device itself, so anyone
who can reach the page can write a client without this repo.

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
  include/board_config.h    board/HAL configuration + first-boot defaults
  src/config/Config.{h,cpp} NVS-backed runtime settings
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

GhostHID is [MIT licensed](LICENSE).

Note that the *compiled firmware* statically links LGPL-3.0 libraries
(ESPAsyncWebServer, AsyncTCP) and the LGPL-2.1 arduino-esp32 core. If you
redistribute a built `.bin`, see [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md)
for what that obliges you to do — in short, ship it with a pointer to the
source, which the pinned Docker build already makes sufficient.
