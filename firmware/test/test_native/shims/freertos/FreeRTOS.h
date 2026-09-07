#pragma once
#include <stdint.h>
typedef void* TaskHandle_t;
inline TaskHandle_t xTaskGetHandle(const char*) { return nullptr; }
inline unsigned uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
