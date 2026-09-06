# Changelog

## 0.6.25

### Fixed

* **Stuck-key regression from multi-controller (H8)** - a controller that
  disconnected while holding a key left it held on the target when another
  controller was still attached (endSession only released on the last one out,
  and the watchdog rode a shared last-message clock the other client kept warm).
  Any controller's disconnect now releases held input while anything is down - a
  spurious release for the others beats a key stuck on the target, the worst
  failure this device can produce.
* **QR / Wi-Fi page: the code's edge crushed the first character of the SSID and
  password.** The text column is now placed off the QR's measured width plus a
  gap (and the QR is scale 3 to make room); long values drop to the small font
  rather than clipping.
* **Login-gate footgun removed** - dropped the `hidden` attribute that fought the
  gate's inline `display:flex`; visibility is controlled purely by `display`.

### Changed

* Max concurrent controllers is now a single shared constant
  (`GHOSTHID_MAX_CONTROLLERS`) that both the network slot table and the
  per-client auth table derive from, so they can never drift apart.


## 0.6.23

Onboarding, auth, and multi-controller overhaul (consolidates several
same-session iterations after 0.6.16).

### Added

* **Multiple controllers at once** - up to 4 web controllers can connect
  simultaneously, each authenticating independently (no shared auth state, so
  one client's login never authorises another). A 5th gets a clear "device
  full" instead of the old silent refusal. The web UI shows a warning banner
  when more than one is connected and the LCD shows the live count. This retires
  the single-controller slot, whose interaction with the rate-limiter could lock
  the legitimate operator out entirely.
* **Login gate** - the pairing token is prompted for up front (a full-page
  prompt) instead of being buried at the bottom of Settings.
* **Firmware version on the main LCD screen.**

### Fixed

* **Login overlay stayed painted over a connected app** - the gate used the
  `hidden` attribute, but its inline `display:flex` overrode it, so a fully
  authenticated session sat invisible behind the login screen and Connect
  looked like it did nothing. Toggled via `display` now.
* **Auth lockout masqueraded as "wrong token."** During a cooldown the device
  returned a generic failure the UI showed as a bad-token error, so a correct
  token during a lockout looked broken. The device now returns
  `locked`+`retry_ms`; the UI shows a "locked, retry in Ns - your token is fine"
  countdown and auto-retries once.
* **Rate-limiter was a setup foot-gun** - gentler first lockout (15s) and the
  counters now decay after 2 minutes idle, so a fumbling operator isn't walled
  off for 15 minutes. No auto-connect on page load (it burned attempts with a
  stale token and churned the slot); stale `ghosthid` default token is purged
  from browser storage.
* **Token is now case-insensitive** (the generated alphabet is uppercase-only,
  so this only removes mobile mistypes) and accepted with or without the display
  spaces on every path (web, serial `unlock`/`token`, HTTP/OTA).

### Changed

* **Settings decluttered** - the device fingerprint, manual certificate
  paste/upload, and the openssl how-to are collapsed into one "Advanced:
  certificates & fingerprints" disclosure now that trust-on-first-use captures
  the server cert automatically.


## 0.6.16

### Added

* **Screen-server certificate: trust on first use** - connecting to a
  Deskflow/Barrier/Synergy server over TLS no longer requires pasting the
  server's PEM by hand. When no certificate is pinned, GhostHID makes one probe
  handshake, captures the certificate the server offers, and shows its SHA-256
  fingerprint in the web UI (and serial `kvm` status). You confirm it once -
  **Trust & connect** in Settings, or `trustcert` on the console - and it is
  pinned for good. Nothing unconfirmed is ever used for a live session, so the
  confirm step is the out-of-band check that blocks a first-connection
  impostor. Pasting a PEM ahead of time still works and skips the prompt.


## 0.6.15

### Fixed

* **AP SSID showed `-0000`** - the device suffix was read via
  `WiFi.macAddress()`, which returns all-zero bytes before the STA interface is
  populated (and in AP-only mode). Now reads the base MAC from efuse
  (`esp_read_mac`), which is valid in any Wi-Fi mode, so the SSID/mDNS suffix is
  the real device ID.
* **On-device text too close to the rounded corners** - bumped the screen inset
  from 12 to 18 px so no glyphs are clipped by the panel/case radius.


## 0.6.14

### Added

* **`bootloader` serial command** - reboots straight into the ROM USB download
  mode so a flasher can write over USB without the physical BOOT+RST dance (uses
  the force-download-boot RTC flag). Gated behind `unlock` like `reset` when a
  token is set. `download` is an alias.


## 0.6.13

Credential-model clarity - one pairing token, stop making it look like several.
Presentation only; nothing stored or checked changes.

### Changed

* **Setting a new token no longer risks locking you out.** Saving a token in
  Settings now also updates the token this browser presents (and localStorage) in
  the same step, so the "set" field and the "connect" field stay in sync. The
  token is whitespace-stripped on save, matching the 4-char-block display.
* **Clearer token labels.** "Token used by this browser" -> "Pairing token (this
  browser)"; "Pairing token" (Settings) -> "Pairing token - set a new one", so
  the set-vs-present split is explicit.
