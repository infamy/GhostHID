# GhostHID 👻

## Project Overview

GhostHID is an open-source wireless USB HID bridge.

The goal is to turn an **ESP32-S2 Key V1** into a tiny device that plugs into a target computer's USB port and appears to that computer as a normal USB keyboard and mouse.

A separate computer sends keyboard/mouse commands to the GhostHID device over Wi-Fi.

The target computer should require **no software, drivers, agents, or network connection**.

### Core concept

```text
┌─────────────────────┐
│ Controller Computer │
│                     │
│ Keyboard / Mouse    │
│        │            │
│        ▼            │
│ GhostHID Client     │
└──────────┬──────────┘
           │
           │ Wi-Fi
           ▼
┌─────────────────────┐
│ GhostHID            │
│ ESP32-S2 Key V1     │
│                     │
│ Wi-Fi receiver      │
│        │            │
│        ▼            │
│ USB HID             │
└──────────┬──────────┘
           │ USB
           ▼
┌─────────────────────┐
│ Target Computer     │
│                     │
│ Sees normal USB     │
│ keyboard + mouse    │
└─────────────────────┘
```

The initial project should focus exclusively on **remote keyboard and mouse input**.

Do NOT initially attempt to implement video capture, remote desktop, mass storage, USB hub functionality, or a complete KVM.

---

# Hardware

Initial target hardware:

**ESP32-S2 Key V1**

The firmware should be designed so that it can eventually support other ESP32-S2/S3 boards with native USB.

The ESP32-S2 is important because it supports USB device functionality and can present itself as USB HID.

Potential future hardware:

* ESP32-S3
* Custom ESP32-S3 GhostHID dongle
* ESP32-S2/S3 boards with USB-A male connector
* Possibly other native-USB MCUs

Do not make the firmware unnecessarily dependent on the physical ESP32-S2 Key board.

Create a hardware abstraction/configuration layer where practical.

---

# MVP Requirements

## 1. USB HID

GhostHID must enumerate as a standard USB HID device.

At minimum:

### Keyboard

Support:

* A-Z
* 0-9
* punctuation
* Enter
* Escape
* Backspace
* Tab
* Space
* Arrow keys
* Home
* End
* Page Up
* Page Down
* Insert
* Delete
* Function keys F1-F12
* Shift
* Ctrl
* Alt
* GUI/Windows/Command key

The HID implementation should support modifier keys correctly.

### Mouse

Support:

* X movement
* Y movement
* Left button
* Right button
* Middle button
* Mouse wheel
* Horizontal wheel if practical

Relative movement is sufficient to *prove* the mouse works, and is what
Phase 1 and Phase 3 implement.

**IMPLEMENTED.** Report ID 7, a second pointer collection alongside the
relative mouse, 16-bit axes over 0..32767. The protocol takes fractions
(`{"type":"mouse_abs","x":0.62,"y":0.31}`) rather than pixels, because the
firmware cannot learn the target's resolution and a fraction survives a
resolution change. Buttons are mirrored from the relative mouse's held mask so
the two collections never disagree.

Originally deferred; promoted because three separate features turned out to be
blocked on it — usable pointing from a phone, edge-crossing, and any
Deskflow-style client whose wire protocol carries absolute coordinates. The
reasoning that drove it:

* Relative-only pointing over a lossy Wi-Fi link is genuinely unpleasant to
  use. Dropped or delayed packets accumulate as *permanent* pointer drift,
  because there is no absolute reference to resync against. With absolute
  positioning a lost packet is corrected by the very next one.
* A browser or Python controller knows the pointer position it wants
  (e.g. "the mouse is at 62% across, 31% down"). Translating that into a
  stream of relative deltas and hoping the target's pointer acceleration
  curve agrees is a losing game. Pointer acceleration silently rescales
  relative deltas; it does not touch absolute reports.

