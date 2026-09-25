#include "FastTlsClient.h"

#include <string.h>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

namespace ghosthid {

void FastTlsClient::enableRxBuffer() {
    if (!sslclient || sslclient->socket < 0) return;
    rx_.fd = sslclient->socket;
    rx_.head = rx_.tail = 0;
    mbedtls_ssl_set_bio(&sslclient->ssl_ctx, &rx_, bioSend, bioRecv, nullptr);
}

const char *FastTlsClient::cipherSuite() {
    if (!sslclient) return "?";
    return mbedtls_ssl_get_ciphersuite(&sslclient->ssl_ctx);
}

int FastTlsClient::bioSend(void *ctx, const unsigned char *buf, size_t len) {
    return mbedtls_net_send(&static_cast<Rx *>(ctx)->fd, buf, len);
}

int FastTlsClient::bioRecv(void *ctx, unsigned char *buf, size_t len) {
    Rx *rx = static_cast<Rx *>(ctx);
    if (rx->head == rx->tail) {
        // One socket call for everything waiting. The socket is non-blocking,
        // so an empty one returns WANT_READ; that, EOF (0) and errors pass
        // straight through exactly as the unbuffered callback reported them.
        const int n = mbedtls_net_recv(&rx->fd, rx->buf, sizeof(rx->buf));
        if (n <= 0) return n;
        rx->head = 0;
        rx->tail = (uint16_t)n;
    }
    size_t k = (size_t)(rx->tail - rx->head);
    if (k > len) k = len;
    memcpy(buf, rx->buf + rx->head, k);
    rx->head += (uint16_t)k;
    return (int)k;
}

}  // namespace ghosthid
