// Minimal FreeRTOS surface for host compile-checking. See stubs/README.md.
#ifndef STUB_FREERTOS_H
#define STUB_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;

#define pdTRUE  1
#define pdFALSE 0

#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))

#endif  // STUB_FREERTOS_H
