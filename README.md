<img src="assets/logo.svg" alt="GhostHID" width="290">

**Locked out of a machine that won't let you install anything?** Staring at a
login screen with no keyboard in reach? Babysitting a box that'll never run an
agent?

**Meet GhostHID.**

Plug a thumb-sized board into any computer's USB port. That machine sees a
perfectly ordinary keyboard and mouse — no drivers, no agent, no account,
nothing installed. Then type on it from your phone, from across the room, or
from across the network.

**GhostHID. The keyboard that isn't there.**

---

An ESP32-S2 presents itself to a target computer as a composite USB keyboard,
relative mouse and absolute pointer, and relays input sent to it over Wi-Fi. The
target needs no software, drivers, agents or network access of its own.

**Status: v0.4.0**, working end to end — joins your network while keeping its own
access point as a fallback, is driven from a browser or a serial console,
updates itself over the air, and can join a Deskflow / Barrier / Input Leap
server as a screen so you simply move the pointer onto it.

Design notes and open problems are in [PLAN.md](PLAN.md); what changed and when
is in [CHANGELOG.md](CHANGELOG.md).

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
SSID:     ghosthid-XXXX        (name + a MAC-derived suffix)
Password: ghosthid-setup       (change it - see below)
Device:   ws://192.168.4.1/ws
Token:    ghosthid
```

Join that network (or reach it on your LAN) and open the address in a browser.
Everything is there: live typing, a key pad, sticky modifiers, a trackpad with
relative and absolute modes, settings and the API reference.

For scripting, the WebSocket protocol is documented on the device itself under
the **About** tab.

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
* Device name — sets both the AP SSID and the mDNS hostname

They survive a reflash, because writing the app image does not touch the NVS
partition. Changes apply on reboot; there is a Reboot button next to Save.

Passwords are **write-only over the API**: the device reports whether each is
set, never its value. Otherwise anyone holding the token could read your Wi-Fi
password out of the device.

The device's own AP always comes up, whatever the station settings say, so a
mistyped SSID can never lock you out — join `ghosthid-XXXX` and fix it.

The build flags are now only **first-boot defaults**, used until something is
stored in NVS:

```bash
make build WIFI_SSID="YourNet" WIFI_PASS="…" AP_PASS="…" TOKEN="…"
```

Handy for flashing a device that should come up already on your network, but
not required — a device flashed with no flags at all brings up
`ghosthid-XXXX` / `ghosthid-setup`, and you configure it from there.

## Updating over the air

After the first USB flash, updates go over the network:

```bash
make build
make ota IP=192.168.7.113 TOKEN=your-token
```

Or drag `firmware.bin` into the web UI's Settings tab.

If you call the endpoint by hand, **send `Content-Type: application/octet-stream`**.
Without it curl defaults to `x-www-form-urlencoded` and the server tries to parse
the whole 800KB image as form fields, runs out of memory and resets the
connection — which looks like a network fault rather than the header mistake it
is.

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

## Lean mode

With the screen client running, a TLS session leaves only ~13KB of contiguous
heap and the web UI becomes sluggish. Lean mode trades the web UI away while the
KVM is in use:

```
> lean on
> reboot
```

The web server is then **never started** while the screen client is enabled, and
the device is managed from the serial console. Not starting it is the only thing
that helps: AsyncTCP never tears its task down, so stopping the server later
frees nothing measurable (92 bytes, measured). Skipping it keeps the largest
block near its 135KB boot value rather than dropping to ~47KB.

You cannot be locked out by this. If the screen client has not connected within
90 seconds of boot — wrong address, server down, network moved — the web server
starts anyway.

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

## CI

Gitea Actions workflows live in `.gitea/workflows/`.

**`build.yml`** runs on every push and pull request:

* the firmware builds
* the embedded web UI parses as JavaScript, its tags balance, and it still fits
  the flash budget — the page lives inside the firmware image, so a runaway UI
  eats the headroom OTA depends on

It installs PlatformIO directly rather than reusing the Docker build. That
image exists to keep a developer's machine clean; nesting it inside a runner
that is already a container buys nothing. The pinned PlatformIO version is kept
in step with `firmware/Dockerfile`.

Each firmware build uploads a downloadable bundle containing both images,
their checksums, and `FLASHING.md` with instructions for a fresh board and for
an over-the-air update. Download it from the run's Artifacts.

CI does not touch any device. Deploying is a deliberate act: it installs code on
a machine that types into someone's computer, and an image that boots but breaks
networking needs physical recovery, since there is no rollback yet.

**`release.yml`** fires when a `v*` tag is pushed. It builds every target,
packages each into a zip, and creates a Gitea Release with the matching
`CHANGELOG.md` section as the notes. It uses the token the runner injects
automatically, so there is nothing to configure.

Build the same bundle locally with:

```bash
make dist        # -> dist/ghosthid-esp32-s2-key/
```

If your runner uses different labels, change `runs-on` in the workflow.

## Porting

The ESP32-S2 is the only target right now. Board specifics are confined to
[`firmware/include/board_config.h`](firmware/include/board_config.h) and the
`[env:...]` section of [`firmware/platformio.ini`](firmware/platformio.ini), and
`HidDevice` is the only code that touches USB — so another native-USB ESP32
should need a board config and a platformio env, not changes elsewhere. That is
untested; there is no second target to prove it against.

## Layout

```
firmware/
  include/board_config.h      board/HAL configuration + first-boot defaults
  src/config/                 NVS settings and the serial setup console
  src/hid/                    the only code that touches USB
  src/net/Network.cpp         Wi-Fi, WebSocket, web UI, OTA
  src/protocol/               transport-agnostic command handling
  src/main.cpp                wiring
  web/index.html              the control UI, embedded into the image
  scripts/                    PlatformIO pre/post build steps
  Dockerfile                  build environment
scripts/                      packaging and release helpers
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