* **Serial help** spells out that `unlock` takes the *same* pairing token as
  `token`, and labels the two Wi-Fi passwords by scope (join-your-network vs the
  device's own AP) - the other easy mix-up.


## 0.6.12

On-device LCD tidy-up.

### Fixed

* **Rounded-corner clipping.** The panel's corners are physically rounded, so
  text/markers placed pixel-tight into a corner lost a few pixels. All pages now
  keep a safe inset from the edges (corner status labels, page titles, the BOOT
  hint).

### Changed

* **Pairing token gets its own screen.** BOOT now cycles Status -> Join Wi-Fi ->
  Pairing token -> Info. The token is shown large in two 4-char blocks
  (`ABCD EFGH`), auto-sizing to fit; the Wi-Fi page keeps the SSID + AP password.
  (Still auto-reverts to Status after 60 s, and only reachable by a physical BOOT
  press.)


## 0.6.11

The remaining hardware-independent open items from the review.

### Security

* **H7 - HTTP token endpoints are now rate-limited and constant-time.** Only the
  WebSocket auth was throttled (H4); `/api/ota`, `/api/identity` and `/api/export`
  did a raw `==` with no limiter - the real brute-force oracle. They now share a
  counter+cooldown (5 wrong tokens -> 30 s; only a supplied-but-wrong token counts,
  so ordinary probes don't lock the operator out) and a constant-time compare.
* **Token minimum length.** `set_config`/`token` now reject a 1-5 char token
  (empty still disables auth; generated default is 8). Floor of 6, still
  LCD-readable.
* **M1 - serial `unlock` gate.** The USB console is reachable by the target host.
  Once a token is set, the commands that change an already-set security setting -
  `wifi`/`wifipass` (when a station is configured), `token`, `reset` - require
  `unlock <token>` first (constant-time, per-boot). Bootstrap stays open: no token
  set, or a blank station, leaves first-run config over the cable ungated.


## 0.6.10

Token ergonomics + finishing M4 + a couple of easy audit wins.

### Changed

* **Pairing token is now 8 characters, shown in two 4-char blocks** (e.g.
  `AB2C 9XKF`) on the LCD Wi-Fi page - easy to read off the screen and type.
  Uppercase, no ambiguous glyphs. The web UI strips whitespace on entry, so
  typing the space (or not) both work. Existing devices keep their current token
  (only fresh provisioning / factory reset generates the new format).
* **Escalating auth backoff.** To keep the shorter token safe, each lockout now
  doubles the cooldown (30s, 60s, 120s ... capped at 15 min) on top of the
  3-strikes rule; a successful auth resets it.

### Security

* **M4 finished.** The `status` response is now also built with ArduinoJson
  (get_config already was in 0.6.7), so no device-derived field can ever reach
  the client through an unescaped `%s`.

### Fixed

* Auth-timeout disconnect is issued once per client instead of on every loop
  pass (log-noise tidy, from the re-audit).
* **L7** - the release workflow passes the `workflow_dispatch` tag input through
  the environment instead of interpolating it into the shell.


## 0.6.9

Re-audit follow-ups on the credential/origin work.

### Fixed

* **N2 — the origin check refused the device's own mDNS URL.** `hostIsOurs` only
  matched the bare device name, but the registered hostname (and AP SSID) is
  `name-<MACsuffix>`, so a browser at the advertised `name-xxxx.local` URL was
  rejected and the UI was only reachable by IP. It now matches that real name
  (fails closed either way — a usability, not a security, regression).

### Security

* **N5 — a bad AP password no longer restores the published default.** If a
  stored AP password failed validation the code fell back to `ghosthid-setup`,
  and since the provisioned flag was already set it would never re-randomise —
  leaving the board on the published default permanently. It now re-randomises
  from the hardware RNG and surfaces the new value on the LCD/serial.


## 0.6.8

### Security

* **LCD auto-reverts to the status page after 60 s (L1).** The Wi-Fi page shows
  the AP password and pairing token in clear for setup; it now falls back to the
  brand/status page after a minute of no BOOT activity, so the credentials aren't
  left on screen for anyone who glances at the device and walks away. Press BOOT
  to bring the page back.


## 0.6.7

Corrections to the 0.6.4-0.6.6 hardening - some of it was done incompletely.

### Security

* **M4 done properly.** `get_config` is now built with ArduinoJson, which escapes
  every string field. 0.6.6 only sanitised the wire-supplied server name and left
  `kvm_host`, `kvm_state` (which embeds the host in the TLS error text) and
  `sta_ssid` going through raw `%s` - a quote in any of those could still break or
  reshape the JSON the UI trusts. (The `status` response was already safe: its
  device-derived fields are hex-encoded.)
* **H2 entropy fixed.** First-boot credential generation now enables the
  bootloader hardware RNG around `esp_random()`. It runs before Wi-Fi starts (the
  AP needs the password first), and `esp_random()` is only guaranteed
  hardware-random once the RF subsystem is up - so the 0.6.5 credentials could
  have been drawn from a weak source.

### Fixed

* **L4 done properly.** The `web off` -> `web on` handler-stacking fix now lives in
  `stopServers()` as `g_server.reset()`. `end()` stops the listener but does not
  clear the handler list, so the 0.6.6 `serversUp_` guard alone did not prevent
  re-registration after a stop.


## 0.6.6

More security hardening — no functionality change.

### Security

* **M4 — server name sanitised at ingest.** The 7-byte protocol name a screen
  server sends is clamped to a safe charset before it is stored, so a hostile or
  spoofed server can't inject quotes/control bytes into `get_config`'s JSON (it
  is echoed there as `kvm_server`).
* **M5 — no more truncated JSON.** If a response would overflow the buffer,
  `reply()` now emits a short well-formed error instead of a document with no
  closing brace.

### Fixed

* **L2** — `mouse_abs` rejects non-finite (NaN/Inf) coordinates instead of
  casting them to a pixel position (undefined behaviour).
* **L4** — `beginServers()` is idempotent; `web off` then `web on` no longer
  stacks duplicate HTTP handlers.


## 0.6.5

Security hardening — no more published default credentials (H2).

### Security

* **Per-device random credentials on first boot (H2).** A board flashed from a
  release no longer ships with the published `ghosthid` token and
  `ghosthid-setup` AP password. On first boot (and after a factory reset) the
  device generates a random pairing token and AP password from its hardware RNG
  and shows them on the **LCD Wi-Fi page** and the serial log. Only the
  known-published defaults are replaced — a custom-built or user-set credential
  is left alone — and it happens once, guarded by a stored flag.
  Upgrading an existing device that still had the default token will randomise it
  on the next boot; read the new token off the screen.
* The web UI no longer pre-fills the token with `ghosthid`, and it stops
  auto-reconnecting after an auth rejection, so a wrong token can't spin into the
  new auth rate-limit during setup — fix the token and press Reconnect.

## 0.6.4

Security hardening — first pass, closing the network attack surface. From a
full security review of v0.6.3.

### Security

* **Drive-by keyboard closed (H1).** The WebSocket handshake now rejects any
  browser `Origin` that isn't the device's own, and HTTP handlers cross-check the
  `Host` header (DNS-rebinding guard). Previously *any* web page a LAN user
  opened could connect to `ws://<device>/ws` and type on the target with no
  interaction. Native (non-browser) clients — which send no `Origin` — still work.
* **Auth brute-force limited (H4).** Three failed `auth` attempts trigger a 30 s
  cooldown and drop the socket; the failure count survives reconnects, so it
  can't be reset by reconnecting. An unauthenticated socket is also dropped after
  5 s so it can't squat the single controller slot and lock out the operator.
* **Constant-time token compare (M8).** Removes the trivial timing side-channel.
* **OTA lock can no longer wedge the device (H6).** If a firmware upload locks
  HID and then never completes (client vanished mid-upload), the lock now clears
  itself after 60 s instead of refusing all input until a manual reboot.
* **Removed a safety flag that did nothing (§8).** `GHOSTHID_AUTORUN_SELFTEST`
  was referenced nowhere in the firmware; a build made "safe" with it was exactly
  as capable of typing. Deleted rather than left as a lie.

## 0.6.3

### Added

* **Host lock-LED feedback.** The firmware now reads the HID *output* report the
  target sends back (the keyboard's Caps/Num/Scroll Lock LED state) and surfaces
  it three ways: a CAPS/NUM/SCROLL row on the on-device LCD, lit badges in the
  web UI's new "Target feedback" panel, and a `locks` + `hled` field on every
  `pong`. A non-zero `hled` (count of output reports the host has sent) is hard
  proof the target has enumerated GhostHID's keyboard and is actually driving
  it - the long-standing "is it actually working?" question, answered from the
  target's own side.

* **Media & system-control keys.** A HID consumer-control collection (volume
  up/down/mute, play-pause, next/previous, stop, brightness) and a
  system-control collection (sleep, wake, power off), exposed as buttons in the
  web UI's Control tab and as `{"type":"media","key":...}` /
  `{"type":"system","key":...}` messages.

* **Config export / import.** A token-gated `GET /api/export` downloads the
  board's non-secret settings as JSON - device name, AP behaviour, scroll
  direction, Wi-Fi SSID and every screen-client setting, *including the server
  certificate*. "Import settings" on another board applies them in one step.
  Secrets (Wi-Fi/AP passwords, pairing token) are never exported and are set
  per-board. Makes provisioning a second, third or fourth board quick.

* **Presenter mode.** A new Present tab: large Next / Prev, Start-show, Black and
  End controls, plus two timers - total talk time and a per-slide timer that
  resets on every advance, like a hardware presenter remote. App presets
  (PowerPoint/Impress, Google Slides, Keynote, PDF) pick the right keys, and the
  screen is kept awake while the clock runs.

* **3D-printed case** (`hardware/case/`). A parametric OpenSCAD case for the
  ESP32-S3-LCD-1.47, built to Waveshare's structural drawing: four locating
  bosses on the Ø2.0 mounting holes, an open USB-A end, side BOOT/RST holes, a
  snap-fit lid, and the GhostHID ghost as a two-colour inlay. Ships as STLs and a
  ready-to-slice `ghosthid-case.3mf`.

* **Promo site** (`site/`). A self-contained landing page with live web-UI
  screenshots and download links for the flasher, firmware and case.

### Changed

* **Settings page tidied** - long explanations trimmed to one-liners and the
  certificate how-to moved into a collapsible, so the page is far less wordy.
* Nav tabs now sit on their own row and never truncate on a narrow header.

### Fixed

* **Stale input state after a reconnect.** The web UI now clears sticky modifiers
  and physical-keyboard capture whenever it (re)connects, so a device reboot/OTA
  can no longer leave combos (e.g. Ctrl+C) misbehaving until a manual refresh.

## 0.6.2

### Added

* **Invert scroll (natural scrolling)** - a Mouse setting in the web UI that
  flips the wheel (and horizontal pan) direction. Applies live to both the web
  trackpad and the screen client; persisted in NVS.

## 0.6.1

### Removed

* **ESP32-S2 support.** The S2 is single-core with ~13KB of contiguous heap and
  could not hold a sustained Deskflow KVM session under load - it kept dropping
  the connection where the dual-core S3 (135KB block, ~10µs service passes) is
  rock steady. GhostHID is now **ESP32-S3 only**; the `esp32-s2-key` build env,
  its web-flasher build, and the chip-selection branches are gone.

## 0.6.0

Adds the ESP32-S3 board with an on-device screen, a browser flasher, and the
0.5.0 stability work folded in.

### Added

* **ESP32-S3-LCD-1.47 (Waveshare) support.** Dual-core, 16MB flash, 8MB OPI
  PSRAM, native USB-C HID. Built on the pioarduino platform (arduino-esp32 3.x)
  because the stock platform's bootloader bootloops this chip revision. The
  Deskflow task is pinned to the second core so Wi-Fi/TLS can't starve input.
  Measured: 135KB largest free block and ~10µs service passes, versus the S2's
  13KB and 48-128ms stalls under load.
* **On-device status LCD.** A hero screen (cyan ghost logo + GhostHID wordmark,
  small corner status), an AP-join QR page (scan to join the device's Wi-Fi),
  and an info page (version/heap/uptime). BOOT short-press cycles pages,
  long-press is the panic release. RGB status LED (red = no USB, green = screen
  has focus, cyan = connected, dim = idle).
* **Browser flasher** (`webflasher/`, ESP Web Tools). Flashes S2 or S3 over Web
  Serial from Chrome/Edge, auto-detecting the chip. `make webflasher` stages it.
* **Server-certificate field** in the web UI (paste box + file upload) with
  generic instructions for obtaining it (the `openssl s_client` one-liner and
  the Barrier/Deskflow `SSL/*.pem` paths). Previously the firmware said "paste
  the PEM in settings" with no field to paste into.

### Changed

* **Generic USB identity by default** (`0x1A2C` "USB Keyboard") instead of
  Espressif's `0x303A:0x4004`, so EDR / BadUSB defences don't flag an unknown
  dev-board HID on insert. Overridable per build in `ghosthid_local.ini`.
* Web UI: the tab bar no longer truncates between ~600-820px (it takes its own
  full-width row earlier); the physical-keyboard capture no longer types into
  other focused fields (it was typing your Wi-Fi password into the target);
  on-screen mouse buttons can't stick down; a rejected token is now visible on
  a phone.
* `make flash` / `flash-factory` are chip-aware (S3 bootloader at 0x0, DIO,
  16MB), and the merged image bakes the correct per-chip flash geometry.

### Fixed

* **Stuck keys under concurrency** — `HidDevice` was called from two FreeRTOS
  tasks with no lock; `releaseAll()` racing a `keyDown()` could re-assert the
  key it had just cleared. Now serialised by a mutex.
* **Watchdog reboot** from an unbounded `mouse_move`/`text` flooding blocking
  USB reports inside one network callback — clamped at the protocol boundary.
* **Cross-task use-after-free** — `reconnect()`/`suspend()` tore down the TLS
  socket under the Deskflow task; now deferred to that task. The pinned CA was
  freed mid-parse by `set_config`; the client now works from a private copy.
* **A second browser tab released the first controller's keys** — only the
  owning WebSocket's disconnect now ends the session.
* **Truncated `get_config` JSON** when a long TLS error was present (reply buffer
  512→1024).
* **Deskflow held-input backstop** (2.5s) so a server that dies mid-keypress
  doesn't leave a key down until the 15s keep-alive; `readExactly` gained a true
  deadline.
* **USB-CDC console could block the firmware** when a host held the port open
  without draining it (a serial monitor over a slow link) — `setTxTimeoutMs(0)`
  makes console writes drop rather than ever block.
* OTA: dangling error pointer, auth checked before any side effect, and an empty
  POST no longer reboots the device.

## 0.5.0

Stability hardening from a three-part audit (concurrency, memory, UI). The
theme: HID reports, the TLS socket and config buffers were touched by three
FreeRTOS tasks with no synchronisation, and blocking USB/TLS work ran inside the
async network callback.

### Fixed

* **Stuck keys under concurrency.** `HidDevice` had no lock, yet was called from
  the WebSocket handler (async_tcp task) and the Deskflow client (its own task).
  The framework's keyboard report is a shared object mutated read-modify-write
  *before* the report is sent, so `releaseAll()` racing a `keyDown()` could
  re-assert the key it had just cleared and leave it held on the target — the
  exact failure the whole layer exists to prevent. Every method that touches the
  report is now serialised by a mutex.
* **Watchdog panic reboot.** `mouse_move` accepted the full int32 range and
  `text` any length; each becomes a flood of blocking USB reports inside one
  network callback, which tripped the 5 s task watchdog. Both are now clamped at
  the protocol boundary.
* **Cross-task use-after-free.** `reconnect()` / `suspend()`, called from the
  network task, tore down the socket and TLS state (freeing the two 17 KB
  TlsArena blocks) under the Deskflow task mid-handshake. They now raise a flag
  the Deskflow task acts on itself. The pinned CA PEM was likewise freed by
  `set_config` while mbedTLS was parsing it; the client now works from a private
  copy taken under a lock.
* **A second tab released the first controller's keys.** A refused second
  WebSocket connection's disconnect ran the same handler as the owner's,
  dropping every held key and de-authenticating the live controller. Only the
  owning client's own disconnect now ends the session.
* **Truncated JSON.** `get_config` with a long screen-client error ran past the
  512-byte reply buffer, so the browser threw on parse every poll — worst
  exactly when the device had a TLS error to report. Buffer raised to 1 KB.
* **Held keys on the Deskflow path.** A server that died mid-keypress (power
  cut, Wi-Fi loss, no TCP FIN) left a key down until the 15 s keep-alive
  timeout. A 2.5 s held-input backstop now releases it, and `readExactly`
  enforces a true deadline so a trickling peer cannot hold a key indefinitely.
* **OTA dangling pointer** (`g_otaError` pointed at a freed stack frame),
  **OTA auth ordering** (an unauthenticated POST could drop the screen session
  and stall the async task before the token was checked), and an **empty POST**
  that rebooted the device having written nothing.
* **UI, actively-wrong cases:** the physical-keyboard capture typed into *any*
  focused field — so switching to Settings with capture on typed your Wi-Fi
  password into the target; an on-screen mouse button could stick down on a
  press-drag-release (no pointer capture); mouse buttons were unreachable by
  keyboard; and a rejected token was invisible on a phone (the state word was
  hidden below 600px). Key/F-key grids could also clip on narrow phones.

### Deferred (tracked in PLAN.md)

* The deeper structural fix — a dedicated HID task behind a queue so only one
  task ever touches the report — plus `SendReport` return-checking, the
  `esp_http_server` swap, and the ESP-IDF-from-source memory wins. None blocks
  stability; all are best done with the device on the bench for verification.

## 0.4.2

### Removed

* **Lean mode.** It skipped the web server entirely while the screen client ran,
  to buy contiguous heap. Measured on hardware it does what it claimed — 15KB
  largest block becomes 23.5KB — but that is a 57% gain on a number nothing is
  failing on, paid for with the whole web interface. It was built on the
  assumption that 13KB was a crisis; measurement showed it is not.

### Added

* The update endpoint refuses to compete for memory. `GET /api/ota` reports what
  is connected and how much is free, so a client can warn before sending 800KB
  rather than after; `POST` returns 409 if the screen client is holding memory,
  and with `?force=1` disconnects it first rather than hoping there is room.
  Attempting an update against a screen session in active use had taken a device
  down. Both the browser and `make ota` ask first.

### Fixed

* `Settings` was truncated to `Setting…` in the top bar on phones. Below 600px
  the tabs now take a full-width row of their own instead of competing with the
  USB badge and Release button — tightening the padding was not enough, since it
  was happening on a 440px iPhone Pro Max and would have been worse on a 375px
  device.
* A second controller was refused with close code 1013 and then silently
  retried, which looked like a flaky connection rather than a deliberate limit.
  The page now says another controller is connected.

## 0.4.1

### Added

* **Lean mode** (`lean on`). With the screen client enabled, the web server is
  not started at all, leaving the heap near its boot state and the serial
  console as the management path. Not starting it is the only thing that helps -
  AsyncTCP never tears its task down, so stopping the server later frees nothing
  measurable. It cannot lock anyone out: if the screen client has not connected
  within 90 seconds of boot, the web server starts regardless.
  **Not yet verified on hardware.**

### Fixed

* Documentation overstated the memory situation. It claimed the web UI becomes
  sluggish once a TLS session is established and that updates require stopping
  the screen client first. Measured, neither holds: the UI serves its full
  16KB page in 34-86ms, an over-the-air update succeeds with a session live in
  13.5s and the device reconnects by itself, the heap is stable with no leak,
  and no HID reports are refused. The earlier update failures predate the TLS
  buffer reservation, which fixed them as a side effect. 13KB of contiguous
  heap is tight and worth watching, but nothing measured fails because of it.
* The repository structure in `PLAN.md` still described directories that were
  either dropped or never filled.

## 0.4.0

Adds a Deskflow / Barrier / Input Leap screen client, verified working end to
end against a live Deskflow 1.8 server over mutual TLS.

### Added

* **Screen client.** GhostHID joins an existing Deskflow, Barrier or Input Leap
  server as a screen, so you move the pointer off the edge of your desktop and
  onto the machine it is plugged into, keyboard following. Those projects
  already solve the hard half - capturing and suppressing input on the
  controller, detecting edge crossings, multi-monitor layout, on every desktop
  OS - so GhostHID is simply another screen in a layout you already have, one
  that needs nothing installed on it.
* **Mutual TLS with a device identity.** These servers require client
  certificates. The device generates an EC P-256 key and self-signed
  certificate on first use and keeps them in NVS, so a server recognises the
  same device each time. The fingerprint is shown in the settings tab.
* Configuration for all of it in the web UI and over serial, plus a top-bar
  badge showing the connection state, labelled with the name the server gave in
  its handshake.
* Per-message diagnostics in `status`: counts by message type, the last
  unrecognised message, refused HID reports, per-task stack headroom and heap
  by boot stage.

### Changed

* **Pointer motion is much smoother.** Two causes, both ours. Every incoming
  position produced a USB report, and each report blocks until the host
  collects it, so a backlog of stale positions was being replayed instead of
  the current one - motion is now coalesced to the newest position. And the
  service loop measured whether it was busy *after* draining the socket, when
  nothing is pending, so it took its slow branch during exactly the motion it
  was meant to serve; it now runs on a fixed one-tick cadence. Evenness
  mattered more than rate: the endpoint already polls at 1ms.
* The screen-client task runs at a higher priority, since at the lowest it was
  freely preempted by Wi-Fi and lwIP on this single-core part.
* `setNoDelay` is now applied to the TLS socket, not only the plaintext one.
* AsyncTCP's task stack halved to 8KB. Its stack is a permanent contiguous
  allocation - AsyncTCP 3.5.0 never tears the task down, which is why stopping
  the web server frees nothing - so its size is subtracted from the largest
  block a TLS handshake can obtain. Measured use is ~2.4KB.
* The TLS I/O buffers are reserved at boot, while the heap is whole, and handed
  to mbedTLS through its allocator hook. This does not reduce memory use; it
  takes the two 16KB allocations out of the fragmentation game, so a dropped
  session can reconnect instead of requiring a reboot.

### Fixed

* Keyboard input did not work at all. Protocol 1.8 does not send `DKDN`: it
  uses a distinct wire code `DKDL`, and a server negotiating 1.8 emits it
  exclusively. Separately, a macOS server sends KeyID 0 on key-up and
  identifies the key only by its physical button, so the client must remember
  which key it pressed for that button and release that.
* Settings fields were overwritten mid-keystroke by the status poll.
* Enabling the screen client after boot reported success when it could not
  actually connect for want of contiguous memory; it now says so.

### Known limits

With a TLS session established the largest free block is around 13KB. That is
tight but, measured rather than assumed, sufficient: the web UI serves its full
page in under 90ms, over-the-air updates succeed with a session live, no HID
reports are refused, and the heap is stable over time. Reducing it further would
need `MBEDTLS_SSL_IN_CONTENT_LEN` lowered, which requires building ESP-IDF from
source - worth doing only if a real failure appears.

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
