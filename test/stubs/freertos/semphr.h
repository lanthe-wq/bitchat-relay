// Minimal FreeRTOS semaphore surface for host compile-checking.
#ifndef STUB_SEMPHR_H
#define STUB_SEMPHR_H

#include "FreeRTOS.h"

typedef void* SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex();
int32_t           xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t);
int32_t           xSemaphoreGiveRecursive(SemaphoreHandle_t);

#endif  // STUB_SEMPHR_H
