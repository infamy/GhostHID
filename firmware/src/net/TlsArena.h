// A small reservation of large blocks for mbedTLS.
//
// A TLS session needs two allocations of roughly 16.7KB each, and they must be
// contiguous. That is easy at boot and unreliable later: by the time the web
// server has started and run for a while, the largest free block is around
// 13KB, so a session that drops cannot be re-established without a reboot.
//
// This does not reduce memory use. It takes those two allocations out of the
// fragmentation game: the blocks are reserved once, while the heap is whole,
// and handed to mbedTLS whenever it asks for something that size. Everything
// else falls through to the normal allocator.

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ghosthid {

class TlsArena {
public:
    // Reserves the blocks and installs the allocator hooks. Call early in
    // setup(), before anything has had a chance to fragment the heap, and only
    // when TLS is actually going to be used - the reservation is permanent.
    // Returns false if the memory could not be reserved, in which case mbedTLS
    // simply uses the ordinary allocator as before.
    static bool begin();

    static bool active();
    static size_t reservedBytes();
    // Blocks currently lent to mbedTLS. Should be 2 while a session is up.
    static size_t inUse();
};

}  // namespace ghosthid
