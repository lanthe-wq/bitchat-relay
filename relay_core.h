/* ============================================================================
 * relay_core.h — pure relay logic, no Arduino and no NimBLE dependencies
 * ============================================================================
 *
 * Everything in here is decidable from a byte buffer alone: header parsing,
 * the dedup cache, and the TTL / drop decision. It is deliberately free of
 * BLE and Arduino types so that it compiles and runs on a host machine, where
 * it is covered by test/test_relay_core.cpp.
 *
 * That split matters for this project specifically. A relay fails silently:
 * a wrong offset or an off-by-one in the dedup cache does not crash, it just
 * quietly drops or mangles traffic, and you find out when you need the mesh.
 * Being able to run the decision logic against known-good and deliberately
 * malformed frames on a laptop is the only cheap way to check it.
 *
 * PROTOCOL OFFSETS: from BinaryProtocol.swift in the bitchat repo
 * (v1HeaderSize = 14, senderIDSize = 8, all multi-byte fields big-endian).
 * These CAN change when the app updates — see the "Verify" section of the
 * README before trusting a deployed unit.
 * ========================================================================== */

#ifndef BITCHAT_RELAY_CORE_H
#define BITCHAT_RELAY_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace relay {

// ---------------------------------------------------------------------------
// WIRE FORMAT
// ---------------------------------------------------------------------------
static const uint8_t PROTOCOL_VERSION = 1;

static const size_t V1_HEADER_SIZE  = 14;  // version,type,ttl,timestamp(8),flags,payloadLen(2)
static const size_t SENDER_ID_SIZE  = 8;
static const size_t OFF_VERSION     = 0;
static const size_t OFF_TYPE        = 1;
static const size_t OFF_TTL         = 2;   // the ONLY byte we ever modify
static const size_t OFF_TIMESTAMP   = 3;   // 8 bytes, big-endian
static const size_t OFF_FLAGS       = 11;
static const size_t OFF_PAYLOAD_LEN = 12;  // 2 bytes, big-endian
static const size_t OFF_SENDER_ID   = V1_HEADER_SIZE;

// Smallest frame that can be routed at all: header + sender ID. Anything
// shorter cannot be keyed for dedup, and a packet we cannot key is a packet
// we must not relay (see VERDICT_TOO_SHORT).
static const size_t MIN_PACKET_SIZE = V1_HEADER_SIZE + SENDER_ID_SIZE;  // 22

// A v1 frame may carry, after the declared payload, an optional recipient ID
// and an optional Ed25519 signature. Which of those are present is encoded in
// the flags byte, and we deliberately do not interpret flag bits here — a
// relay that guesses wrong about flag semantics would drop valid traffic. We
// only use the total as a flag-independent upper sanity bound.
static const size_t MAX_OPTIONAL_TRAILER = 8 + 64;  // recipient ID + signature

// Largest frame we will buffer. bitchat's max payload is 512 bytes, which is
// what a 517-byte ATT MTU is chosen to carry.
static const size_t MAX_PACKET_SIZE = 512;

// ---------------------------------------------------------------------------
// RELAY POLICY
// The real client clamps TTL by link density: dense graphs (>= 6 links) cap
// broadcast TTL at 5, thin chains relay at full depth.
// ---------------------------------------------------------------------------
static const size_t   DENSE_LINK_THRESHOLD = 6;
static const uint8_t  DENSE_TTL_CAP        = 5;
static const uint32_t DEDUP_WINDOW_MS      = 5UL * 60UL * 1000UL;

// ---------------------------------------------------------------------------
// DECISION
// ---------------------------------------------------------------------------
enum Verdict {
  VERDICT_RELAY = 0,      // forward with Decision::newTtl in byte 2
  VERDICT_TOO_SHORT,      // below MIN_PACKET_SIZE — unroutable, unkeyable
  VERDICT_OVERSIZED,      // larger than we are willing to buffer
  VERDICT_BAD_VERSION,    // not v1: byte 2 may not even be TTL, do not touch it
  VERDICT_SHORT_PAYLOAD,  // declared payloadLen cannot fit in the frame
  VERDICT_DUPLICATE,      // already relayed inside the dedup window
  VERDICT_TTL_EXHAUSTED   // would arrive dead, nothing to gain by sending it
};

inline const char* verdictName(Verdict v) {
  switch (v) {
    case VERDICT_RELAY:         return "relay";
    case VERDICT_TOO_SHORT:     return "too-short";
    case VERDICT_OVERSIZED:     return "oversized";
    case VERDICT_BAD_VERSION:   return "bad-version";
    case VERDICT_SHORT_PAYLOAD: return "short-payload";
    case VERDICT_DUPLICATE:     return "duplicate";
    case VERDICT_TTL_EXHAUSTED: return "ttl-exhausted";
  }
  return "?";
}

struct Decision {
  Verdict verdict;
  uint8_t newTtl;   // only meaningful when verdict == VERDICT_RELAY
};

// ---------------------------------------------------------------------------
// HEADER ACCESSORS — callers must have checked len >= MIN_PACKET_SIZE first
// ---------------------------------------------------------------------------
inline uint16_t declaredPayloadLen(const uint8_t* buf) {
  return (uint16_t)(((uint16_t)buf[OFF_PAYLOAD_LEN] << 8) | buf[OFF_PAYLOAD_LEN + 1]);
}

inline uint64_t timestampOf(const uint8_t* buf) {
  uint64_t ts = 0;
  for (size_t i = 0; i < 8; i++) ts = (ts << 8) | buf[OFF_TIMESTAMP + i];  // big-endian
  return ts;
}

inline uint32_t fnv1a(const uint8_t* d, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
  return h;
}