Cost is low: a second HID report descriptor (Usage Page Desktop, Usage
Pointer, absolute X/Y as 16-bit logical 0..32767). Keep BOTH reports in the
descriptor — relative for "nudge the pointer" and trackpad-style gestures,
absolute for "put the pointer here".

Caveat to verify on hardware: some KVM switches and a few BIOS
implementations handle absolute-pointer devices poorly. This is why relative
stays in the descriptor rather than being replaced.

---

# 2. Wireless Transport

Initial transport should be Wi-Fi.

The ESP32 should support two possible operating modes.

## Mode A — Access Point

GhostHID creates its own Wi-Fi network.

Example:

```text
SSID: GhostHID-XXXX
```

The controller computer connects directly to GhostHID.

This should be the default/simple mode.

Advantages:

* No existing Wi-Fi required
* Works in isolated environments
* Easy initial setup
* Useful for BIOS/offline machines
* Predictable networking

## Mode B — Station Mode

GhostHID connects to an existing Wi-Fi network.

The controller connects to the ESP32 over the LAN.

This should be implemented after AP mode is working.

---

# 3. Command Protocol

Use a persistent connection rather than individual HTTP requests for keyboard/mouse events.

Preferred initial protocol:

**WebSocket**

Example conceptual messages:

```json
{
  "type": "key",
  "key": "A",
  "pressed": true
}
```

```json
{
  "type": "key",
  "key": "A",
  "pressed": false
}
```

```json
{
  "type": "mouse_move",
  "dx": 12,
  "dy": -4
}
```

```json
{
  "type": "mouse_button",
  "button": "left",
  "pressed": true
}
```

```json
{
  "type": "mouse_button",
  "button": "left",
  "pressed": false
}
```

```json
{
  "type": "mouse_wheel",
  "delta": -1
}
```

Note: one USB HID relative-mouse report carries a **signed 8-bit** delta, so
any `dx`/`dy` outside -127..127 must be split across multiple reports by the
firmware. The protocol layer should accept full-range integers and let the HID
layer do the chunking (`HidDevice::mouseMove` already does this) — otherwise
large movements silently wrap.

The actual wire protocol does not necessarily need to use JSON.

Consider a compact binary protocol once the functionality is proven.

The architecture should make it possible to replace JSON/WebSocket with a lower-latency protocol later.

---

# 4. Controller Client

Create a simple reference client.

Preferred first implementation:

**Python**

The Python client should be able to:

* Discover/connect to GhostHID
* Connect over WebSocket
* Send keyboard events
* Send mouse movement
* Send mouse buttons
* Send mouse wheel
* Send text
* Display connection status
* Reconnect automatically

Example:

```bash
ghosthid --host 192.168.4.1
```

Eventually support:

```bash
ghosthid type "Hello world"
ghosthid key CTRL+ALT+DELETE
```

But the first client can simply provide a Python API.

Example:

```python
ghosthid.key_down("CTRL")
ghosthid.key_down("ALT")
ghosthid.key_down("DELETE")

ghosthid.key_up("DELETE")
ghosthid.key_up("ALT")
ghosthid.key_up("CTRL")
```

And:

```python
ghosthid.mouse_move(100, -20)
ghosthid.mouse_button("left", True)
ghosthid.mouse_button("left", False)
```

---

# 5. Browser Interface

After the basic Python client works, add a small web UI served by the ESP32.

Opening:

```text
http://ghosthid.local/
```

should provide a basic control interface.

The web UI should include:

* Connection status
* Keyboard buttons
* Text input
* Mouse control area
* Mouse buttons
* Mouse wheel
* Reboot device
* Wi-Fi configuration

Do not spend significant time making the UI beautiful initially.

Functionality first.

---

# 6. Text Input

Add a high-level "type text" command.

For example:

```json
{
  "type": "text",
  "text": "Hello from GhostHID"
}
```

GhostHID converts the text into the appropriate HID keyboard events.

Important:

Do not assume every Unicode character can be represented by a standard USB keyboard.

