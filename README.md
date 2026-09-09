<img src="assets/logo.svg" alt="GhostHID" width="290">

**Locked out of a machine that won't let you install anything?** Staring at a
login screen with no keyboard in reach? Babysitting a box that will never run an
agent?

**Meet GhostHID.**

Plug a thumb-sized board into any computer's USB port. That machine sees a
perfectly ordinary keyboard and mouse: no drivers, no agent, no account, nothing
installed. Then type on it from your phone, from across the room, or from across
the network.

**GhostHID. The keyboard that isn't there.**

---

An ESP32-S3 on a USB-A stick presents itself to the target as an ordinary
keyboard, mouse, and absolute pointer, and relays input sent to it over Wi-Fi.
The target needs no software, drivers, agents, or network access of its own. You
drive it from a phone or browser, or add it to a Deskflow / Barrier / Input Leap
setup as a screen and just move your pointer onto it.

**Status:** v0.9.0, working end to end.

**Supported hardware:** one board for now, the
[**Waveshare ESP32-S3-LCD-1.47**](https://www.waveshare.com/esp32-s3-lcd-1.47.htm),
a USB-A stick with a 1.47-inch colour LCD. That is the board the release build
and `ghosthid.app` target. Other native-USB ESP32-S3 boards are a porting
exercise (see [CONTRIBUTING.md](CONTRIBUTING.md#porting)), not a supported build.

## Flash it

The easiest way is the browser flasher at **[ghosthid.app](https://ghosthid.app)**
(desktop Chrome or Edge, over USB). It verifies the chip, checks the firmware
signature, and writes it in about a minute. It flashes the supported board, the
Waveshare ESP32-S3-LCD-1.47. Building and flashing from source is covered in
[CONTRIBUTING.md](CONTRIBUTING.md).

A fresh flash erases stored settings, which is expected for a new device.

## Use it

After flashing, GhostHID brings up its own Wi-Fi access point:

```
SSID:     ghosthid-XXXX        (name + a MAC-derived suffix)
Password: ghosthid-setup       (change it)
```

The on-board screen shows a QR code: scan it to join the AP, then open the
device in a browser. Everything is there: live typing, a key pad, sticky
modifiers, a trackpad with relative and absolute modes, presenter controls,
media keys, and settings. On the Settings tab you set your own Wi-Fi, a pairing
token, and the AP password, so it can live on your network instead of its own.

It is built for touch, so a phone works as well as a laptop. The device's own AP
always comes up regardless of your Wi-Fi settings, so a mistyped SSID can never
lock you out.

The full WebSocket protocol is documented on the device itself under the About
tab, so anyone who can reach the page can write a client.

## Security

GhostHID is a keyboard, so anyone who can drive it can type into a logged-in
session, and it is built accordingly: a per-device random pairing token that
never crosses the wire (challenge-response), authenticated input that can't be
forged or replayed, mutual-TLS screen sessions, a sealed mode that removes the
serial console from the USB bus entirely, and three independent layers that
guarantee a held key is always released. The complete threat model and every
deliberate trade-off are on the [security page](https://ghosthid.app/security/).

## Verifying a release

Release artifacts are signed with [minisign](https://jedisct1.github.io/minisign/).
Each `.zip` and the raw `ghosthid-firmware.bin` ships with a matching `.minisig`.
The public key ships as [`ghosthid.pub`](ghosthid.pub), so from a clone:

```bash
minisign -Vm ghosthid-firmware.bin -p ghosthid.pub
```

A `Signature and comment signature verified` line means the file is authentic
and untampered. The device enforces nothing about signing (it is a
bring-your-own-board project); verification is provenance for the person doing
the flashing.

## Contributing

The firmware is PlatformIO / Arduino-ESP32, the control UI is a single embedded
`index.html`, and the marketing site plus browser flasher live in `site/`. Build
and test with:

```bash
make build      # compile in a container (needs Docker + make)
make test       # host unit tests
```

[CONTRIBUTING.md](CONTRIBUTING.md) has the full build, flash, test, and porting
guide, plus how the code is laid out. Design notes and open problems are in
[PLAN.md](PLAN.md), and the changelog is [CHANGELOG.md](CHANGELOG.md). Issues and
pull requests are welcome. Please report a security finding to the maintainer
privately rather than in a public issue.

## Licence

GhostHID is [MIT licensed](LICENSE).

The *compiled firmware* statically links LGPL-3.0 libraries (ESPAsyncWebServer,
AsyncTCP) and the LGPL-2.1 arduino-esp32 core. If you redistribute a built
`.bin`, see [THIRD-PARTY-LICENSES.md](THIRD-PARTY-LICENSES.md) for what that
obliges: in short, ship it with a pointer to the source.
