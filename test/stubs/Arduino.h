// Minimal Arduino surface for host compile-checking. See stubs/README.md.
#ifndef STUB_ARDUINO_H
#define STUB_ARDUINO_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cstdio>

class HardwareSerial {
 public:
  void begin(unsigned long) {}
  int  printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void println(const char*) {}
  void println() {}
};

inline int HardwareSerial::printf(const char*, ...) { return 0; }

extern HardwareSerial Serial;

class EspClass {
 public:
  uint32_t getFreeHeap() { return 0; }
};
extern EspClass ESP;

// On ESP32 `unsigned long` is 32 bits, so millis() is effectively uint32_t.
// Spelled that way here so the host compiler does not report a narrowing that
// cannot happen on the target.
uint32_t millis();
void     delay(uint32_t);

#endif  // STUB_ARDUINO_H
