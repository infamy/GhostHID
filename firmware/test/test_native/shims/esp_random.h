#pragma once
#include <stdint.h>
// Deterministic for tests; provisioning just needs *some* value.
inline uint32_t esp_random() { static uint32_t n = 2463534242u; n ^= n<<13; n^=n>>17; n^=n<<5; return n; }
