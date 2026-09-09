# Contributing to GhostHID

The board is a Waveshare ESP32-S3-LCD-1.47 on a USB-A stick. It presents a
composite USB keyboard, relative mouse, and absolute pointer to the target, and
relays input over Wi-Fi. This guide covers building, flashing, testing, and the
code layout. Design notes and open problems live in [PLAN.md](PLAN.md).

## Build

The toolchain runs in a container, so nothing is installed on your machine. You
need Docker and `make`.

```bash
make build      # compile (first run downloads the toolchain, ~6 min)
make help       # all targets
```

Output is a single flashable image at
`firmware/.pio/build/esp32-s3-lcd147/ghosthid-merged.bin`.

## Flash from source

Flashing runs on the host, because Docker Desktop on macOS cannot pass a USB
serial port into a Linux container.

A blank board may need to be put into the ROM bootloader by hand the first time:

> Hold **BOOT** then tap **RESET** (or plug the cable in), then release **BOOT**.

```bash
make ports                                # find the port
make flash PORT=/dev/cu.usbmodemXXX
make monitor PORT=/dev/cu.usbmodemXXX     # USB CDC console, 115200
```

After flashing, unplug and replug. From then on the board enumerates on its own
as a keyboard, a mouse, and (when not sealed) a serial console on the one cable.

### First-time setup over serial

The quickest way to configure a fresh device is the cable you just flashed it
with, with no need to join its access point first:

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
password are fine. Settings save immediately and apply on reboot. `show` reports
whether each password is set but never prints one, and `reset` erases everything
back to the build-time defaults.

### Configuration

Settings live in **NVS on the device**, not in the firmware image, and are
edited from the web UI's **Settings** tab:

* Wi-Fi station SSID and password (blank SSID disables station mode)
* AP password (WPA2, 8 to 63 characters)
* Pairing token
* Device name, which sets both the AP SSID and the mDNS hostname

They survive a reflash, because writing the app image does not touch the NVS
partition. Passwords are **write-only over the API**: the device reports whether
each is set, never its value.

The build flags are only **first-boot defaults**, used until something is stored
in NVS:

```bash
make build WIFI_SSID="YourNet" WIFI_PASS="…" AP_PASS="…" TOKEN="…"
```

A device flashed with no flags brings up `ghosthid-XXXX` / `ghosthid-setup`, and
you configure it from there.

## Updating over the air

After the first USB flash, updates go over the network:

```bash
make build
make ota IP=192.168.7.113 TOKEN=your-token
```

Or drag `firmware.bin` into the web UI's Settings tab.

If you call the endpoint by hand, **send `Content-Type: application/octet-stream`**.
Without it curl defaults to `x-www-form-urlencoded`, and the server tries to
parse the whole image as form fields, runs out of memory, and resets the
connection, which looks like a network fault rather than the header mistake it
is.

Upload **`firmware.bin`**, not `ghosthid-merged.bin`: the merged image includes
the bootloader and partition table at absolute offsets and is for USB flashing
only. Both the browser and the device check the image's magic byte and reject
the wrong one.

The partition table is dual-slot (`app0` / `app1` + `otadata`). The incoming
image is written to the inactive slot, and `otadata` only switches once it
validates, so a dropped link or a power cut mid-upload leaves the running
firmware untouched. An image that boots but breaks networking still needs USB
recovery; there is no automatic rollback yet (see PLAN.md "Known Gaps").

## Testing

Host unit tests run without a board (Unity + a recording HID stub):

```bash
make test        # or: pio test -e native
```

They cover the transport-agnostic logic: command processing, the keymap, config,
the Deskflow dispatch, the crypto (HMAC / SHA-256), sealed mode, and the
authenticated-input path.

## Stuck-key safety

A modifier stuck down on the target is the worst failure this device can
produce, so there are three independent layers:

1. **Clean disconnect** releases everything immediately on WebSocket close.
2. **Watchdog** releases everything if a controller holds input then goes silent
   for 750 ms. The client pings every 250 ms to stay clear of this.
3. **BOOT button** is a physical panic release that needs no network at all.

## Memory

With the screen client connected over TLS the largest contiguous heap block sits
around 13 to 15 KB, which is fine in practice: the web UI serves its full page in
under 90 ms, OTA updates succeed with a session live, and no HID reports are
refused. The TLS I/O buffers are reserved at boot while the heap is whole, and
the update endpoint disconnects the screen client first rather than running out
midway.

## CI

CI workflows live in `.github/workflows/`, shared by both hosts: Gitea and
GitHub each run only the workflows meant for them (a `github.server_url`
guard on every job skips the others), so there is no separate `.gitea/`
folder to keep in sync.

* **`build.yml`** runs on every push and pull request: the firmware builds, and
  the embedded web UI is checked to parse as JavaScript, balance its tags, and
  still fit the flash budget (the page lives inside the firmware image, so a
  runaway UI eats the headroom OTA depends on). It installs PlatformIO directly
  rather than nesting the Docker build inside a runner.
* **`release.yml`** fires on a `v*` tag: it builds, packages each target into a
  zip, signs with minisign, and creates a release with the matching
  `CHANGELOG.md` section as the notes.

Build the same bundle locally with `make dist`.

## Porting

Board specifics are confined to
[`firmware/include/board_config.h`](firmware/include/board_config.h) and the
`[env:...]` section of [`firmware/platformio.ini`](firmware/platformio.ini), and
`HidDevice` is the only code that touches USB, so another native-USB ESP32 should
need a board config and a platformio env rather than changes elsewhere. That is
untested; there is no second target to prove it against.

## Layout

```
firmware/
  include/board_config.h      board/HAL configuration + first-boot defaults
  src/config/                 NVS settings and the serial setup console
  src/hid/                    the only code that touches USB
  src/net/                    Wi-Fi, WebSocket, web UI, OTA, Deskflow client
  src/protocol/               transport-agnostic command handling
  src/crypto/                 self-contained SHA-256 / HMAC
  src/ui/                     the on-board LCD
  src/main.cpp                wiring
  web/index.html              the control UI, embedded into the image
  test/test_native/           host unit tests
scripts/                      packaging and release helpers
site/                         marketing site + browser flasher (ghosthid.app)
```

`HidDevice` is the seam the whole design rests on: transports call into it and
never touch USB, and it owns the held-key state that makes "release everything
on disconnect" possible.
