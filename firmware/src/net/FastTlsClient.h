// GhostHID - TLS client with a buffered receive path for the screen client.
//
// mbedTLS pulls each record off the socket in pieces - the 5-byte header, then
// the body - and every piece is a separate lwIP socket call. From the KVM task
// (core 1) each of those waits on the tcpip thread (core 0), which is busiest
// exactly while pointer motion is streaming in. Deskflow sends one small record
// per mouse move, so that was measured at ~1.4ms per 12-byte message, most of a
// pass, and the reason moves queued up behind each other. (Measured afterwards:
// the socket calls themselves were ~40us each, so the bigger share of that
// time is mbedTLS record processing - this removes the round trips, not that.)
//
// enableRxBuffer() swaps in a receive callback that reads everything the socket
// has in ONE call and hands mbedTLS its pieces from memory. Same bytes, same
// order, same error semantics - only fewer round trips to the other core.

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <WiFiClientSecure.h>

namespace ghosthid {

#ifdef GHOSTHID_NATIVE_TEST
using FastTlsClient = WiFiClientSecure;   // host tests never open a socket
#else
class FastTlsClient : public WiFiClientSecure {
public:
    // Call after a successful connect(). A reconnect re-runs the framework's
    // setup, which restores its own callbacks, so call it again after each one.
    void enableRxBuffer();

    // The negotiated cipher suite, for the connect log.
    const char *cipherSuite();

private:
    // Handed to mbedTLS as its I/O context. `fd` MUST stay first: sends go to
    // mbedtls_net_send, which reads the context as an mbedtls_net_context (a
    // struct holding just the fd) - the same trick the framework itself uses.
    struct Rx {
        int      fd = -1;
        uint16_t head = 0, tail = 0;
        uint8_t  buf[2048];
    };
    Rx rx_;

    static int bioSend(void *ctx, const unsigned char *buf, size_t len);
    static int bioRecv(void *ctx, unsigned char *buf, size_t len);
};
#endif

}  // namespace ghosthid
