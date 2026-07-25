# Compile-check stubs

These headers are **not** a BLE simulator. They are just enough of the Arduino,
FreeRTOS and NimBLE-Arduino 2.x surfaces to let a host compiler parse and
type-check `bitchat_relay.ino` without an ESP32 toolchain:

```
cd test && make compile-check
```

Every signature here was transcribed from the real **NimBLE-Arduino 2.3.6**
headers, so a clean compile means the sketch calls the 2.x API correctly —
which is the failure mode the README warns about most loudly, since 1.x
changed several of these signatures and the resulting error wall is the first
thing people hit.

What this does catch: wrong argument counts, wrong argument order, wrong types,
missing `override`, const-correctness, narrowing conversions.

What it does not catch: anything about actual radio behaviour. Only real
hardware and the `STRICT_HEADER_LOGGING` bring-up procedure in the README can
tell you the relay is really moving packets.

If NimBLE changes an API, fix it here too or the check silently stops being
meaningful.
