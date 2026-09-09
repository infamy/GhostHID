# Threat model

The operational half of the security documentation: what guards what, what this does not
defend against, and how to deploy it without surprises. The reasoning behind these choices
is in [SECURITY-POSTURE.md](SECURITY-POSTURE.md).

Also published at `/security/` on the website, and drops into `README.md` as a
`## Threat model` section between **Sending keystrokes** and **Using it from a phone**.

---

## Threat model

GhostHID is a keyboard. Anyone who can drive it can type into a logged-in session, and
typing into a logged-in session is the same thing as running commands there. So the
security of this device is the security of every machine you plug it into, and it is worth
being exact about what that means.

### What the target sees

An ordinary composite USB keyboard and mouse. No driver, nothing installed, and a generic
VID/PID rather than an Espressif one — so the target cannot practically tell it from the
keyboard already on the desk. Nothing is logged on the target, and nothing there records
what was typed.

That is the entire point of the device. It is also the risk, and the two are not
separable: **whoever reaches the WebSocket and passes the pairing token has what someone
sitting at the keyboard has.** Not "can move the pointer" — a shell, in practice.

Plug this into machines you administer. That is not a legal disclaimer, it is the boundary
the design actually assumes.

### What guards what

| Boundary | Guarded by |
|---|---|
| Radio range → the device's AP | WPA2, a per-device random passphrase set on first boot |
| The AP or your LAN → the WebSocket | Pairing token on every command; browser-origin and Host checks; auth attempts rate-limited |
| A screen server → HID input | Mutual TLS with the server's certificate pinned (when `kvm_tls` is on) |
| The target machine → the device | **Nothing while unsealed** — see below. **Sealed:** no serial interface exists on the bus at all. |

### What it does defend against

* **Someone in range who has not been given the passphrase.** The AP is WPA2, never open;
  each device generates its own random passphrase and token on first boot (shown on the
  LCD and serial), so no two boards share credentials and there is no published default to
  guess. A stored passphrase that fails validation is refused rather than silently
  bringing the AP up open.
* **A drive-by from another web page.** WebSockets are not bound by the browser's
  same-origin policy, so this is checked explicitly: a handshake whose `Origin` is not the
  device's own address is refused, and the `Host` header is cross-checked to blunt DNS
  rebinding. A page you did not open cannot reach the socket.
* **Token guessing.** Repeated auth failures trip a cooldown and drop the connection, and
  an unauthenticated connection that squats the single client slot is timed out — so the
  slot cannot be held against you, and guesses cannot be sprayed at line rate.
* **Recovering the token, or forging input, from the network.** The token is never
  transmitted: a controller proves it with challenge-response (`HMAC-SHA256(token, nonce)`
  over a fresh random nonce). Once authenticated, every input command carries a MAC and a
  monotonic counter, so a peer who captures traffic cannot replay it, tamper with it, or
  synthesise input of their own. (Confidentiality of the *content* is separate — see
  non-goal 3.)
* **An unauthenticated peer on the network.** The token gates every HID command, every
  config read and write, the firmware-update endpoint and the config export. Refusing the
  *commands* rather than the connection is deliberate: it is what stops a peer that reaches
  the socket from typing.
* **Reading your secrets back out over the network.** The Wi-Fi passphrase, AP passphrase
  and token are write-only: the API reports whether each is set, never its value, and the
  serial console's `show` never prints one. (This holds against the network; it does not
  hold against local flash extraction — see item 4.)
* **A rogue screen server.** With `kvm_tls` on, the connection is mutual TLS against the
  certificate you pinned. Nothing else can feed input down that path.
* **Keys stuck down on the target.** Three independent layers — see *Stuck-key safety*.
* **A failed update.** Dual-slot OTA; an interrupted upload leaves the running firmware
  untouched.
* **The target machine taking the device over — once sealed.** A sealed device enumerates
  HID-only: no serial interface is added to the USB descriptor, so there is nothing on the
  bus for a process on the target to talk to. Unsealing takes a ~5 s physical BOOT hold, a
  reboot and an explicit `unseal`, and the window is one-shot. Seal anything you deploy
  unattended.

### What it does not defend against

These are non-goals, not oversights. Know them before you deploy.

1. **The machine you plug it into — unless the device is sealed.** On an unsealed device the
   USB serial console has no authentication, so any process on the target can reconfigure it
   — change or clear the pairing token, repoint the Wi-Fi, or factory-reset it. It cannot
   read your passwords (`show` masks them), but it can take the device over across the USB
   boundary. Plugging an **unsealed** GhostHID into a machine you do not administer means
   handing that machine the device.

   **Sealing closes this.** A sealed device never adds the serial interface to its USB
   descriptor, so it enumerates HID-only and offers the target nothing to talk to. Undoing
   it takes a ~5 s physical BOOT hold, a reboot, and an explicit `unseal`; the window is
   one-shot, so a device nobody unseals is HID-only again next boot. Seal anything you
   deploy and walk away from.

