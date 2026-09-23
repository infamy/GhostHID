// GhostHID - Deskflow/Barrier/Synergy message handling (input path).
//
// Split out of DeskflowClient.cpp so the message interpreter can be compiled and
// unit-tested on the host WITHOUT the TLS/Wi-Fi transport that fills the rest of
// that file. These are still DeskflowClient member functions; only the transport
// calls they make (sendCode/sendScreenInfo/disconnect) live in the other TU.

#include "DeskflowClient.h"

#include <Arduino.h>
#include <string.h>

#include "config/Config.h"
#include "hid/HidDevice.h"
#include "DeskflowWire.h"

namespace ghosthid {

void DeskflowClient::dispatch(const uint8_t *m, size_t len) {
    lastTrafficMs_ = millis();

    if (is(m, len, "CALV")) { sendCode("CALV"); return; }   // keep-alive
    if (is(m, len, "CNOP")) { return; }
    if (is(m, len, "QINF")) { sendScreenInfo(); return; }
    if (is(m, len, "CIAK")) { return; }
    if (is(m, len, "CROP")) { return; }
    if (is(m, len, "DSOP")) { return; }                     // options: nothing to set
    if (is(m, len, "DCLP")) { return; }                     // no clipboard on a HID device
    if (is(m, len, "CCLP")) { return; }                     // clipboard-grab notice: ignore
    if (is(m, len, "CSEC")) { return; }                     // screensaver
    if (is(m, len, "LSYN")) { return; }                     // language sync (1.8+)

    if (is(m, len, "CINN")) {                               // pointer entered this screen
        if (len >= 12) {
            absX_ = (int16_t)rd16(m + 4);
            absY_ = (int16_t)rd16(m + 6);
            haveAbs_ = true;
        }
        hasFocus_ = true;
        return;
    }

    if (is(m, len, "COUT")) {                               // pointer left this screen
        // Whatever was held when the pointer left must not stay held: the
        // controller has stopped sending us key-up events.
        hid_.releaseAll();
        hasFocus_ = false;
        return;
    }

    if (is(m, len, "DMMV") && len >= 8) {                   // absolute move
        ++nMove_;
        absX_ = rdS16(m + 4);
        absY_ = rdS16(m + 6);
        haveAbs_ = true;                                    // emitted after the drain
        return;
    }

    if (is(m, len, "DMRM") && len >= 8) {                   // relative move
        ++nMove_;
        relDx_ += rdS16(m + 4);
        relDy_ += rdS16(m + 6);
        return;
    }

    if ((is(m, len, "DMDN") || is(m, len, "DMUP")) && len >= 5) {
        const bool down = is(m, len, "DMDN");
        MouseButton b;
        switch (m[4]) {                                     // 1 left, 2 middle, 3 right
            case 1:  b = MouseButton::Left;   break;
            case 2:  b = MouseButton::Middle; break;
            case 3:  b = MouseButton::Right;  break;
            default: return;
        }
        ++nBtn_;
        // Flush first, blocking: a click has to happen where the pointer now
        // is, not where it was before the pending motion was applied.
        flushPointer(true);
        if (down) hid_.mouseButtonDown(b); else hid_.mouseButtonUp(b);
        return;
    }

    if (is(m, len, "DMWM") && len >= 8) {                   // wheel: x then y delta
        const int16_t xd = rdS16(m + 4), yd = rdS16(m + 6);
        // The protocol works in units of 120 per detent, as Windows does.
        if (yd) hid_.mouseWheel(yd / 120 ? yd / 120 : (yd > 0 ? 1 : -1));
        if (xd) hid_.mousePan(xd / 120 ? xd / 120 : (xd > 0 ? 1 : -1));
        return;
    }

    // DKDL is protocol 1.8's key-down: a distinct wire code, not a variant of
    // DKDN (kMsgDKeyDownLang = "DKDL%2i%2i%2i%s"). A 1.8 server sends only
    // DKDL for key-down and never DKDN, which is why key-ups arrived alone.
    // The first three fields sit at the same offsets in both; DKDL appends a
    // length-prefixed language string we have no use for.
    if (is(m, len, "DKDL") || is(m, len, "DKDN") || is(m, len, "DKUP")) {
        ++nKey_;
        const bool down = is(m, len, "DKDL") || is(m, len, "DKDN");
        {
            char *dst = down ? lastKeyDownRaw_ : lastKeyRaw_;
            size_t n = len < 16 ? len : 16;
            char *w = dst;
            for (size_t i = 0; i < n && (w - dst) < 36; ++i) w += snprintf(w, 4, "%02x", m[i]);
            *w = '\0';
        }
        if (len < 10) {
            snprintf(lastUnhandled_, sizeof(lastUnhandled_), "K%u", (unsigned)len);
            return;
        }
        const uint16_t keyId  = rd16(m + 4);
        const uint16_t button = rd16(m + 8);

        if (down) {
            uint8_t code = keyIdToHid(keyId);
            if (code == 0) {
                snprintf(lastUnhandled_, sizeof(lastUnhandled_), "k%04x", (unsigned)keyId);
                return;
            }
            // Remember which HID key this physical button produced, because the
            // matching key-up will not say.
            rememberKey(button, code);
            hid_.keyDown(code);
        } else {
            // Key-up carries KeyID 0 and identifies the key by button alone.
            uint8_t code = forgetKey(button);
            if (code == 0) code = keyIdToHid(keyId);   // fall back if we missed the down
            if (code == 0) {
                snprintf(lastUnhandled_, sizeof(lastUnhandled_), "u%04x", (unsigned)button);
                return;
            }
            hid_.keyUp(code);
        }
        return;
    }

    if (is(m, len, "DKRP") && len >= 12) {                  // auto-repeat
        const uint16_t keyId = rd16(m + 4);
        const uint8_t code = keyIdToHid(keyId);
        // The target does its own auto-repeat once a key is held, so a repeat
        // message needs no action; re-sending would double it.
        (void)code;
        return;
    }

    if (is(m, len, "CBYE")) { disconnect("server closed the session"); return; }
    if (is(m, len, "EBSY")) { disconnect("screen name already in use"); return; }
    if (is(m, len, "EUNK")) {
        // The commonest setup mistake, so say what to do about it.
        char msg[80];
        snprintf(msg, sizeof(msg), "server has no screen named '%s'", config_.deskflowScreen());
        disconnect(msg);
        backoffMs_ = 30000;     // no point retrying hard; this needs a human
        return;
    }
    if (is(m, len, "EBAD")) { disconnect("server reported a protocol violation"); return; }
    if (is(m, len, "EICV")) { disconnect("incompatible protocol version"); return; }

    // Anything else: remember the code so an unexpected message is visible
    // rather than silently ignored.
    ++nOther_;
    {
        size_t n = len < 16 ? len : 16;
        char *w = lastOtherRaw_;
        for (size_t i = 0; i < n && (w - lastOtherRaw_) < 36; ++i) {
            w += snprintf(w, 4, "%02x", m[i]);
        }
        *w = '\0';
    }
}

void DeskflowClient::rememberKey(uint16_t button, uint8_t hid) {
    for (size_t i = 0; i < heldByButtonCount_; ++i) {
        if (heldByButton_[i].button == button) { heldByButton_[i].hid = hid; return; }
    }
    if (heldByButtonCount_ < kMaxHeld) {
        heldByButton_[heldByButtonCount_++] = {button, hid};
    }
}

uint8_t DeskflowClient::forgetKey(uint16_t button) {
    for (size_t i = 0; i < heldByButtonCount_; ++i) {
        if (heldByButton_[i].button == button) {
            const uint8_t hid = heldByButton_[i].hid;
            heldByButton_[i] = heldByButton_[--heldByButtonCount_];
            return hid;
        }
    }
    return 0;
}

void DeskflowClient::flushPointer(bool block) {
    if (haveAbs_) {
        const float w = config_.deskflowWidth() > 0 ? config_.deskflowWidth() : 1;
        const float h = config_.deskflowHeight() > 0 ? config_.deskflowHeight() : 1;
        const float x = (float)absX_ / w, y = (float)absY_ / h;
        if (block) {
            hid_.mouseMoveAbsolute(x, y);
            haveAbs_ = false;
        } else if (hid_.tryMouseMoveAbsolute(x, y)) {
            haveAbs_ = false;
        }
        // else: endpoint busy - leave it pending; the next pass sends the newest.
    }
    if (relDx_ != 0 || relDy_ != 0) {
        if (block) {
            hid_.mouseMove(relDx_, relDy_);
            relDx_ = relDy_ = 0;
        } else {
            // One report per pass; the remainder (and anything that arrives
            // meanwhile) is summed and sent on later passes.
            hid_.tryMouseMoveStep(relDx_, relDy_);
        }
    }
}

}  // namespace ghosthid
