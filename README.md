# bitchat-relay

**A cheap, always-on ESP32 node that extends a [bitchat](https://github.com/permissionlesstech/bitchat) Bluetooth mesh.**

bitchat phones relay for each other, but phones move, sleep, and run out of battery. This is a ~$15 board that sits in a window on mains power and does the relaying job permanently.

> **Unofficial.** Not affiliated with, endorsed by, or maintained by the bitchat project. It speaks bitchat's wire protocol as an independent implementation.

---

## Contents

- [How it works](#how-it-works)
- [What it deliberately doesn't do](#what-it-deliberately-doesnt-do)
- [Repository layout](#repository-layout)
- [Hardware](#hardware)
- [Quick start](#quick-start)
- [Configuration](#configuration)
- [Verifying it works](#verifying-it-works)
- [Host checks](#host-checks)
- [Reading the serial log](#reading-the-serial-log)
- [Troubleshooting](#troubleshooting)
- [Protocol notes](#protocol-notes)
- [Limitations](#limitations)
- [Regulatory](#regulatory)
- [Credits](#credits)
- [License](#license)

---

## How it works

The ESP32 runs both BLE roles simultaneously:

- **Peripheral** — advertises bitchat's service UUID, so real phones running bitchat discover it and connect like they would to any other peer.
- **Central** — scans for other bitchat peers and connects outward to them.

Every packet arriving on one link is checked for duplicates, has its TTL decremented, and is re-sent on every *other* link.

### The one byte that matters

Every bitchat packet opens with a fixed 14-byte header:

```
byte  00     01     02      03 ─────── 10      11      12-13     14 ─────── 21     22 →
     ┌──────┬──────┬──────┬──────────────────┬───────┬─────────┬────────────────┬──────────┐
     │ VER  │ TYPE │ TTL  │    TIMESTAMP     │ FLAGS │   LEN   │   SENDER ID    │ PAYLOAD  │
     └──────┴──────┴──▲───┴──────────────────┴───────┴─────────┴────────────────┴──────────┘
                      │
              the only byte a relay ever writes
```

bitchat computes packet signatures **excluding** the TTL byte, precisely so that relays can decrement it in place without invalidating anything. So the relay copies the buffer, changes byte 2, and forwards it — everything else passes through untouched.

## What it deliberately doesn't do

A relay is a dumb pipe. This one does **not**:

- **Decrypt anything.** Private messages stay opaque Noise ciphertext. The relay has no keys and wants none.
- **Verify signatures.** That's the destination's job.
- **Reassemble fragments.** Each fragment is relayed as its own packet.

This is a security property, not a shortcut. A compromised or buggy relay can drop your traffic, but it cannot read it.

## Repository layout

```
bitchat_relay.ino    the sketch — BLE plumbing, config at the top
relay_core.h         packet decision: header parsing, dedup, TTL policy
test/                host checks, no ESP32 required
AUDIT.md             code audit findings and what changed
```

Arduino IDE compiles `relay_core.h` automatically as long as it sits next to the
`.ino`. There is nothing extra to configure.

The split exists because a relay fails *silently*. A wrong byte offset or an
off-by-one in the dedup cache doesn't crash — it quietly drops or mangles
traffic, and you find out when you need the mesh. Keeping that logic free of BLE
types means it can be run against known-good and deliberately malformed packets
on a laptop.

## Hardware

| | Part | Search for | Notes |
|---|---|---|---|
| **Required** | ESP32 with external antenna socket | `ESP32-DevKitC-32UE`, `ESP32-WROOM-32U` | The **U** suffix means a U.FL/IPEX socket instead of a printed antenna. Without it you get little range advantage over a phone. |
| **Required** | 2.4 GHz antenna | `2.4GHz 6dBi antenna U.FL IPEX` | Often bundled with "U" boards. Cheapest range upgrade available. |
| **Required** | USB **data** cable | matching your board | Must carry data, not just power. |
| **Required** | 5V USB supply | any phone charger, 1A+ | For permanent deployment. |
| Optional | SSD1306 OLED | `0.96" OLED I2C SSD1306 128x64` | Status display. Not wired up in this sketch — add once the relay itself works. |
| Optional | IP65 box + cable gland | `IP65 junction box`, `PG7 gland` | Outdoor mounting. Keep the antenna outside the box. |

**No soldering required.** The minimum build is: clip on antenna, plug in USB.

⚠️ **SMA vs RP-SMA** look identical and don't mate properly. Buy the pigtail and antenna as a matched pair.

## Quick start

### 1. Arduino IDE setup

Install [Arduino IDE 2.x](https://www.arduino.cc/en/software), then:

**File → Preferences → Additional boards manager URLs:**
```
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```

**Tools → Board → Boards Manager** → install `esp32` by Espressif Systems.

**Tools → Manage Libraries** → install `NimBLE-Arduino` by h2zero, **version 2.x** (2.5.0+).

> ⚠️ **The NimBLE version matters.** This sketch is written for **2.x**. Version 1.x changed several callback signatures and will produce a wall of compiler errors. If you see errors mentioning `onResult`, `NimBLEAdvertisedDeviceCallbacks`, or `setPower`, that's the cause.

### 2. Raise the connection limit

NimBLE defaults to **3** simultaneous connections. A relay needs inbound links from phones *and* outbound links to peers at the same time, so 3 runs out fast. ESP32 supports up to 9.

Open `nimconfig.h` in your NimBLE library folder:

| OS | Path |
|---|---|
| Windows | `Documents\Arduino\libraries\NimBLE-Arduino\src\` |
| macOS | `~/Documents/Arduino/libraries/NimBLE-Arduino/src/` |
| Linux | `~/Arduino/libraries/NimBLE-Arduino/src/` |

Search the file for `MAX_CONNECTIONS`, change the value to `9`, make sure the line isn't commented out, save, and **restart the IDE**.

> Search for the string rather than pasting a define — the exact macro name has changed between NimBLE versions, and a mismatched paste silently does nothing.

**PlatformIO users** — skip the file edit and use:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps = h2zero/NimBLE-Arduino@^2.5.0
build_flags = -D CONFIG_BT_NIMBLE_MAX_CONNECTIONS=9
monitor_speed = 115200
```

### 3. Flash

1. Open `bitchat_relay.ino`
2. **Tools → Board** → `ESP32 Dev Module` (or `ESP32S3 Dev Module` for an S3)
3. **Tools → Port** → select your board
4. Upload
5. **Tools → Serial Monitor**, set to **115200**

If upload stalls at `Connecting........`, hold the **BOOT** button until writing starts.

A healthy boot:

```
=== bitchat relay booting ===
[srv ] advertising as bitchat peer
[scan] scanning for bitchat peers
[stat] links=0  outbound=0  heap=213...
```

`links=0` is correct at this point — there's nothing to talk to yet.

## Configuration

At the top of `bitchat_relay.ino`:

| Constant | Default | Purpose |
|---|---|---|
| `BITCHAT_SERVICE_UUID` | `F47B5E2D-…-8E1D2C3A4B5C` | **Production** mesh. Testnet ends `…4B5A`. Mixing them means you'll never see real peers. |
| `BITCHAT_CHAR_UUID` | `A1B2C3D4-…-0E1F2A3B4C5D` | Message transfer characteristic. |
| `DEVICE_NAME` | `bitchat-relay` | Advertised name. Goes in the scan response, so the service UUID keeps its place in the advertisement itself. |
| `TX_POWER_DBM` | `9` | Max on most ESP32s. NimBLE 2.x takes plain dBm, **not** `ESP_PWR_LVL_*`. |
| `DESIRED_MTU` | `517` | Gives a 512-byte payload, matching bitchat's max. A *preference* — links that negotiate less are skipped for packets they can't carry intact, rather than being sent a truncated one. |
| `STRICT_HEADER_LOGGING` | `1` | Dumps raw header bytes. **Leave on for bring-up, turn off for deployment.** |
| `LOG_DROPS` | `1` | Logs every packet not forwarded, and why. Cheap — leave it on. Without it, a relay dropping everything looks exactly like a quiet mesh. |
| `STRICT_LENGTH_CHECK` | `1` | Rejects frames whose declared `payloadLen` can't fit in the frame received. Set to `0` only if a protocol change makes it over-strict. |
| `DEDUP_CACHE_SIZE` | `256` | Seen-packet ring. The real client keeps 1000; raise this if `[stat]` shows heap to spare and you see duplicates re-forwarded. |
| `SCAN_INTERVAL_MS` / `SCAN_WINDOW_MS` | `160` / `64` | 40 % scan duty cycle. The radio is shared with every live connection — scanning harder is what makes a relay drop the links it already has. |
| `FAILURE_BACKOFF_MS` | `60000` | How long to leave a peer alone after a failed connect, so a device advertising the UUID without the service can't pin the relay in a retry loop. |

Wire-format constants and the relay policy live in `relay_core.h`:

| Constant | Default | Purpose |
|---|---|---|
| `OFF_*`, `V1_HEADER_SIZE`, `SENDER_ID_SIZE` | — | Packet layout. Change these if the app's header changes. |
| `DENSE_LINK_THRESHOLD` / `DENSE_TTL_CAP` | `6` / `5` | Simplified version of the real client's density-based TTL clamping. |
| `DEDUP_WINDOW_MS` | `300000` | 5 minutes, matching the client. |

## Verifying it works

**Do not skip this.** A wrong byte offset doesn't crash — it silently drops or mangles everything, and you find out when you need the mesh.

With `STRICT_HEADER_LOGGING 1`, stand next to the relay with bitchat open on a phone and send a public message:

```
[srv ] peer connected to us, handle=1
[link] + handle=1 role=peripheral  (total 1)
[srv ] MTU now 517 on handle 1
[srv ] handle=1 notifications on
[rx  ] handle=1 len=76 hdr=01 04 07 00 00 01 93 6F ...  | ver=1 type=4 ttl=7
[relay] forwarded 76 byte packet to 0 link(s)
```

Check four things:

- **`ver=1`** — protocol version parsed correctly. Something like `147` means your offsets are wrong.
- **`ttl=7`** — fresh packets start at 7. Values of 7/6/5 are all plausible; 200+ means byte 2 is being misread.
- **`notifications on`** — the phone subscribed, so the relay has somewhere to deliver. Without this line the link can only receive, not send.
- **`forwarded … to 0 link(s)`** — correct with one phone. There's nowhere else to send it.

**The real test:** two phones, separated until they can't see each other directly, relay in between. If they can now exchange messages and the log shows `forwarded … to 1 link(s)`, it works.

Then set `STRICT_HEADER_LOGGING 0` and re-flash — it's chatty and slows things under load. Leave `LOG_DROPS 1`: it's cheap, and it's the difference between a quiet mesh and a relay silently dropping everything.

## Host checks

Two checks run on any machine with `g++` and `make` — no board, no BLE stack:

```
cd test && make check
```

- **`make compile-check`** type-checks the sketch against stub headers that
  mirror the real NimBLE-Arduino 2.3.6 signatures. This catches the 1.x/2.x API
  drift that produces the compiler error wall described above, without waiting
  on the ESP32 toolchain.
- **`make run`** runs 57 checks over the packet decision logic: header parsing,
  the dedup window and its behaviour across the `millis()` rollover, TTL and
  density clamping, malformed and hostile frames, and a two-relay loop
  containment model.

Neither says anything about radio behaviour. They're a fast way to know you
haven't broken the parsing or the relay policy — the on-hardware verification
above is still mandatory.

## Reading the serial log

| Tag | Meaning |
|---|---|
| `[srv ]` | Peripheral role — someone connected **to** the relay |
| `[cli ]` | Central role — the relay connected **out** to someone |
| `[scan]` | A bitchat advertisement spotted nearby |
| `[link]` | Connection added to / removed from the link table |
| `[rx  ]` | Raw packet arrived (header logging only) |
| `[relay]` | Packet decremented and passed on |
| `[drop ]` | Packet **not** forwarded, with the reason (see below) |
| `[warn ]` | Frame length doesn't match its declared `payloadLen` — suspect the offsets have drifted from the app |
| `[stat]` | 15-second heartbeat: link count, outbound connections, free heap |

`[relay]` lines carry counters when something didn't go out cleanly:

| Counter | Meaning |
|---|---|
| `failed=N` | The BLE stack refused the send. Usually congestion. |
| `skipped-mtu=N` | That link's negotiated MTU is too small to carry the packet intact. Normal for a second or two after a peer connects, while MTU exchange completes. Persistent means a peer that won't negotiate up. |
| `skipped-unsubscribed=N` | A phone is connected but hasn't enabled notifications, so there's nowhere to deliver. |

`[drop ]` reasons, worst-first:

| Reason | Meaning |
|---|---|
| `bad-version` | Not a v1 packet. Byte 2 may not be TTL, so the relay won't touch it. Expect this after a protocol change — re-verify your offsets. |
| `short-payload` | Declared `payloadLen` doesn't fit in the bytes received. Truncated or misparsed. |
| `too-short` / `oversized` | Below 22 bytes or above 512. Not a valid bitchat frame. |
| `ttl-exhausted` | Would arrive dead. Normal and healthy. |
| `duplicate` | Already relayed. The common case in a working mesh, so it is **not** logged. |

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| No serial port at all | Charge-only cable, or missing driver | Swap cable first. Then install CP210x / CH340 driver. |
| `Failed to connect to ESP32` | Not in flashing mode | Hold **BOOT** during `Connecting......` |
| Serial monitor shows gibberish | Wrong baud | Set to 115200 |
| Errors re: `onResult` / `setPower` | NimBLE 1.x installed | Upgrade to 2.x |
| Boots fine, `links=0` forever | No peers in range — usually the answer | Stand next to it with the app open and foregrounded |
| `links=0` with a phone right there | Wrong service UUID, or protocol changed | Confirm UUID ends `…4B5C`; cross-check `BLEService.swift` |
| `no free client slots` | Connection limit still 3 | Redo the `nimconfig.h` step, restart IDE |
| Connects then drops repeatedly | Weak signal, supply browning out on TX, or scanning too hard | Test closer; try a better 5V supply; raise `SCAN_INTERVAL_MS` |
| `ver`/`ttl` are nonsense | Byte offsets no longer match the app | Check `BinaryProtocol.swift` upstream, update `OFF_*` in `relay_core.h` |
| Heap falls steadily | Fragmentation, or genuinely too many peers | Was a client leak before the audit — see `AUDIT.md` S1. If it recurs, capture the `[stat]` trend and the `[drop ]` reasons before power-cycling. |
| `[drop ] reason=bad-version` on everything | The app moved to a new protocol version | The relay is refusing to corrupt packets it can't parse. Re-verify offsets against `BinaryProtocol.swift`. |
| `[relay] ... skipped-mtu=N` persists | A peer won't negotiate a larger MTU | Nothing to do at the relay end; that link can only carry short frames. |
| `[relay] ... to 0 link(s)` with 2+ links | Peers connected but not subscribed | Check for `skipped-unsubscribed=N` on the same line. |

## Protocol notes

Constants in this sketch were derived from upstream source, not guessed:

| What | Where to verify |
|---|---|
| Packet format, header size, field order | [`bitchat/Protocols/BinaryProtocol.swift`](https://github.com/permissionlesstech/bitchat/blob/main/bitchat/Protocols/BinaryProtocol.swift) |
| Service & characteristic UUIDs | `bitchat/Services/BLE/BLEService.swift` |
| Relay / TTL / fanout policy | `RelayController.swift`, `BLEFanoutSelector.swift` |
| Overall protocol design | [`WHITEPAPER.md`](https://github.com/permissionlesstech/bitchat/blob/main/WHITEPAPER.md) |
| Independent wire-compatible implementation | [`dearabhin/bitchat-cli`](https://github.com/dearabhin/bitchat-cli) (Python) — useful for cross-checking offsets against working code rather than docs |
| NimBLE API | [h2zero.github.io/NimBLE-Arduino](https://h2zero.github.io/NimBLE-Arduino/) |

Key facts encoded in the sketch:

- v1 header is **14 bytes**; sender ID is **8 bytes**; all multi-byte fields are **big-endian**
- TTL lives at **byte offset 2** and starts at **7**
- Signatures **exclude** the TTL byte, so in-place decrement is safe
- Real client dedup: LRU seen-set, 1000 entries, 5-minute expiry, keyed by sender + timestamp + type + payload digest (this sketch approximates with a 256-entry ring buffer + FNV-1a)

## Limitations

- **A single relay does nothing on its own.** It only helps if bitchat users are near it. Mesh usefulness scales with node count — two cheap relays in two windows beat one expensive relay.
- **Not a walkie-talkie.** Bluetooth range is Bluetooth range. Kilometre reach needs a different radio (bridging to LoRa/Meshtastic is a much larger project).
- **Placement beats everything.** Height and clear sightlines outperform transmit power and antenna gain almost every time. A basement relay with a great antenna loses to an upstairs-window relay with the stock one.
- **The app can change.** bitchat is actively developed. If a future update changes the packet layout or UUIDs, this goes deaf rather than erroring. Re-verify with header logging after app updates.
- **Simplified relay policy.** The real client's density-based TTL clamping and deterministic fanout selection are approximated here, not reproduced exactly.

## Regulatory

2.4 GHz is unlicensed, which means "operate within the rules without applying" — not "no rules". US: FCC Part 15; most countries have a close equivalent.

- **Setting TX power to 9 dBm in software** — fine, that's the chip's rated max.
- **Higher-gain antenna** — generally fine for hobby use. Note gain isn't free: it flattens the radiation pattern, which usually helps street-level coverage but reduces it directly above and below.
- **External RF power amplifier** — this is where you can exceed legal EIRP limits and where a certified module stops being certified. Check local regulations first.
- **Deploying on property that isn't yours** — get permission.

## Credits

- [permissionlesstech/bitchat](https://github.com/permissionlesstech/bitchat) — the protocol and the app. All wire-format credit belongs upstream.
- [h2zero/NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino) — the BLE stack that makes simultaneous central+peripheral practical on ESP32.
- [dearabhin/bitchat-cli](https://github.com/dearabhin/bitchat-cli) — independent Python implementation, useful as a second reference for the wire format.

