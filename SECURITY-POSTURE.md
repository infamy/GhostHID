<img src="assets/logo.svg" alt="GhostHID" width="240">

# Security posture

*How this project thinks about security, what it defends, and what it deliberately
does not. Written to be read by a person, start to finish.*

---

## Start with the uncomfortable part

GhostHID is a keyboard. Anyone who can drive it can type into a logged-in session, and
typing into a logged-in session is the same thing as running commands there.

So this is not a device where a security failure means someone wiggles your mouse. **An
authentication failure here is full compromise of the target machine** — no driver, no
agent, no EDR hook, no audit trail, behind a USB identity chosen to look like the keyboard
already on the desk. We design from that sentence, not from a softer one.

Everything below follows from taking it literally. If a control would not hold against
"the attacker gets a shell", we do not describe it as a boundary.

---

## What we actually believe

Seven principles. Each one produced decisions you can find in the code, so each is listed
with what it cost or caused — a principle that never forced a trade-off is decoration.

### 1. Start from the worst case, not the demo

The threat we model is arbitrary command execution on the target, because that is what the
device *is*. That framing is why the pairing token gates every command rather than merely
the connection, why token comparison is constant-time, and why the OTA path is treated as
equal in severity to the keystroke path — installing firmware and typing a command are the
same privilege here.

### 2. No two devices share a secret

There is no default password. There never was, and there is nothing to look up.

On first boot each board generates its own random AP passphrase and pairing token, seeded
from bootloader entropy before the radio is up. Two GhostHIDs sitting side by side have
nothing in common an attacker can reuse. This is the single highest-value thing a small
hardware project can do, and it is cheap, so there is no excuse for the alternative.

### 3. Fail closed, and be loud about it

A stored AP passphrase that fails validation is re-randomised, not quietly restored to a
default and certainly not brought up as an open network. An over-long response becomes a
well-formed error rather than truncated JSON. An unauthenticated peer that squats the
client slot is timed out so it cannot hold the device hostage. Repeated auth failures trip
an escalating cooldown and drop the connection.

The pattern: when the device is unsure, it refuses and says so, rather than degrading into
something that still works but no longer protects you.

### 4. The owner owns the board

This is a bring-your-own-ESP32 project. **The device must never refuse to run an image its
owner flashed.**

That single commitment rules out the two controls a checklist would demand — Secure Boot
and on-device signature enforcement — and we are comfortable with that, for reasons in
the next section. What we do instead is sign official releases so anyone who wants
provenance can verify a download is genuinely ours, unmodified, before flashing it. Trust
for whoever wants it; lock-in for nobody.

### 5. If it cannot hold, do not call it a boundary

We have rejected security features on these grounds, and we say so in writing rather than
shipping them for the feature list.

An arm/disarm model was proposed and dropped: it is incompatible with the entire
unattended use case, and the detection value it offered is already delivered by the status
LED. Prevention-by-presence and "act on a machine you are not sitting at" are mutually
exclusive; you cannot sell both.

A proposed "protected mode" — suppressing the specific keyboard combos a scripted
injection uses — is documented as **friction and a tripwire, not a boundary**. It breaks
the copy-paste one-liner and generates the highest-signal audit event we have. It cannot
stop a patient attacker, and the doc says exactly that, including which bypass path stays
open and why closing it would be worse.

### 6. A stuck key is a security bug

The worst thing this device can physically do to a target is hold a modifier down. That
gets three *independent* layers, not one: the disconnect handler releases everything
immediately; a watchdog releases everything if a controller holds input then goes silent
for 750 ms; and the BOOT button is a physical panic release that needs no network at all.

Independent is the operative word. Any one of the three can fail without the other two
caring.

### 7. Publish the gaps

The list of things this device does not defend against is part of the documentation, in
the README, in the same voice as the features. Not an appendix, not a disclaimer nobody
reads. A user who does not know the boundary cannot stay inside it.

---

## What guards what

| Boundary | Guarded by |
|---|---|
| Radio range → the device's own AP | WPA2 with a per-device random passphrase, set on first boot |
| The AP or your LAN → the WebSocket | Pairing token on every command; browser-origin and `Host` checks; auth attempts throttled |
| A screen server → HID input | Mutual TLS with the server's certificate pinned, when `kvm_tls` is on |
| The target machine → the device | **Nothing while unsealed** — deliberate, explained below. **Sealed:** no serial interface exists on the bus at all. |