Initially support:

* ASCII
* US keyboard layout

Later consider:

* keyboard layouts
* Unicode strategies
* international keyboards

---

# 7. Connection / Input Safety

GhostHID needs protection against stuck keys.

This is important.

If the controller disconnects while Ctrl or Shift is held down, GhostHID should automatically release all currently held keys/buttons.

Implement:

```text
WebSocket disconnect
        ↓
release all keys
release all mouse buttons
reset mouse state
```

Also implement a watchdog/timeout.

If no valid controller heartbeat has been received for some configurable period, release all inputs.

Example:

```text
heartbeat timeout = 2 seconds
```

The exact timeout should be configurable.

---

# 8. Authentication

Do not initially implement complicated cryptography.

For MVP, support a configurable pairing/password mechanism.

At minimum:

```text
GhostHID-AB12
Password: ********
```

The device should not accept arbitrary remote HID commands from anyone who happens to connect to the network.

Design the protocol so stronger authentication/encryption can be added later.

Potential future approach:

* Device-generated identity
* Pairing code
* Public/private keys
* TLS
* Authenticated WebSocket

---

# 9. Device Discovery

Initially support:

* mDNS
* `ghosthid.local`

Device name should be configurable.

Example:

```text
ghosthid-7A31.local
```

If mDNS is unreliable on a particular network, IP address should still work.

---

# 10. USB Enumeration

The target machine should see GhostHID as a normal HID device.

Prefer a composite HID device containing:

```text
USB
 ├── HID Keyboard
 └── HID Mouse
```

Avoid requiring custom drivers.

Test on:

* Windows
* Linux
* macOS

Also test during early boot/BIOS where possible.

The important property is:

> The target computer must not need GhostHID software.

---

# Development Phases

## Phase 1 — USB HID Proof of Concept — **IMPLEMENTED**

Goal:

Plug ESP32-S2 Key V1 into a computer.

Firmware automatically:

1. Enumerates as a composite USB keyboard **and mouse**
2. Sends a test key
3. Sends test mouse movement

Mouse is **not** optional here. Enumerating both interfaces from day one is
what pins down the composite descriptor, and a composite device is the part
most likely to behave differently across Windows/Linux/macOS/BIOS. Discovering
that after the networking is built would mean re-testing everything.

Build and flash:

```bash
make build              # compiles in a container; no toolchain on your machine
make ports              # find the board
make flash PORT=/dev/cu.usbmodemXXX
make monitor PORT=/dev/cu.usbmodemXXX
```

Success criteria:

```text
ESP32-S2 → USB → PC

PC recognizes:
USB Keyboard
USB Mouse
```

No networking yet.

---

## Phase 2 — Wi-Fi Command Receiver — **IMPLEMENTED** (AP path untested)

Implement:

* Wi-Fi AP
* WebSocket server
* Basic keyboard commands

Test:

```text
Laptop
   ↓ Wi-Fi
ESP32-S2
   ↓ USB
PC
```

From the laptop:

```text
send "hello"
```

The target PC should receive:

```text
hello
```

---

## Phase 3 — Mouse — **IMPLEMENTED**, relative and absolute

Add:

* Relative X/Y
* Left/right/middle buttons
* Wheel

Test latency and reliability.

---

## Phase 4 — Python Controller — **DROPPED**

The browser UI arrived first and covers the same ground with nothing to install,
so a Python client was redundant. The protocol is documented on the device for
anyone who wants to script against it.

Build a reusable Python client library.

Example:

```python
from ghosthid import GhostHID

g = GhostHID("192.168.4.1")

g.connect()

g.type("Hello world")
g.key("ENTER")
g.mouse_move(100, 50)
g.click("left")
```

---

## Phase 5 — Browser UI — **IMPLEMENTED** (served from flash; adds settings, API docs, OTA)

Add a lightweight web interface.

The ESP32 itself serves the UI.

Use WebSocket for real-time control.

