# Changelog

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
