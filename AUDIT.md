# Code audit — bitchat BLE mesh relay

End-to-end audit of `bitchat_relay.ino` (352 lines, ESP32 + NimBLE-Arduino 2.x),
covering correctness, concurrency, BLE API usage, protocol handling and
robustness for unattended long-running deployment.

**14 findings: 2 critical, 3 high, 4 medium, 5 low.** All are fixed in this
branch. Nine findings are behavioural bugs that would have degraded or killed a
deployed relay; the remaining five are robustness and observability issues.

## How this was verified

There is no ESP32 toolchain reachable from this environment, so the audit did
not rely on reading the NimBLE docs from memory:

- **Against real library source.** NimBLE-Arduino **2.3.6** was checked out and
  every API call in the sketch was read against the actual headers and
  implementation. Findings below cite `file:line`. This also disproved two
  suspicions I started with — see [Verified correct](#verified-correct), which
  matters as much as the bug list, because those are the places where a
  well-meaning "fix" would introduce a bug.
- **Executable tests.** The packet decision logic was extracted into
  `relay_core.h` and is covered by 57 host-run checks in
  `test/test_relay_core.cpp`. Two of them reimplement the *old* decision path
  and assert that it misbehaves, so S4 and S7 are demonstrated rather than
  argued.
- **Compile check.** `test/stubs/` mirrors the verified 2.3.6 signatures closely
  enough to type-check the sketch on a host with `-Wall -Wextra -Wpedantic
  -Wshadow`. This is the check that catches the 1.x/2.x API drift the README
  warns about.

```
cd test && make check      # compile-check + 57 logic tests
```

**Not covered:** radio behaviour. Nothing here substitutes for the
`STRICT_HEADER_LOGGING` bring-up procedure in the README on real hardware with
real phones. Also uncovered: whether the UUIDs and offsets still match current
bitchat. They were verified for internal consistency, not against a live app —
the upstream repo is outside this environment's network policy.

---

## S0 — Blocker: the repository contained no source code

`bitchat_relay.ino` was deleted in `0d9323b` ("Delete bitchat_relay.ino"), the
most recent commit, leaving only `README.md` and the field manual PDF. The
README refers to the sketch as the thing you open, configure and flash in six
places, so the repository as published could not be used at all.

The audited code was recovered from `0d9323b^`. It was uploaded once
(`e0f1828`) and never modified, so there is a single version and no divergence
to reconcile. This branch restores it, fixed.

If the deletion was deliberate — a pending re-upload, say — then that intent is
worth recording somewhere, because from the outside it reads as an accident.

---

## Critical

### S1 — `NimBLEClient` slot leak permanently kills outbound relaying

`connectToPeer()` never freed the clients it created:

- normal disconnect → `onDisconnect` removed the link but left the client object
  alive;
- peer had no bitchat service or characteristic → `c->disconnect()` was called,
  but the client was never deleted.

`NimBLEDevice::createClient()` allocates into a fixed array and returns
`nullptr` once every slot is occupied — **and a disconnected client still
occupies its slot** (`NimBLEDevice.cpp:345-355`). So after
`MAX_CONNECTIONS` (9, after the README's `nimconfig.h` step) cumulative
outbound peers had come and gone, `createClient()` returned `nullptr` forever.

The relay does not crash or log anything alarming. It keeps advertising, keeps
accepting inbound phones, and silently stops connecting outward — exactly half
its job, permanently, until power-cycled. On an always-on node in a window,
nine peer churns is hours, not months. This is the strongest candidate for the
two symptoms the README already documents as mysteries: `no free client slots`
and "heap falls steadily".

**Fix:** `c->setSelfDelete(true, true)` hands the client's lifetime to the
stack, covering the disconnect path, the connect-failure path and the
no-service path.

Note the interaction that makes the naive fix wrong: with
`deleteOnConnectFail` set, a failed `connect()` **already** self-deletes
(`NimBLEClient.cpp:292`). The original code's `NimBLEDevice::deleteClient(c)`
after a failed connect therefore had to be removed at the same time, or the fix
would have traded a slot leak for a double free.

### S2 — Link table shared between two tasks with no synchronisation

`links` and `connectedAddrs` were plain `std::vector`s mutated from two
different FreeRTOS tasks:

| Access | Task |
|---|---|
| `addLink` via `connectToPeer` | Arduino `loop()` |
| `addLink` / `removeLink` via BLE callbacks | NimBLE host task |
| `forwardToAllExcept` iterating `links` | NimBLE host task |
| `links.size()` in the heartbeat | Arduino `loop()` |

A `push_back` from `loop()` that reallocates while the host task is midway
through `for (auto& l : links)` leaves that loop walking freed memory, and the
next thing it does with the dangling entry is dereference `l.remoteChr` and
write to the radio. The window is small but it is hit precisely when the relay
is busiest — a new peer joining while traffic is flowing — and the failure is a
reboot, not a log line.

**Fix:** a recursive mutex guards all shared state, with a timeout so a BLE
callback drops a packet rather than deadlocking. Two constraints shaped the
design and are commented in the code:

- `loop()` must never hold the mutex across a blocking BLE call. `connect()`
  can block for seconds; holding the lock through it would stall the entire
  host task. `connectToPeer()` does all blocking work unlocked and takes the
  lock only to publish the finished link.
- Forwarding *does* hold the lock across the sends, deliberately, so a link
  cannot be torn down mid-iteration. The only contender is `loop()`, which
  holds it for microseconds.

---

## High

### S3 — Connect target could be overwritten mid-connect

```c
if (doConnect) {
  doConnect = false;
  NimBLEDevice::getScan()->stop();
  connectToPeer(targetAddr);        // by reference to the global
```

`connectToPeer` took `const NimBLEAddress&` bound to the global `targetAddr`,
and the gate was cleared *before* the call. A scan result arriving on the host
task during the connect — `stop()` does not retroactively cancel already-queued
events — re-armed `targetAddr` while it was being read, so the relay could
connect to a peer it never selected and then record the wrong address in
`connectedAddrs`, corrupting the outbound dedupe for as long as that link
lived.

**Fix:** copy the address into a local *before* clearing the gate, and pass by
value.

### S4 — No protocol version check: the relay rewrote and rebroadcast non-v1 frames

`OFF_VERSION` was defined and printed in the header dump, but never tested.
Any frame of 14+ bytes arriving on the characteristic had **byte 2 overwritten**
and was rebroadcast on every other link.

Byte 2 is only TTL *by v1 convention*. In a hypothetical v2 layout it could be
anything, and the relay would corrupt one byte of every v2 packet it touched
and spread the result across the mesh. That is meaningfully worse than not
relaying: a silent relay is a dead node, whereas this is a node actively
damaging traffic that healthy peers would have carried correctly.

It also contradicts the README's own promise that a protocol change makes the
relay "go deaf rather than erroring".

**Fix:** drop unless `buf[OFF_VERSION] == 1`. Proven by
`test/test_relay_core.cpp` → *regression: version check*, which asserts the new
code drops a v2 frame **and** that the legacy path forwarded it with byte 2
rewritten to 6.

### S5 — MTU-oblivious forwarding silently truncated packets

`forwardToAllExcept` sent the full packet on every egress link regardless of
that link's negotiated ATT MTU. `DESIRED_MTU` is only a *preference*: a link
that has not finished MTU exchange, or whose peer refuses 517, sits at the
23-byte BLE default and can carry **20** payload bytes — less than the 22-byte
minimum bitchat frame.

Oversized notifications do not fail loudly, they truncate. The destination
receives a frame whose declared `payloadLen` no longer matches its body, i.e.
the relay manufactures corrupt packets and the log still reports success. The
window is widest immediately after a phone connects, which is exactly when the
README tells you to stand next to the relay and trust the log.

**Fix:** compute per-link capacity from the live MTU
(`NimBLEServer::getPeerMTU` for inbound, `NimBLEClient::getMTU` for outbound),
skip links that cannot carry the packet intact, and report them separately as
`skipped-mtu=N`.

---

## Medium

### S6 — Advertising built in the wrong order, demoting the service UUID out of the ADV packet

```c
adv->setName(DEVICE_NAME);                 // 1
adv->addServiceUUID(BITCHAT_SERVICE_UUID); // 2
adv->enableScanResponse(true);             // 3  <-- too late
```

`setName()` only diverts the name into scan-response data when scan response is
*already* enabled (`NimBLEAdvertising.cpp:517-529`), and it defaults to
disabled (`NimBLEAdvertising.cpp:43`). So at step 1 the 13-character name
consumed 15 bytes of the 31-byte advertisement. At step 2 the 128-bit service
UUID needed 18 more — 33 > 31 — so `addServiceUUID` failed on the
advertisement and fell back to stuffing it into the scan response instead
(`NimBLEAdvertising.cpp:408-417`, budget enforced at
`NimBLEAdvertisementData.cpp:39-47`).

Net effect: the relay advertised its **name** and hid its **service UUID** in
the scan response — backwards. It still worked for the relay's own active
scanning, which is why it would survive a bring-up test between two relays.
What it breaks is discovery by phones: a passive scanner never sees the UUID at
all, and iOS matches background scan filters against the advertisement packet,
so a bitchat phone with the app backgrounded may never find the relay. That is
the deployed case — a relay in a window exists to be found by phones in
pockets.

This is a strong candidate for the README's `links=0 with a phone right there`.

**Fix:** call `enableScanResponse(true)` **first**. The name then goes to the
scan response and the service UUID keeps its place in the advertisement, at 18
of 31 bytes. `addServiceUUID`'s return value is now also checked, since the
whole failure mode here was a silent `false`.

### S7 — Dedup bypass for sub-22-byte frames allowed an unbounded relay loop

`dedupKey()` returned `0` for any frame shorter than 22 bytes, and
`alreadySeen()` treated `0` as "cannot key it — let it through" *without
caching it*. Frames of 14–21 bytes were therefore never deduplicated at all.

Two relays in range of each other would bounce such a frame back and forth,
each hop only bounded by TTL — and with fanout to N links per hop the growth is
multiplicative, not linear. Well-formed bitchat packets are always ≥22 bytes,
so this is not reachable by accident; it is reachable by anyone who can write
to an unauthenticated characteristic, which is anyone in radio range. A single
16-byte write, and the relay pair amplifies it.

**Fix:** require `len >= 22` to relay at all, which also makes every relayable
packet keyable by construction, and carry validity in the verdict rather than
overloading a sentinel key of `0`. Proven by *regression: short-frame dedup
bypass*, which shows the legacy path forwarding the same runt 50/50 times and
the new path 0/50, while a valid packet is still relayed exactly 1/50.

### S8 — Advertising restarted twice on every disconnect

`advertiseOnDisconnect(true)` makes the library call `startAdvertising()` as
soon as the disconnect callback returns (`NimBLEServer.cpp:423-425`), and the
sketch's `onDisconnect` also called `getAdvertising()->start()`. The second
call fails because advertising is already running, logging an error on every
disconnect and training the operator to ignore the error channel.

**Fix:** removed the manual restart from `onDisconnect`.

The near-identical call in `onConnect` is **kept**, and is not a bug —
`NimBLEServer.cpp:356-386` shows the library does not restart advertising after
a *successful* connection, so without it the relay would accept exactly one
inbound phone and then go invisible. That asymmetry is easy to "clean up"
wrongly, so it is commented in place.

### S9 — Deliveries to unsubscribed peers were counted as successful

`NimBLECharacteristic::notify(value, len, connHandle)` with an explicit handle
calls `ble_gattc_notify_custom` directly and **does not check whether the peer
subscribed** (`NimBLECharacteristic.cpp:266-287`). It returns success. Every
inbound link was therefore counted in `forwarded ... to N link(s)` whether or
not the peer had enabled notifications — so the one number the README tells you
to read during verification overstated reality, and a phone that connected but
never subscribed looked identical to a working link.

**Fix:** track subscription state via the characteristic's `onSubscribe`
callback, notify only subscribed handles, and report the rest as
`skipped-unsubscribed=N`.

---

## Low

### S10 — Two heap allocations per packet on the BLE host task

`NimBLEAttValue v = pChar->getValue()` copies by value
(`NimBLEValueAttribute.h`), then `std::vector<uint8_t> out(buf, buf + len)`
allocated again — up to 512 bytes each, twice per packet, inside the host task.
Fragmentation is the failure mode that matters for a node intended to run for
months, and it is indistinguishable from a leak in the `[stat]` heap line,
which the README already teaches you to watch.

**Fix:** one static scratch buffer. Safe without a lock because
`handleIncomingPacket` only ever runs on the host task — documented in the code
next to the buffer, since that invariant is what makes it safe.

### S11 — `payloadLen` was never validated (`OFF_PAYLOAD_LEN` was dead)

The constant was defined and never read, so a frame whose declared payload
length disagreed with the bytes actually received was relayed unchanged,
propagating corruption instead of stopping it.

**Fix:** reject frames shorter than `22 + payloadLen`. Deliberately
conservative: the check is **flag-independent**, because which optional fields
(recipient ID, signature) are present is encoded in the flags byte, and this
environment cannot reach upstream to confirm the flag bit assignments. Guessing
them and getting it wrong would drop *all* valid traffic — the worst possible
failure for a relay. The lower bound holds under every flag interpretation. The
matching upper bound is logged as a `[warn ]` rather than dropped, for the same
reason.

### S12 — Failed peers were retried in a hot loop

If a device advertised the bitchat service UUID without the service behind it,
`connectToPeer` returned before recording the address, so the next scan result
re-queued the same peer immediately, forever. Each iteration also leaked a
client slot (S1), so the two compounded: a single stale advertiser in range
could exhaust the relay's outbound capacity in seconds.

**Fix:** a 16-entry failure list with a 60-second backoff, pruned on the
heartbeat. Addresses are also *claimed before* connecting rather than after, so
the scanner cannot queue a peer that a connect is already in flight for.

### S13 — 90 % scan duty cycle starved established links

`setInterval(100)` / `setWindow(90)` are milliseconds in 2.x
(`NimBLEScan.cpp:264-274`), so the radio spent 90 % of its time scanning while
also servicing up to nine connections and advertising. Being mains-powered
makes an aggressive duty cycle *affordable*, which is what the original comment
observed, but not *free* — the radio is still a single shared resource. This is
a plausible contributor to the README's "connects then drops repeatedly".

**Fix:** 64 ms window / 160 ms interval (40 %). Still finds peers within a
second or two.

### S14 — Silent drops were invisible

Every drop path was a bare `return`. With `STRICT_HEADER_LOGGING 0` — the
setting the README tells you to deploy with — a relay dropping 100 % of traffic
looked exactly like a quiet mesh.

**Fix:** a `[drop ]` line with the reason, on by default and independently
switchable. Duplicates are excluded, since in a working mesh they are the
common healthy case and would drown out everything else.

---

## Verified correct

Checked against 2.3.6 source and found **right**. Recorded because each one
looks wrong at a glance, and "fixing" any of them would introduce a bug.

- **In-place TTL mutation on signed packets.** Consistent with bitchat
  computing signatures excluding byte 2. The design comment is accurate.
- **`removeLink(c->getConnHandle())` inside the client's `onDisconnect`.** I
  expected the handle to be already cleared, which would have silently orphaned
  every outbound link. It is not: `NimBLEClient.cpp:973-976` calls
  `onDisconnect` **first** and clears `m_connHandle` after it returns. Safe as
  written.
- **`writeValue(buf, len, false)`.** Write-*without*-response is the correct and
  necessary choice, not laziness. The with-response path blocks the calling task
  on a semaphore, and this runs on the BLE host task — `true` would deadlock the
  stack under load.
- **Dedup key excludes TTL.** Required. Including the one byte every hop
  rewrites would make each hop a distinct packet and dedup would never fire.
- **`millis()` rollover.** `now - entry.at` on `uint32_t` is wrap-safe. Tested
  explicitly across the ~49.7-day boundary rather than assumed.
- **Dedup before the TTL check.** Correct ordering: a packet arriving already
  dead is still recorded, so a later copy of it is suppressed. Preserved, and
  now tested.
- **All 2.x callback signatures.** `onWrite`, `onSubscribe`, server
  `onConnect`/`onDisconnect`/`onMTUChange`, client `onConnect`/`onDisconnect`,
  and `onResult` all match the 2.3.6 headers exactly.
- **`getClient() const` on a `const NimBLERemoteService*`.** Compiles;
  `NimBLERemoteService.h:40` is const-qualified.

---

## Not changed, worth knowing

- **Dedup ring holds 256 entries against the client's 1000, and is FIFO rather
  than LRU.** A busy site can evict an entry still inside its 5-minute window
  and re-forward one duplicate. Bounded and harmless; the tradeoff is
  documented and pinned by a test rather than left as a surprise. Raise
  `DEDUP_CACHE_SIZE` if `[stat]` heap headroom allows.
- **A peer can hold two links to the relay** (inbound as peripheral, outbound as
  central), so it may receive the same packet twice. Its own dedup discards the
  copy. Wasteful, not incorrect; fixing it needs peer identity, which a relay
  deliberately does not track.
- **`STRICT_HEADER_LOGGING 1` still blocks the host task.** Unavoidable for a
  hex dump over 115200 baud, and it is already the documented bring-up-only
  setting. It is now one of three independent log switches, so drop logging can
  stay on in deployment while header logging goes off.
- **UUIDs and byte offsets are unverified against current bitchat.** Outside
  this environment's network policy. The README's verification procedure remains
  the only way to confirm them, and it is still mandatory after an app update.
- **The README's table of contents links to a `License` section that does not
  exist**, and there is no `LICENSE` file in the repository. Left alone
  deliberately: picking a license is the owner's call, not an audit fix. Worth
  resolving, though — an unlicensed repo is legally "all rights reserved" by
  default, which is an odd fit for a project whose whole premise is that two
  cheap relays beat one and that people should build more of them.
- **`Field Manual — bitchat BLE Mesh Relay.pdf`** is documentation, not code,
  and was not audited. Its text could not be extracted here — it embeds subset
  fonts with custom encodings — so `README.md` was treated as the canonical
  specification. If the two disagree, the PDF may need a refresh.