---

## Phase 6 — Station Mode — **IMPLEMENTED**, concurrent with the AP

Add connection to an existing Wi-Fi network.

Configuration should be available through the web interface.

---

## Phase 7 — Pairing & Security — **PARTIAL**

Implement:

* pairing
* authentication
* device identity
* secure command channel

Do not compromise the simple setup experience.

---

## Phase 8 — Performance — **MEASURED AND TUNED**

Browser-to-device WebSocket round trip, measured over 150 samples:

```text
min 4.3   median 7.5   p90 24.0   p99 91.2   max 121.8 ms
throughput: 1.08 ms per input event (~930 events/sec)
```

What actually mattered, in order:

1. **Wi-Fi modem sleep.** The radio only woke on DTIM beacons, so an event could
   wait ~100ms for the next wake. `WiFi.setSleep(false)` took the average round
   trip from 36.9ms to 23.8ms. The single largest win.
2. **A 2ms delay after every HID report, which was pure waste.**
   `USBHID::SendReport` already blocks on a semaphore released by
   `tud_hid_report_complete_cb`, so the USB stack provides its own backpressure
   and cannot be outrun. That delay cost 2ms per keystroke and per chunk of a
   long mouse move, for nothing.
3. **TCP_NODELAY on WebSocket clients.** Nagle batches small writes, which is
   exactly wrong for a stream of tiny input events.
4. **Coalescing pointer motion to one message per animation frame.** A trackpad
   fires at 120Hz; sending one frame per event flooded the link and queued fresh
   input behind stale positions.

What did NOT matter, recorded so it is not re-attempted: **shutting the access
point down while the station is connected.** The ~155ms spikes recurring every
~500ms looked exactly like AP/STA airtime contention, so this was implemented
and measured - spikes got *worse* (5.0% -> 7.5% of samples over 60ms). A
controlled ping of the router over the same link then showed the same ~150ms
tail (max 143ms, sd 37.7) as the device (max 151ms, sd 37.3). The spikes are the
Wi-Fi environment between controller and access point, not this device, which
adds only ~5ms over the router baseline. The AP-fallback mode was kept as an
option but is no longer the default.

The lesson worth keeping: a plausible mechanism that fits the observed
periodicity is not evidence. The controlled comparison took two minutes and
overturned it.

Measure:

* keyboard latency
* mouse latency
* packet loss
* reconnect time
* USB HID reliability

Optimize the transport.

If JSON/WebSocket becomes a bottleneck, introduce a compact binary protocol while keeping WebSocket as the transport.

---

---

# Design Decisions

Recorded so they are not silently re-litigated later.

## Toolchain: Arduino (arduino-esp32) via PlatformIO, built in Docker

The original plan never named a framework. Evaluated:

| Option | Verdict |
|---|---|
| **Arduino / arduino-esp32 + PlatformIO** | **Chosen.** `USBHIDKeyboard` / `USBHIDMouse` sit on TinyUSB and work today. Mature Wi-Fi + WebSocket libraries for later phases. |
| ESP-IDF + TinyUSB directly | Viable, more control over the HID descriptor. Worth revisiting *if* the absolute-pointer or boot-protocol work outgrows the Arduino wrappers. |
| Embedded Rust (`esp-hal`) | Rejected for now. The S2 is Xtensa, so it needs `espup`'s forked Rust/LLVM — *more* toolchain, not less. USB-device support on S2 is the least-trodden path; documented examples are overwhelmingly ESP-IDF/TinyUSB. That is the "reinvent TinyUSB" trap this plan warns against. |
| TinyGo | Rejected. Its USB device stack covers RP2040/SAMD/nRF52840; the ESP32-S2 is not a supported target at all, and TinyGo has no driver for the ESP32's own Wi-Fi radio. Would mean writing both a USB stack and a Wi-Fi driver. |

