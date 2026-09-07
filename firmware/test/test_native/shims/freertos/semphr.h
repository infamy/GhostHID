#pragma once
typedef void* SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)1; }
inline int  xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return 1; }
inline int  xSemaphoreGive(SemaphoreHandle_t) { return 1; }
#define portMAX_DELAY 0xffffffff
