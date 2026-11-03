#pragma once
struct StaticSemaphore_t {};
using SemaphoreHandle_t = StaticSemaphore_t*;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
inline bool xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return true; }
inline bool xSemaphoreGive(SemaphoreHandle_t) { return true; }