If "modern language, minimal tooling" becomes the priority, the honest answer
is a **hardware** change (RP2350 / Pico 2 W: upstream Rust, first-class USB
HID via embassy, CYW43 Wi-Fi), not a language change on the S2.

All USB-touching code is confined to `firmware/src/hid/HidDevice.{h,cpp}`, so
this decision stays cheap to revisit.

## Build isolation

The toolchain lives in a container (`firmware/Dockerfile`); nothing is
installed on the developer's machine. Flashing runs host-side because Docker
Desktop on macOS cannot pass a USB serial port into a Linux container — so the
build emits a single merged image (`ghosthid-merged.bin`, written at `0x0`)
rather than making the host juggle four binaries and their offsets.

---

# Known Gaps

Open issues in this plan, roughly by importance. Not yet scheduled.

0. **Hosts increasingly block newly-attached HID devices at lock/boot screens.**
   CONFIRMED on hardware: GhostHID enumerated and worked on a logged-in
   desktop, but the target refused it at its boot screen. This is host policy,
   not a firmware bug, and it directly threatens the "no software on the
   target" premise in exactly the situation a KVM is most wanted (a machine
   sitting at a login prompt you cannot otherwise reach).

   Known mechanisms to investigate per platform:
   * macOS 13+ "Allow accessories to connect" - new USB accessories are
     blocked while the machine is locked.
   * Windows - BitLocker pre-boot accepts only a limited set of keyboards;
     Group Policy can restrict USB device installation by class or ID.
   * Linux - USBGuard, and `authorized_default=0` on some hardened builds.

   What to actually test, in order:
   * Does a device already plugged in *before* boot/lock get through, where a
     hot-plugged one does not? If so, "leave it plugged in" is the workaround
     and belongs in the docs as a first-class instruction.
   * Does a boot-protocol keyboard-only descriptor fare better than our
     composite CDC + keyboard + mouse device? This overlaps with gap 1 and is
     the strongest argument for building that variant.
   * Does the VID/PID matter (gap 7)? Some policies allow-list by ID.

   Whatever the answer, the README must state plainly where GhostHID does and
   does not work. Silently failing at a lock screen is the worst outcome.

1. **BIOS/UEFI needs a different device than the one we are building.**
   The plan says "test during early boot/BIOS" but that is in tension with a
   composite CDC + keyboard + mouse device. Many BIOS implementations only
   drive a *boot-protocol* keyboard and ignore anything more complex.
   Likely resolution: a build variant (`-DGHOSTHID_BIOS_MODE`) that enumerates
   as a single boot-protocol keyboard with no CDC and no mouse. Needs to be
   proven on real hardware before the MVP claims BIOS support.

2. **RESOLVED** ~~Heartbeat timeout of 2 s is too slow for its own purpose.~~
   Now 750 ms, with a clean WebSocket close releasing immediately so the timer
   only ever covers abrupt link loss. Original note kept for the reasoning:
   A modifier stuck for two full seconds has already done damage on the target
   (autorepeat, Ctrl-chords). Suggest ~250-500 ms for the watchdog. Note the
   watchdog is only the backstop: a clean TCP/WebSocket close should release
   everything *immediately*, and only an abrupt link loss falls through to the
   timer.

3. **RESOLVED** ~~The AP must be WPA2, not just the app-layer password.~~
   The AP is WPA2 and `Config::setApPassword` refuses anything outside 8-63
   characters rather than silently coming up open. Note kept:
   Section 8 covers authenticating the *controller*, but on an open AP every
   keystroke crosses the air in plaintext for anyone in range, and the
   app-layer password is replayable. WPA2 on the AP is one line of config and
   should be the default, with the passphrase printed/derived per device.

4. **USB suspend is unhandled.** If the target sleeps or the host suspends the
   port while keys are held, GhostHID should release everything on the suspend
   callback. Same failure mode as controller disconnect, different trigger.