2. **Anyone who has the token.** It is a bearer credential: no per-user identity, no
   revocation beyond changing it, and (today) no record of who used it.

3. **Reading what a session sends.** The token itself is safe on the wire — since 0.9.0 it
   is proven by challenge-response (`HMAC-SHA256(token, nonce)`) and never transmitted, and
   input is carried in a MAC'd, counted envelope that cannot be forged or replayed. What is
   still not encrypted is the *content*: the transport is `ws://` / `http://`, so a peer who
   already holds the AP passphrase and captures the traffic can read what a session does —
   the keys you send — without being able to recover the token or inject input. A TLS
   control plane would close this; it is the accepted residual gap.

4. **Local flash extraction.** NVS is not encrypted and secure boot is not enabled
   (deliberately — see below), so the Wi-Fi passphrase, the pairing token and the device's
   TLS private key are readable from flash by anyone who can put the board into ROM download
   mode. Two ways there: physically holding BOOT at reset (walk off with the stick for a
   minute), or the serial `bootloader` command — which is why that command requires the
   pairing token, and why Sealed mode disables it entirely so a deployed device needs the
   physical button. Treat the token as also granting "dump every secret" unless the device
   is sealed.

5. **Firmware you flash — by design.** This is a bring-your-own-board project: the device
   runs whatever image you give it and never refuses one. OTA is gated only by the token,
   so **treat the token as the power to install firmware**. Official releases are signed
   (minisign) so you can verify a download is genuinely ours before flashing it — but that
   check is yours to run; the device does not enforce it, and never will.

6. **A screen server reached with `kvm_tls` off.** That path is plain TCP with no
   authentication of the peer. Anyone who can MITM or win a DNS race for that host gets a
   keyboard on the target. The setting exists for bench use; do not leave it off in the
   field.

7. **An image that boots but breaks networking.** There is no automatic rollback, so
   recovery is physical.

### Current gaps

There are no open line-level findings. The drive-by class (origin and `Host` checks,
auth throttling on both the WebSocket and HTTP paths, per-device credentials) is closed,
and so are the two OTA robustness bugs that followed from it.

What should still shape how you deploy this, in the order it matters:

* **The control plane is plaintext** — the most significant limitation, and item 3 above.
  A known-PSK network is not confidentiality against the people who hold the PSK.
* **There is no audit trail.** The device keeps no record, so misuse leaves no evidence on
  the device or the target. A wear-aware design (volatile detail in PSRAM, durable counters
  in NVS, never keystroke content) is specified but not built.
* **The safety-critical invariants are not yet covered by tests** — `releaseAll` from every
  path, the auth gate, the OTA input lock. A native harness is in progress.

### Why no secure boot / signed-firmware lock

Deliberate. Locking the board to only run signed images defends against a physical attacker
replacing firmware — a threat this device already concedes (item 4) — at the cost of
burning one-way fuses and turning a bring-your-own-ESP32 into an appliance you cannot
modify. That trade is wrong for this project. We sign releases so you can *verify* them;
we do not stop you running your own.

### Deploying it safely

The first-boot credentials get a fresh board talking; they are yours to keep or replace.

1. **Note your per-device credentials.** On first boot the AP passphrase and token are
   randomised and shown on the LCD (BOOT to cycle to the Wi-Fi page) and on the serial
   console. Write them down, or set your own.
2. **If you set your own token, make it 6+ characters** and random. Length only defends the
   online-guess path; it does nothing against a network sniffer (item 3) or the LCD/flash,
   so six is plenty.
3. **Prefer the device's own AP** over joining a shared network. Smaller exposure, and the
   AP is always available regardless.
4. **Unplug it when you are not using it.** It is a keyboard; it is only a risk while it is
   attached to something.
5. **Treat the token like an SSH key.** Anyone who has it can type on the target and
   install firmware.

### If you are on the other side of this

If you are defending machines rather than driving them remotely, a device like this is
blocked the same way any unexpected HID is: USB device-authorisation policy — `USBGuard` on
Linux, device-installation restrictions by device ID on Windows, an MDM-enforced allowlist
on macOS. The general control is "do not accept a newly-enumerated keyboard on a machine
that already has one", and it works against this device as well as any other.

### Reporting a problem

See [SECURITY.md](SECURITY.md).