Three details in that table are worth pulling out, because they are the ones reviewers ask
about.

**The token gates commands, not the connection.** A peer that reaches the socket still
cannot type, read config, push firmware or export settings. Refusing at the command layer
rather than the door is what makes "reached the socket" a non-event.

**WebSockets ignore the same-origin policy**, so a page you never opened could otherwise
reach the device from your own browser. The handshake's `Origin` is checked against the
device's real address and the `Host` header is cross-checked to blunt DNS rebinding.

**Secrets are write-only across the network.** The Wi-Fi passphrase, AP passphrase and
token can be set but never read back: the API reports only whether each is set, and the
serial console's `show` masks them. This holds against the network. It does not hold
against someone holding the board — which is the next section.

---

## Decisions we made on purpose

This is the part worth reviewing. Each of these is a place we did not do the obvious thing,
and the reasoning matters more than the conclusion.

### No Secure Boot, no flash encryption

Locking the board to signed images only defends against a physical attacker replacing
firmware — a threat this device **already concedes**, because anyone holding the board can
put it in ROM download mode and read flash regardless. The cost is burning one-way eFuses,
bricking on a mistake, and permanently converting a bring-your-own-ESP32 into an appliance
its owner cannot modify.

Paying an irreversible cost to partially cover a threat we have already accepted is a bad
trade. We took the honest version instead: flash is readable by someone with physical
access, it is written down as a non-goal, and the token should be treated as also granting
"dump every secret" unless the device is sealed.

### Signed releases, zero on-device enforcement

*Status: agreed and specified, not yet shipped — there is no public release yet, and this
is a gate to clear before the first one.*

The plan is minisign — one small static binary, one keypair, a one-line public key, no
keyring or web-of-trust ceremony — signing release artifacts in CI, with verification
**operator-side and opt-in**. It needs no firmware changes, which is the point: provenance
is a distribution property, not a device behaviour.

On-device enforcement was considered and rejected twice over. It would defeat the
build-your-own premise, and it buys little even on its own terms: OTA already requires the
token, and the token already grants full keyboard control, so a token-holder flashing
firmware is not an escalation — they already own the target.

The other pre-release gate is pinning the toolchain. The build currently pulls a floating
platform URL and caret-ranged libraries, which means the binary is not purely a function of
the commit — and signing an artifact you cannot reproduce is a weaker promise than it
looks.

### The USB identity is generic, and we say why

The device presents a generic keyboard VID/PID rather than an Espressif one, so the target
cannot practically distinguish it from the keyboard already plugged in.

That is the product working as intended, and it is also precisely the risk. The two are not
separable, so we do not present the blend-in property as a security feature — it is stated
as what makes the deployment boundary matter. Plug this into machines you administer. That
is not a legal disclaimer; it is the boundary the design assumes.

### The serial console is open for bootstrap

A brand-new board has to be configurable by someone holding it, so the console starts open.
The dangerous commands — changing Wi-Fi, changing or clearing the token, factory reset,
entering the bootloader — sit behind an `unlock <token>` gate once a token is set.

A full console password was proposed and downgraded on purpose: physical or USB access
already implies a flash dump, so a password there protects nothing real while making
first-run setup worse.

**Sealed mode is the real answer, and it is stronger than a password would have been.**
Rather than protecting the console, it deletes it: on a sealed device no serial interface
is added to the USB descriptor at all, so the board enumerates HID-only and a process on
the target has nothing to talk to. Undoing it takes a ~5-second physical BOOT hold, a
reboot, and an explicit `unseal` — and the window is one-shot, so a device nobody unseals
returns to HID-only on the next boot.

The shape of that design is forced by hardware, not chosen: an interface can only join the
USB descriptor before the USB stack starts, so the decision is made once at boot and cannot
be toggled at runtime. That constraint is why sealing takes effect across a reboot, and why
unsealing requires physical hands rather than a network command — which is exactly the
property you want from it.

### Credentials on the screen need a physical press

The pairing token is displayed on the LCD only on a page reached by physically pressing
BOOT, and it auto-reverts to the status page after 60 seconds. Hiding it entirely was
considered and rejected: it is the only recovery path on a device whose stored config
survives a reflash. Requiring physical presence to reveal a credential is the correct
shape.

### The token stays off the wire; session content is not yet encrypted