5. **Controller-side key capture is harder than the plan implies.**
   Sections 4 and 5 assume the client can read raw key events. In a browser,
   `keydown` gives you a `code` and is fine. In Python it needs OS-level
   hooks with real permission friction (macOS Accessibility grants, Windows
   low-level hooks, X11-vs-Wayland on Linux). Worth scoping before Phase 4,
   or the Python client stays a scripting API rather than a live pass-through.

6. **Boot keyboard is 6-key rollover.** `HidDevice` tracks up to 16 held keys,
   but the wire format tops out at 6 simultaneous non-modifiers. Decide whether
   to accept that or move to an NKRO descriptor (which costs BIOS
   compatibility — see gap 1).

7. **VID/PID are unspecified.** Currently Espressif's `0x303A:0x4004`. Fine for
   development; a shipped device wants a deliberate choice, as some KVMs and
   BIOS whitelist by VID/PID.

8. **Power budget unvalidated.** Wi-Fi AP TX bursts plus USB HID, drawn from
   the target's port. Some KVMs, unpowered hubs and front-panel ports current-
   limit hard enough to brown out the radio. Measure before trusting it.

9. **OTA has no rollback safety net.** Updates are crash-safe (dual-slot: an
   interrupted upload leaves the running image untouched), but an image that
   *boots and then fails* - breaks Wi-Fi, crashes after a minute - is not
   caught, because Arduino does not enable ESP-IDF's `app_rollback`. Recovery
   is a USB reflash. Enabling rollback properly means marking the app valid
   only after the network comes up and a controller connects.

10. **`tests/` exists in the layout with nothing said about it.** The keymap,
   the mouse-delta chunking and the held-key bookkeeping are all pure logic
   and unit-testable on the host, with no board attached. Worth doing — they
   are exactly the parts where a silent bug looks like flaky hardware.

---

# Repository Structure

Suggested structure:

```text
ghosthid/
│
├── firmware/
│   ├── src/
│   ├── include/
│   ├── usb/
│   ├── wifi/
│   ├── protocol/
│   └── web/
│
├── controller/
│   ├── ghosthid/
│   └── examples/
│
├── docs/
│   ├── protocol.md
│   ├── hardware.md
│   ├── setup.md
│   └── architecture.md
│
├── hardware/
│   └── esp32-s2-key/
│
├── tests/
│
├── LICENSE
└── README.md
```

Licensed **MIT** (see `LICENSE`).

Note the compiled firmware statically links LGPL-3.0 libraries
(ESPAsyncWebServer, AsyncTCP) and the LGPL-2.1 arduino-esp32 core. That
obligation attaches to the distributed binary, not to this source. The pinned
Docker build satisfies the LGPL relink requirement by construction; see
`THIRD-PARTY-LICENSES.md`.

---

# Architecture Principles

Keep the system modular.

```text
                 ┌───────────────────┐
                 │ Controller        │
                 │                   │
                 │ Python / Browser  │
                 └─────────┬─────────┘
                           │
                      Transport
                           │
                           ▼
                 ┌───────────────────┐
                 │ Protocol Layer    │
                 └─────────┬─────────┘
                           │
                           ▼
                 ┌───────────────────┐
                 │ HID Abstraction   │
                 └─────────┬─────────┘
                           │
                    ┌──────┴──────┐
                    ▼             ▼
                 Keyboard       Mouse
                    │             │
                    └──────┬──────┘
                           ▼
                       USB HID
```

The protocol should not directly manipulate USB.

The USB layer should not know whether commands came from Wi-Fi, BLE, WebSocket, or another transport.

This will make future transports possible.

---

# Future Ideas

These are NOT MVP requirements.

Potential future features:

### BLE

Allow a phone or computer to connect over BLE.

### ESP-NOW

Use another ESP32 as the controller/transmitter.

```text
ESP32 controller
       │
   ESP-NOW
       │
       ▼
GhostHID
       │
      USB
       ▼
Target PC
```

### Physical keyboard forwarding

