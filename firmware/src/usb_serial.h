// Sealed-mode serial control.
//
// The device must enumerate with NO USB serial (CDC) interface when sealed -
// "not on the bus" is less attack surface than "present but muted". Two facts
// force the shape of this:
//   1. ARDUINO_USB_CDC_ON_BOOT starts CDC before setup(), too early to gate on
//      the NVS flag - so this build sets CDC_ON_BOOT=0 and owns CDC itself.
//   2. USBCDC's *constructor* calls tinyusb_enable_interface(), i.e. merely
//      constructing a USBCDC adds the CDC interface to the descriptor. So the
//      object must not exist at all on a sealed boot.
//
// SealAwareSerial is a Print facade holding an optional USBCDC, created lazily on
// begin() and never before. Unsealed boots call begin() (before USB.begin()
// finalises the descriptor) -> CDC present. Sealed boots never call begin() ->
// no USBCDC is constructed, no CDC interface, HID-only enumeration. Every
// Serial.* call routes here and is a safe no-op while no CDC exists. The BOOT-hold
// unseal path calls begin() for the first time that boot, so the RX queue is
// created fresh and input works.
//
// This header is force-included into every S3 translation unit (platformio.ini
// `-include`) and remaps the `Serial` name to UsbSerial, so existing code needs no
// rename. Force-inclusion reaches C sources too, so everything is __cplusplus-gated.

#pragma once

#ifdef __cplusplus

#include <Arduino.h>     // defines `Serial` (= UART0 when CDC_ON_BOOT=0); pulled first
#include <USBCDC.h>

class SealAwareSerial : public Print {
public:
    // Lazily construct the CDC on first begin(). Constructing USBCDC is what adds
    // the interface to the USB descriptor, so this is the single gate between
    // "serial present" and "HID-only".
    void begin(unsigned long baud = 0) {
        if (cdc_ == nullptr) cdc_ = new USBCDC(0);
        cdc_->begin(baud);
    }
    void end()                       { if (cdc_) cdc_->end(); }
    void setTxTimeoutMs(uint32_t t)  { if (cdc_) cdc_->setTxTimeoutMs(t); }
    int  available()                 { return cdc_ ? cdc_->available() : 0; }
    int  read()                      { return cdc_ ? cdc_->read() : -1; }
    void flush()                     { if (cdc_) cdc_->flush(); }
    size_t write(uint8_t b) override { return cdc_ ? cdc_->write(b) : 0; }
    size_t write(const uint8_t *buf, size_t n) override {
        return cdc_ ? cdc_->write(buf, n) : 0;
    }
    // Whether a CDC exists and the host has it open. False while sealed.
    explicit operator bool() const   { return cdc_ && static_cast<bool>(*cdc_); }

private:
    USBCDC *cdc_ = nullptr;
};

extern SealAwareSerial UsbSerial;

// Route the `Serial` name to our facade. Arduino.h above has already done its own
// `#define Serial Serial0`; override it here, after, so every later use resolves
// to UsbSerial.
#ifdef Serial
#undef Serial
#endif
#define Serial UsbSerial

#endif  // __cplusplus