// Identity of a packet, independent of how many hops it has taken.
//
// TTL is excluded on purpose: it is the one byte every relay rewrites, so
// including it would make each hop look like a brand new packet and dedup
// would never fire. Everything else that distinguishes one packet from
// another is folded in.
inline uint64_t dedupKey(const uint8_t* buf, size_t len) {
  const size_t restOff       = OFF_SENDER_ID + SENDER_ID_SIZE;
  uint32_t     senderDigest  = fnv1a(buf + OFF_SENDER_ID, SENDER_ID_SIZE);
  uint32_t     payloadDigest = (len > restOff) ? fnv1a(buf + restOff, len - restOff) : 0;
  return ((uint64_t)senderDigest << 32) ^ timestampOf(buf)
       ^ ((uint64_t)buf[OFF_TYPE] << 56) ^ payloadDigest;
}

// ---------------------------------------------------------------------------
// DEDUP CACHE
// Spec: LRU seen-set, 1000 entries, 5 min expiry, keyed by sender + timestamp
// + type + payload digest. We approximate with a fixed ring sized for ESP32
// RAM. Not bit-identical to the real client, but it kills relay loops.
// ---------------------------------------------------------------------------
template <size_t N>
class DedupCache {
 public:
  DedupCache() : next_(0) { clear(); }

  void clear() {
    memset(slots_, 0, sizeof(slots_));
    next_ = 0;
  }

  // Returns true if this key was already recorded inside the window.
  //
  // On a hit the stored timestamp is deliberately NOT refreshed: the window is
  // measured from when the packet was first seen, so a peer retransmitting the
  // same frame forever cannot keep it suppressed forever.
  bool seen(uint64_t key, uint32_t nowMs, uint32_t windowMs = DEDUP_WINDOW_MS) {
    for (size_t i = 0; i < N; i++) {
      if (slots_[i].used && slots_[i].key == key) {
        // Unsigned subtraction, so this stays correct across the ~49.7 day
        // millis() rollover.
        if ((uint32_t)(nowMs - slots_[i].at) < windowMs) return true;
        slots_[i].at = nowMs;   // expired: treat as new, restart its window
        return false;
      }
    }
    slots_[next_].key  = key;
    slots_[next_].at   = nowMs;
    slots_[next_].used = true;
    next_ = (next_ + 1) % N;
    return false;
  }

  size_t capacity() const { return N; }

  size_t occupancy() const {
    size_t n = 0;
    for (size_t i = 0; i < N; i++) if (slots_[i].used) n++;
    return n;
  }

 private:
  struct Slot { uint64_t key; uint32_t at; bool used; };
  Slot   slots_[N];
  size_t next_;
};

// ---------------------------------------------------------------------------
// THE RELAY DECISION
//
// Ordering is load-bearing:
//   1. size and version first, because every later step reads header fields
//      whose meaning is only defined for a v1 frame of routable length;
//   2. dedup before TTL, so that a packet which arrives already dead is still
//      recorded and a later copy of it does not get relayed;
//   3. TTL last, since it is the only step that produces a value.
// ---------------------------------------------------------------------------
template <size_t N>
inline Decision evaluate(DedupCache<N>& cache,
                         const uint8_t* buf,
                         size_t         len,
                         size_t         linkCount,
                         uint32_t       nowMs,
                         bool           strictLength = true,
                         uint32_t       windowMs     = DEDUP_WINDOW_MS) {
  Decision d;
  d.verdict = VERDICT_RELAY;
  d.newTtl  = 0;

  if (len < MIN_PACKET_SIZE)  { d.verdict = VERDICT_TOO_SHORT; return d; }
  if (len > MAX_PACKET_SIZE)  { d.verdict = VERDICT_OVERSIZED; return d; }

  // A non-v1 frame has an unknown layout, so byte 2 is not necessarily TTL.
  // Decrementing it would corrupt the packet, and forwarding a corrupted
  // packet is worse than not forwarding it.
  if (buf[OFF_VERSION] != PROTOCOL_VERSION) { d.verdict = VERDICT_BAD_VERSION; return d; }

  if (strictLength) {
    // Flag-independent lower bound: whatever optional fields are present, a
    // v1 frame must still be long enough to hold header + sender ID + the
    // payload length it declares. Shorter means truncated or misparsed.
    const size_t minimum = MIN_PACKET_SIZE + (size_t)declaredPayloadLen(buf);
    if (len < minimum) { d.verdict = VERDICT_SHORT_PAYLOAD; return d; }
  }

  if (cache.seen(dedupKey(buf, len), nowMs, windowMs)) {
    d.verdict = VERDICT_DUPLICATE;
    return d;
  }

  const uint8_t ttl = buf[OFF_TTL];
  if (ttl == 0) { d.verdict = VERDICT_TTL_EXHAUSTED; return d; }

  uint8_t newTtl = (uint8_t)(ttl - 1);
  if (linkCount >= DENSE_LINK_THRESHOLD && newTtl > DENSE_TTL_CAP) newTtl = DENSE_TTL_CAP;
  if (newTtl == 0) { d.verdict = VERDICT_TTL_EXHAUSTED; return d; }

  d.newTtl = newTtl;
  return d;
}

// Frame is longer than any legal combination of optional fields explains.
// Not a drop condition — trailing bytes are conceivable and dropping on a
// guess would be worse — but worth logging, since it usually means the
// offsets have drifted from the app.
inline bool trailerLooksWrong(const uint8_t* buf, size_t len) {
  if (len < MIN_PACKET_SIZE) return false;
  return len > MIN_PACKET_SIZE + (size_t)declaredPayloadLen(buf) + MAX_OPTIONAL_TRAILER;
}

}  // namespace relay

#endif  // BITCHAT_RELAY_CORE_H