Allow a real keyboard connected to a controller ESP32 to be forwarded through GhostHID.

```text
Real keyboard
      │
      ▼
Controller
      │
    Wi-Fi
      │
      ▼
GhostHID
      │
     USB
      ▼
Target PC
```

### Edge-crossing control (Synergy / lan-mouse style)

Move the pointer off the edge of the controller's screen and have it appear on
the target, keyboard following it, then come back at the far edge.

Synergy, Input Leap and lan-mouse all need software on **both** machines.
GhostHID would need none on the target - so it works on a machine you cannot
install on, one sitting at a login screen, or an air-gapped box. That is a
different product, not a clone.

**This is blocked on absolute mouse positioning, and is the strongest argument
for building it.** With relative deltas we are nudging a pointer we cannot see:
the target applies its own pointer acceleration, our idea of the position
drifts within seconds, and we never learn when the pointer reached the far edge
to hand control back. The user cannot be shown where the pointer is either.

Absolute positioning inverts that. We stop trying to observe the pointer and
start dictating it - no acceleration is applied to absolute reports, so it is
1:1 by construction, and we always know where the pointer is because we put it
there. It is self-correcting too: if someone nudges the physical mouse on the
target, the next absolute report re-asserts position rather than compounding an
error. Edge detection becomes arithmetic we already have the inputs for.

```text
controller pointer hits right edge
        -> capture and hide the local cursor
        -> send absolute coordinates, scaled to the target's resolution
virtual pointer reaches the target's left edge
        -> release capture, cursor returns to the controller
```

## Better: be a client of an existing server

Rather than building our own edge-crossing, implement a **Deskflow / Barrier /
Input Leap client**. Those projects already solve the genuinely hard parts -
capturing and suppressing input on the controller, edge detection, multi-monitor
layout, a config UI, cross-platform support. GhostHID would just be another
"screen" that happens to need no software installed on it.

This removes gap 5 entirely: controller-side key capture stops being our
problem.

Protocol comparison, if we pick one:

| Target | Transport | Difficulty |
|---|---|---|
| **Deskflow / Barrier / Input Leap** | plain TCP :24800, documented legacy Synergy 1.x protocol (`DMMV`, `DMDN`, `DKDN`, `CINN`/`COUT`) | **easiest, no crypto** |
| lan-mouse | DTLS over UDP :4242 (WebRTC.rs, self-signed certs, TOFU fingerprints) | hard - DTLS interop, no embedded reference |
| Synergy 3 | new proprietary protocol | avoid |

UDP itself is no obstacle - it is in fact the better transport for input, with
no head-of-line blocking. The cost in lan-mouse's case is DTLS. mbedTLS is
already linked and supports DTLS 1.2, so it is possible; the risk is interop
debugging against WebRTC.rs.

Note a cross-project effort to define a **unified protocol** across Deskflow,
Input Leap, Barrier, Synergy and lan-mouse. If that lands, one implementation
covers all of them - an argument against over-investing in the legacy Barrier
wire format now.

Side benefit if the lan-mouse path were ever taken: the same mbedTLS DTLS work
would give GhostHID's own protocol an encrypted transport, closing the
cleartext-keystrokes problem.

Requires:

* absolute HID report descriptor (see MVP Requirements / Mouse). `DMMV` carries
  absolute screen coordinates, so this is a hard prerequisite, not a nicety -
  the third separate feature to be blocked on it
* a keysym -> HID usage mapping table; protocol key events are X11 keysym-based
* our advertised screen size configured to match the target's resolution
* the target's screen resolution as a configured value - we cannot query it
* controller-side input capture and suppression, which is gap 5: macOS needs
  Accessibility permission and a CGEventTap, and Wayland has no portable path
* an unambiguous escape hatch, so a controller that loses the link cannot leave
  the user with no cursor on either machine

Out of scope regardless, because all of it needs an agent on the target:
clipboard sharing, drag-and-drop between machines, and discovering the target's
real resolution or monitor layout.

