#include "TlsArena.h"

#include <Arduino.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/platform.h>

namespace ghosthid {
namespace {

// mbedTLS asks for MBEDTLS_SSL_IN_CONTENT_LEN (16384) plus record overhead for
// each of its input and output buffers. 17KB covers both with room to spare.
//
// Two blocks serves exactly one session, which is all this firmware can have:
// there is a single WiFiClientSecure and a single screen server.
constexpr size_t kBlockSize  = 17 * 1024;
constexpr size_t kBlockCount = 2;

// Anything smaller than this is a routine allocation - certificate parsing,
// session state - and is better served by the ordinary heap.
constexpr size_t kLargeThreshold = 8 * 1024;

struct Block {
    uint8_t *mem = nullptr;
    bool     taken = false;
};

Block             g_blocks[kBlockCount];
SemaphoreHandle_t g_lock = nullptr;
bool              g_active = false;
size_t            g_inUse = 0;

void *arenaCalloc(size_t n, size_t size) {
    const size_t want = n * size;

    if (g_active && want >= kLargeThreshold && want <= kBlockSize) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        for (Block &b : g_blocks) {
            if (!b.taken) {
                b.taken = true;
                ++g_inUse;
                xSemaphoreGive(g_lock);
                memset(b.mem, 0, want);       // calloc must return zeroed memory
                return b.mem;
            }
        }
        xSemaphoreGive(g_lock);
        // Both blocks lent out. That should not happen with one session, so
        // rather than fail, fall through to the ordinary heap - a wrong
        // assumption here should degrade, not break the connection.
    }
    return calloc(n, size);
}

void arenaFree(void *ptr) {
    if (ptr == nullptr) return;
    if (g_active) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        for (Block &b : g_blocks) {
            if (b.mem == ptr) {
                b.taken = false;
                if (g_inUse > 0) --g_inUse;
                xSemaphoreGive(g_lock);
                return;                       // ours: reclaim, never free()
            }
        }
        xSemaphoreGive(g_lock);
    }
    free(ptr);
}

}  // namespace

bool TlsArena::begin() {
    if (g_active) return true;

    g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) return false;

    for (Block &b : g_blocks) {
        b.mem = static_cast<uint8_t *>(malloc(kBlockSize));
        if (b.mem == nullptr) {
            // Give back whatever we did get; a partial reservation is worse
            // than none, because it fragments without solving anything.
            for (Block &c : g_blocks) { free(c.mem); c.mem = nullptr; }
            vSemaphoreDelete(g_lock);
            g_lock = nullptr;
            Serial.println("[tls] could not reserve buffers; using the normal heap");
            return false;
        }
    }

    if (mbedtls_platform_set_calloc_free(arenaCalloc, arenaFree) != 0) {
        for (Block &b : g_blocks) { free(b.mem); b.mem = nullptr; }
        vSemaphoreDelete(g_lock);
        g_lock = nullptr;
        Serial.println("[tls] allocator hook unavailable; using the normal heap");
        return false;
    }

    g_active = true;
    Serial.printf("[tls] reserved %u x %uKB for TLS buffers\r\n",
                  (unsigned)kBlockCount, (unsigned)(kBlockSize / 1024));
    return true;
}

bool   TlsArena::active()        { return g_active; }
size_t TlsArena::reservedBytes() { return g_active ? kBlockSize * kBlockCount : 0; }
size_t TlsArena::inUse()         { return g_inUse; }

}  // namespace ghosthid
