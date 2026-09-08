// Sealed-mode serial control.
//
// Hard TinyUSB constraint discovered on-hardware: an interface can only be added
// to the USB descriptor BEFORE tinyusb_init() (which runs at USB.begin()).
// tinyusb_enable_interface() refuses once USB has started, and USBCDC's very
// constructor is what calls it. So whether the device has a serial port is decided
// once, at boot, by whether a USBCDC object exists before USB.begin() - it cannot
// be toggled at runtime. Serial state therefore changes only across a reboot.
//
// This build sets CDC_ON_BOOT=0 and constructs the USBCDC ITSELF, at C++ static-
// init time (before setup(), before USB.begin()), and only when the device should
// expose serial: unsealed, or during a one-shot "unseal window" a BOOT hold
// requested before rebooting. Sealed with no window -> no USBCDC is constructed ->
// no CDC interface -> the device enumerates HID-only, with no port on the bus.
//
// SealAwareSerial is a Print facade over that optional USBCDC. When none exists
// (sealed) every Serial.* call is a safe no-op. This header is force-included into
// every S3 translation unit (platformio.ini `-include`) and remaps `Serial` to it,
// so existing code needs no rename. Force-inclusion reaches C sources too, hence
// the __cplusplus gate.

#pragma once

#ifdef __cplusplus

#include <Arduino.h>     // defines `Serial` (= UART0 when CDC_ON_BOOT=0); pulled first
#include <USBCDC.h>

// Derives from Stream (not just Print) so `&UsbSerial` is usable anywhere a
// `Stream*` is expected - the force-include remaps `Serial` across libraries too
// (e.g. Adafruit BusIO's debug Stream default), which would not compile against a
// Print-only facade.
class SealAwareSerial : public Stream {
public:
    // Called once at static init (see usb_serial.cpp) when a CDC should exist this
    // boot. Constructing the USBCDC is what registers the interface, so this is the
    // single gate between "serial present" and "HID-only".
    void attach(USBCDC *c)           { cdc_ = c; }
    bool present() const             { return cdc_ != nullptr; }

    void begin(unsigned long baud = 0) { if (cdc_) cdc_->begin(baud); }
    void end()                       { if (cdc_) cdc_->end(); }
    void setTxTimeoutMs(uint32_t t)  { if (cdc_) cdc_->setTxTimeoutMs(t); }
    int  available() override        { return cdc_ ? cdc_->available() : 0; }
    int  read() override             { return cdc_ ? cdc_->read() : -1; }
    int  peek() override             { return cdc_ ? cdc_->peek() : -1; }
    void flush() override            { if (cdc_) cdc_->flush(); }
    size_t write(uint8_t b) override { return cdc_ ? cdc_->write(b) : 0; }
    size_t write(const uint8_t *buf, size_t n) override {
        return cdc_ ? cdc_->write(buf, n) : 0;
    }
    explicit operator bool() const   { return cdc_ && static_cast<bool>(*cdc_); }

private:
    USBCDC *cdc_ = nullptr;
};

extern SealAwareSerial UsbSerial;

// Route the `Serial` name to our facade. Arduino.h above has already done its own
// `#define Serial Serial0`; override it here, after.
#ifdef Serial
#undef Serial
#endif
#define Serial UsbSerial

#endif  // __cplusplus