### Multiple targets

One controller could manage several GhostHID devices.

### Macro support

Store and execute sequences such as:

```text
CTRL+ALT+T
type("ssh server")
ENTER
```

### Remote KVM integration

GhostHID could eventually become the input component of a complete open-source KVM.

```text
             ┌─────────────┐
             │ Controller  │
             └──────┬──────┘
                    │
             ┌──────┴──────┐
             │ GhostHID    │
             │             │
             │ Input       │
             │ + Video     │
             └──────┬──────┘
                    │
                 Target
```

But video is explicitly outside the initial project.

---

# MVP Definition of Done

## Verification status

Separated deliberately from "implemented", because the two diverged badly
during development and the difference is where the bugs were hiding.

**Proven on hardware** (ESP32-S2 + macOS host):

* enumerates as a composite HID keyboard + mouse, descriptor decoded and
  checked (`0x303A:0x4004`, report IDs 1 and 2, 6-key rollover, rel X/Y
  +/-127, wheel, AC Pan)
* typed keystrokes reached the target OS
* joins an existing network in station mode, with the AP up concurrently
* serves the web UI and answers WebSocket auth / ping / error paths

* settings persist across a reflash - verified by writing a marker, flashing,
  and reading it back
* the serial setup console, and station mode joining a real network
* over-the-air update end to end: 820KB in 11.6s, plus both rejection paths
  (wrong image, bad token) returning proper errors
* absolute positioning enumerates - report ID 7, `81 02` (Absolute), 16-bit
  axes over 0..32767, alongside the relative pointer's `81 06` / +/-127 - and
  **moves the pointer on a real target**, confirmed by hand
* **Windows and Linux targets both accept the device and respond to input.**
  That closes definition-of-done item 14, and means the composite descriptor
  works across all three desktop platforms rather than only the one it was
  developed against
* four separate boards flashed from blank, each taking a distinct MAC-derived
  identity (BA98, BF78, C0E4, C3BC), so several can share a network

**Implemented but NOT yet verified on hardware** - do not claim these work:

* release-on-disconnect and the 750 ms watchdog actually firing. This is the
  safety property the whole design rests on and it has never once been observed
  working, which makes it the most conspicuous gap left
* a controller associating with the device's own AP (Mode A) rather than over a
  joined network
* anything on iOS - the touch UI and sticky modifiers are untried on real
  hardware

GhostHID v0.1 is complete when:

1. ESP32-S2 Key V1 plugs into a target computer.
2. Target computer recognizes it as a USB keyboard.
3. Target computer recognizes it as a USB mouse.
4. ESP32-S2 creates a Wi-Fi AP.
5. A laptop connects directly to that AP.
6. A Python client connects to the ESP32.
7. Python can send keyboard presses.
8. Python can send text.
9. Python can move the mouse.
10. Python can click mouse buttons.
11. Python can scroll.
12. Disconnecting the controller automatically releases all held keys/buttons.
13. No software is installed on the target computer.
14. Basic operation works on Windows and Linux. **DONE**
15. The project can be flashed onto another ESP32-S2/S3 with minimal configuration changes.

---

# Important Development Rule

Before writing substantial amounts of code, inspect the existing ESP32-S2 USB HID libraries/examples and determine the most reliable native USB implementation for the ESP32-S2.

Do not reinvent TinyUSB/HID functionality unless necessary.

Likewise, investigate existing open-source ESP32 Wi-Fi HID projects and reuse compatible components where their licenses permit it.

The goal is to build a clean, maintainable project rather than unnecessarily writing USB HID infrastructure from scratch.

---

# Project Philosophy

GhostHID should feel like an appliance.

Plug it in.

Connect to it.

Control the machine.

The target machine should simply think:

> "Someone plugged in a keyboard and mouse."

No agent.
No drivers.
No account.
No software on the target.

**GhostHID — The keyboard that isn't there.** 👻