As of 0.9.0 the pairing token is **never transmitted**. A controller proves it knows the
token with a challenge-response handshake — the device sends a random nonce, the client
returns `HMAC-SHA256(token, nonce)`, and only the proof crosses the wire. A passive
listener who captures a session recovers nothing replayable, so token length is genuinely
meaningful again. From there every keystroke and mouse command rides in a MAC'd envelope
with a monotonic counter, so input cannot be forged, tampered with, or replayed even by
someone sitting on the same network.

What is **not** yet solved: the transport itself is still `ws://` / `http://`, so the
*content* of a session is not encrypted. Someone who already holds the AP passphrase and
captures the traffic can still read what a session does — the keys you send — even though
they cannot recover the token or inject input of their own. Closing that last gap needs a
TLS control plane (ESPAsyncWebServer offers none, and per-connection TLS is a real memory
cost on this part); it is recorded as the remaining limitation rather than papered over.
The bearer-token model itself — one shared secret, no per-user identity — is non-goal 2
below and unchanged.

---

## What this does not defend against

Non-goals, not oversights. Anyone deploying the device should be able to recite these.

1. **The machine you plug it into — unless the device is sealed.** An unsealed device can be
   reconfigured or taken over across the USB boundary. It cannot read your passwords, but it
   can own the stick. Sealing closes this by removing the serial interface from the bus.
2. **Anyone who has the token.** It is a bearer credential — no per-user identity, no
   revocation beyond changing it, no record of who used it.
3. **Anyone watching the network.** See the plaintext control plane above.
4. **Local flash extraction.** Physical access yields the Wi-Fi passphrase, the pairing
   token and the TLS private key.
5. **Firmware you choose to flash.** By design. The token is the power to install firmware.
6. **A screen server reached with `kvm_tls` off.** Plain TCP, no peer authentication. The
   setting exists for bench use; leaving it off in the field hands a keyboard to whoever
   can win a DNS race.
7. **An image that boots but breaks networking.** No automatic rollback; recovery is
   physical.

---

## How this gets reviewed

`SECURITY-REVIEW.md` is a **living state document, not a log**. Findings are ranked, and
each one is either fixed with the version that fixed it, accepted by design with the
reasoning attached, or withdrawn with the argument recorded so it does not get re-raised in
six months. Nothing is closed by assertion.

Two habits are worth naming because they are unusual:

**Withdrawn findings keep their reasoning.** The arm/disarm model, the console password and
hiding the LCD token are all written up as *why not*, permanently. A reviewer can disagree
with the conclusion and argue against the actual argument rather than guessing at it.

**Fixes that introduced follow-ups say so.** The OTA per-request state fix (0.6.26) created
an owner-cleanup problem that became its own tracked finding and its own fix (0.6.27). That
lineage is in the document. A review doc that only records wins is not a review doc.

Where the project stands today: no open line-level findings. What remains is proposed
hardening — a wear-aware audit log, the protected-mode tripwire, a native test harness for
the safety-critical invariants — tracked as design work rather than as defects, because
calling design work a defect is how backlogs start lying to you. Two items are gates
before the first public release rather than backlog: release signing and a pinned
toolchain.

The known weak spots, in order of how much they should worry you: the plaintext control
plane, the absence of any audit trail, and the untested safety invariants. All three are
written down above or in the review doc, and none of them are secrets.

---

## If you are on the other side of this

If you defend machines rather than drive them remotely: this device is blocked the way any
unexpected HID is blocked — USB device-authorisation policy. `USBGuard` on Linux,
device-installation restrictions by device ID on Windows, an MDM-enforced allowlist on
macOS.

The general control is *do not accept a newly-enumerated keyboard on a machine that already
has one*, and it works against GhostHID exactly as well as it works against anything else in
this category. The generic USB identity does not evade it, because that control does not
depend on recognising the specific device.

We would rather you know that than not.

---

## Reporting a problem

Report privately to the maintainer rather than opening a public issue, and expect the
finding to be tracked in `SECURITY-REVIEW.md` by name — fixed with a version, accepted with
reasoning, or withdrawn with the argument. Findings that change the picture above will
change this document too.

---

*Companion documents: [`SECURITY-REVIEW.md`](SECURITY-REVIEW.md) is the working findings
state, and the README's threat-model section is the operational version for people
deploying a device. This document is the reasoning behind both.*
