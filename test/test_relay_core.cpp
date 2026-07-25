/* ============================================================================
 * test_relay_core.cpp — host tests for the relay decision logic
 * ============================================================================
 *
 * Builds and runs on a laptop, no ESP32 and no BLE stack required:
 *
 *     cd test && make
 *
 * Two things are covered here.
 *
 *   1. The current logic in relay_core.h behaves as documented, including on
 *      malformed input.
 *   2. The bugs found in the pre-audit version stay fixed. Those cases carry a
 *      LEGACY() reimplementation of the old decision path so the test proves
 *      the old code really was wrong rather than just asserting the new code is
 *      right. See AUDIT.md for the finding each one maps to.
 * ========================================================================== */

#include "../relay_core.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Tiny test harness
// ---------------------------------------------------------------------------
static int g_checks = 0;
static int g_failed = 0;
static std::string g_group;

static void group(const char* name) {
  g_group = name;
  printf("\n\033[1m%s\033[0m\n", name);
}

static void check(bool cond, const char* what) {
  g_checks++;
  if (cond) {
    printf("  \033[32mPASS\033[0m %s\n", what);
  } else {
    g_failed++;
    printf("  \033[31mFAIL\033[0m %s\n", what);
  }
}

static void checkVerdict(relay::Verdict got, relay::Verdict want, const char* what) {
  g_checks++;
  if (got == want) {
    printf("  \033[32mPASS\033[0m %s (%s)\n", what, relay::verdictName(got));
  } else {
    g_failed++;
    printf("  \033[31mFAIL\033[0m %s: want %s, got %s\n",
           what, relay::verdictName(want), relay::verdictName(got));
  }
}

// ---------------------------------------------------------------------------
// Packet builder — a well-formed v1 bitchat frame
// ---------------------------------------------------------------------------
struct Packet {
  std::vector<uint8_t> b;

  Packet(uint8_t type = 4, uint8_t ttl = 7, uint64_t ts = 0x0000019300000001ULL,
         size_t payloadLen = 10, uint8_t senderSeed = 0xAA, uint8_t payloadSeed = 0x10) {
    b.assign(relay::MIN_PACKET_SIZE + payloadLen, 0);
    b[relay::OFF_VERSION] = relay::PROTOCOL_VERSION;
    b[relay::OFF_TYPE]    = type;
    b[relay::OFF_TTL]     = ttl;
    for (size_t i = 0; i < 8; i++)
      b[relay::OFF_TIMESTAMP + i] = (uint8_t)(ts >> (8 * (7 - i)));  // big-endian
    b[relay::OFF_FLAGS] = 0;
    b[relay::OFF_PAYLOAD_LEN]     = (uint8_t)(payloadLen >> 8);
    b[relay::OFF_PAYLOAD_LEN + 1] = (uint8_t)(payloadLen & 0xFF);
    for (size_t i = 0; i < relay::SENDER_ID_SIZE; i++)
      b[relay::OFF_SENDER_ID + i] = (uint8_t)(senderSeed + i);
    for (size_t i = 0; i < payloadLen; i++)
      b[relay::MIN_PACKET_SIZE + i] = (uint8_t)(payloadSeed + i);
  }

  const uint8_t* data() const { return b.data(); }
  size_t         size() const { return b.size(); }
};

// ---------------------------------------------------------------------------
// LEGACY: the pre-audit decision path, transcribed from the original sketch.
// Present only so the regression tests can show the old behaviour.
// ---------------------------------------------------------------------------
namespace legacy {

struct SeenEntry { uint64_t key; uint32_t at; bool used; };
static const size_t   CACHE_SIZE = 256;
static SeenEntry      cache[CACHE_SIZE];
static size_t         next = 0;

static void reset() { memset(cache, 0, sizeof(cache)); next = 0; }

// Original dedupKey: returned 0 for any frame shorter than 22 bytes.
static uint64_t dedupKey(const uint8_t* buf, size_t len) {
  if (len < relay::OFF_SENDER_ID + relay::SENDER_ID_SIZE) return 0;
  return relay::dedupKey(buf, len);
}

// Original alreadySeen: a key of 0 was let through *and never cached*.
static bool alreadySeen(const uint8_t* buf, size_t len, uint32_t now) {
  uint64_t k = dedupKey(buf, len);
  if (k == 0) return false;
  for (size_t i = 0; i < CACHE_SIZE; i++) {
    if (cache[i].used && cache[i].key == k) {
      if (now - cache[i].at < relay::DEDUP_WINDOW_MS) return true;
      cache[i].at = now;
      return false;
    }
  }
  cache[next].key = k; cache[next].at = now; cache[next].used = true;
  next = (next + 1) % CACHE_SIZE;
  return false;
}

// Original handleIncomingPacket, minus the BLE calls. Returns true if the
// packet would have been forwarded, with newTtl written to out.
static bool wouldRelay(const uint8_t* buf, size_t len, size_t linkCount,
                       uint32_t now, uint8_t* out) {
  if (len < relay::V1_HEADER_SIZE) return false;   // only 14, not 22
  if (alreadySeen(buf, len, now))  return false;
  uint8_t ttl = buf[relay::OFF_TTL];
  if (ttl == 0) return false;
  uint8_t newTtl = ttl - 1;
  if (linkCount >= relay::DENSE_LINK_THRESHOLD && newTtl > relay::DENSE_TTL_CAP)
    newTtl = relay::DENSE_TTL_CAP;
  if (newTtl == 0) return false;
  *out = newTtl;
  return true;                                     // no version check at all
}

}  // namespace legacy

// ---------------------------------------------------------------------------
typedef relay::DedupCache<256> Cache;

int main() {
  printf("bitchat relay — core logic tests\n");

  // -------------------------------------------------------------------------
  group("header parsing");
  {
    Packet p(4, 7, 0x0102030405060708ULL, 10);
    check(relay::timestampOf(p.data()) == 0x0102030405060708ULL,
          "timestamp decodes big-endian");
    check(relay::declaredPayloadLen(p.data()) == 10,
          "payloadLen decodes big-endian");
    check(p.data()[relay::OFF_VERSION] == 1, "version at offset 0");
    check(p.data()[relay::OFF_TTL] == 7,     "ttl at offset 2");
    check(relay::MIN_PACKET_SIZE == 22,      "min routable frame is 22 bytes");

    // The offsets the README tells you to eyeball during bring-up.
    Packet big(4, 7, 0ULL, 512 - relay::MIN_PACKET_SIZE);
    check(big.size() == 512, "max frame is 512 bytes");
  }

  // -------------------------------------------------------------------------
  group("happy path");
  {
    Cache c;
    Packet p(4, 7);
    relay::Decision d = relay::evaluate(c, p.data(), p.size(), 1, 1000);
    checkVerdict(d.verdict, relay::VERDICT_RELAY, "fresh packet relays");
    check(d.newTtl == 6, "ttl 7 becomes 6");

    // Only byte 2 may change. Confirm the decision never asks for more.
    Packet q(4, 7);
    std::vector<uint8_t> before(q.b);
    Cache c2;
    relay::evaluate(c2, q.data(), q.size(), 1, 1000);
    check(memcmp(before.data(), q.data(), q.size()) == 0,
          "evaluate() does not mutate the input buffer");
  }

  // -------------------------------------------------------------------------
  group("ttl policy");
  {
    Cache c;
    Packet p1(4, 1, 111ULL);
    checkVerdict(relay::evaluate(c, p1.data(), p1.size(), 1, 1000).verdict,
                 relay::VERDICT_TTL_EXHAUSTED, "ttl 1 is not forwarded");

    Packet p0(4, 0, 222ULL);
    checkVerdict(relay::evaluate(c, p0.data(), p0.size(), 1, 1000).verdict,
                 relay::VERDICT_TTL_EXHAUSTED, "ttl 0 is dropped");

    Packet p2(4, 2, 333ULL);
    relay::Decision d = relay::evaluate(c, p2.data(), p2.size(), 1, 1000);
    checkVerdict(d.verdict, relay::VERDICT_RELAY, "ttl 2 relays");
    check(d.newTtl == 1, "ttl 2 becomes 1");

    // Density clamp: >= 6 links caps TTL at 5.
    Packet dense(4, 7, 444ULL);
    relay::Decision dd = relay::evaluate(c, dense.data(), dense.size(), 6, 1000);
    checkVerdict(dd.verdict, relay::VERDICT_RELAY, "dense mesh still relays");
    check(dd.newTtl == relay::DENSE_TTL_CAP, "6 links clamps ttl to 5");

    Packet thin(4, 7, 555ULL);
    check(relay::evaluate(c, thin.data(), thin.size(), 5, 1000).newTtl == 6,
          "5 links does not clamp");

    // Clamp must never raise a TTL.
    Packet low(4, 3, 666ULL);
    check(relay::evaluate(c, low.data(), low.size(), 9, 1000).newTtl == 2,
          "clamp never increases ttl");
  }

  // -------------------------------------------------------------------------
  group("dedup");
  {
    Cache c;
    Packet p(4, 7, 777ULL);
    checkVerdict(relay::evaluate(c, p.data(), p.size(), 1, 1000).verdict,
                 relay::VERDICT_RELAY, "first sighting relays");
    checkVerdict(relay::evaluate(c, p.data(), p.size(), 1, 1000).verdict,
                 relay::VERDICT_DUPLICATE, "second sighting is a duplicate");

    // The same packet one hop later: TTL differs, identity must not.
    Packet hopped(4, 6, 777ULL);
    checkVerdict(relay::evaluate(c, hopped.data(), hopped.size(), 1, 1000).verdict,
                 relay::VERDICT_DUPLICATE, "same packet at lower ttl is a duplicate");
    check(relay::dedupKey(p.data(), p.size()) == relay::dedupKey(hopped.data(), hopped.size()),
          "dedup key ignores ttl");

    // Distinct packets must not collide.
    Packet other(4, 7, 888ULL);
    checkVerdict(relay::evaluate(c, other.data(), other.size(), 1, 1000).verdict,
                 relay::VERDICT_RELAY, "different timestamp is a different packet");
    Packet otherType(5, 7, 777ULL);
    checkVerdict(relay::evaluate(c, otherType.data(), otherType.size(), 1, 1000).verdict,
                 relay::VERDICT_RELAY, "different type is a different packet");
    Packet otherSender(4, 7, 777ULL, 10, 0xBB);
    checkVerdict(relay::evaluate(c, otherSender.data(), otherSender.size(), 1, 1000).verdict,
                 relay::VERDICT_RELAY, "different sender is a different packet");
    Packet otherPayload(4, 7, 777ULL, 10, 0xAA, 0x99);
    checkVerdict(relay::evaluate(c, otherPayload.data(), otherPayload.size(), 1, 1000).verdict,
                 relay::VERDICT_RELAY, "different payload is a different packet");
  }

  group("dedup window and rollover");
  {
    Cache c;
    Packet p(4, 7, 999ULL);
    check(relay::evaluate(c, p.data(), p.size(), 1, 1000).verdict == relay::VERDICT_RELAY,
          "seen at t=1s");
    check(relay::evaluate(c, p.data(), p.size(), 1, 1000 + relay::DEDUP_WINDOW_MS - 1).verdict
              == relay::VERDICT_DUPLICATE,
          "still suppressed just inside the 5 min window");
    check(relay::evaluate(c, p.data(), p.size(), 1, 1000 + relay::DEDUP_WINDOW_MS).verdict
              == relay::VERDICT_RELAY,
          "relays again once the window expires");

    // Window is measured from first sighting, so a peer spamming the same
    // frame cannot keep it suppressed indefinitely.
    Cache c2;
    Packet q(4, 7, 1234ULL);
    relay::evaluate(c2, q.data(), q.size(), 1, 0);
    for (uint32_t t = 1; t < relay::DEDUP_WINDOW_MS; t += 30000)
      relay::evaluate(c2, q.data(), q.size(), 1, t);
    check(relay::evaluate(c2, q.data(), q.size(), 1, relay::DEDUP_WINDOW_MS).verdict
              == relay::VERDICT_RELAY,
          "repeated hits do not extend suppression forever");

    // millis() wraps every ~49.7 days; unsigned subtraction must stay correct.
    Cache c3;
    Packet r(4, 7, 4321ULL);
    const uint32_t nearWrap = 0xFFFFFF00u;
    check(relay::evaluate(c3, r.data(), r.size(), 1, nearWrap).verdict == relay::VERDICT_RELAY,
          "seen just before millis() rollover");
    check(relay::evaluate(c3, r.data(), r.size(), 1, nearWrap + 500).verdict
              == relay::VERDICT_DUPLICATE,
          "still suppressed after the counter wraps past zero");
  }

  group("dedup cache is bounded");
  {
    relay::DedupCache<256> c;
    for (uint64_t i = 0; i < 256; i++) {
      Packet p(4, 7, 10000ULL + i);
      relay::evaluate(c, p.data(), p.size(), 1, 1000);
    }
    check(c.occupancy() == 256, "cache fills to capacity");

    Packet first(4, 7, 10000ULL);
    check(relay::evaluate(c, first.data(), first.size(), 1, 1000).verdict
              == relay::VERDICT_DUPLICATE,
          "oldest entry still present at exactly capacity");

    // One more eviction cycle and the oldest entry is gone — the documented
    // tradeoff of a 256-entry ring versus the client's 1000.
    for (uint64_t i = 256; i < 300; i++) {
      Packet p(4, 7, 10000ULL + i);
      relay::evaluate(c, p.data(), p.size(), 1, 1000);
    }
    check(c.occupancy() == 256, "cache never exceeds capacity");
    check(relay::evaluate(c, first.data(), first.size(), 1, 1000).verdict
              == relay::VERDICT_RELAY,
          "oldest entry evicted after wrap (known ring-buffer tradeoff)");
  }

  // -------------------------------------------------------------------------
  group("malformed input");
  {
    Cache c;
    uint8_t tiny[13] = {1, 4, 7};
    checkVerdict(relay::evaluate(c, tiny, sizeof(tiny), 1, 1000).verdict,
                 relay::VERDICT_TOO_SHORT, "13 byte frame rejected");

    uint8_t header_only[14] = {1, 4, 7};
    checkVerdict(relay::evaluate(c, header_only, sizeof(header_only), 1, 1000).verdict,
                 relay::VERDICT_TOO_SHORT, "header with no sender ID rejected");

    uint8_t just_under[21] = {1, 4, 7};
    checkVerdict(relay::evaluate(c, just_under, sizeof(just_under), 1, 1000).verdict,
                 relay::VERDICT_TOO_SHORT, "21 byte frame rejected");

    Packet bare(4, 7, 1ULL, 0);
    check(bare.size() == 22, "22 byte frame is the minimum");
    checkVerdict(relay::evaluate(c, bare.data(), bare.size(), 1, 1000).verdict,
                 relay::VERDICT_RELAY, "22 byte frame with empty payload relays");

    checkVerdict(relay::evaluate(c, bare.data(), 0, 1, 1000).verdict,
                 relay::VERDICT_TOO_SHORT, "zero length write rejected");

    std::vector<uint8_t> huge(relay::MAX_PACKET_SIZE + 1, 0);
    huge[relay::OFF_VERSION] = 1;
    huge[relay::OFF_TTL]     = 7;
    checkVerdict(relay::evaluate(c, huge.data(), huge.size(), 1, 1000).verdict,
                 relay::VERDICT_OVERSIZED, "frame above 512 bytes rejected");

    // Declared payload longer than the frame can hold.
    Packet lying(4, 7, 2ULL, 10);
    lying.b[relay::OFF_PAYLOAD_LEN]     = 0x01;
    lying.b[relay::OFF_PAYLOAD_LEN + 1] = 0x00;   // claims 256 bytes in a 32 byte frame
    checkVerdict(relay::evaluate(c, lying.data(), lying.size(), 1, 1000).verdict,
                 relay::VERDICT_SHORT_PAYLOAD, "payloadLen longer than frame rejected");

    // A frame carrying recipient ID + signature is longer than payloadLen
    // alone implies, and must still be accepted.
    Packet signed_pkt(4, 7, 3ULL, 10);
    signed_pkt.b.resize(signed_pkt.b.size() + 8 + 64, 0x5A);
    signed_pkt.b[relay::OFF_FLAGS] = 0x03;
    checkVerdict(relay::evaluate(c, signed_pkt.data(), signed_pkt.size(), 1, 1000).verdict,
                 relay::VERDICT_RELAY, "signed packet with recipient ID relays");
    check(!relay::trailerLooksWrong(signed_pkt.data(), signed_pkt.size()),
          "recipient ID + signature is within the trailer bound");

    Packet fat(4, 7, 4ULL, 10);
    fat.b.resize(fat.b.size() + 200, 0);
    check(relay::trailerLooksWrong(fat.data(), fat.size()),
          "unexplained trailing bytes are flagged");
  }

  // -------------------------------------------------------------------------
  group("regression: version check (AUDIT S4)");
  {
    // A v2 frame has an unknown layout — byte 2 need not be TTL. The old code
    // decremented it anyway and rebroadcast the result.
    Packet v2(4, 7, 5ULL, 10);
    v2.b[relay::OFF_VERSION] = 2;

    Cache c;
    checkVerdict(relay::evaluate(c, v2.data(), v2.size(), 1, 1000).verdict,
                 relay::VERDICT_BAD_VERSION, "v2 frame is dropped, not rewritten");

    legacy::reset();
    uint8_t legacyTtl = 0;
    bool    legacyRelayed = legacy::wouldRelay(v2.data(), v2.size(), 1, 1000, &legacyTtl);
    check(legacyRelayed, "legacy code forwarded the v2 frame (the bug)");
    check(legacyTtl == 6, "legacy code rewrote byte 2 of a v2 frame (the bug)");

    Packet v0(4, 7, 6ULL, 10);
    v0.b[relay::OFF_VERSION] = 0;
    Cache c2;
    checkVerdict(relay::evaluate(c2, v0.data(), v0.size(), 1, 1000).verdict,
                 relay::VERDICT_BAD_VERSION, "version 0 is dropped");
  }

  group("regression: short-frame dedup bypass (AUDIT S7)");
  {
    // A 14..21 byte frame keyed to 0 in the old code, and key 0 meant "let it
    // through without caching" — so it was never deduplicated and two relays
    // in range would bounce it back and forth.
    uint8_t runt[16] = {0};
    runt[relay::OFF_VERSION] = 1;
    runt[relay::OFF_TYPE]    = 4;
    runt[relay::OFF_TTL]     = 7;

    legacy::reset();
    uint8_t ttlOut = 0;
    int legacyForwards = 0;
    for (int i = 0; i < 50; i++)
      if (legacy::wouldRelay(runt, sizeof(runt), 1, 1000, &ttlOut)) legacyForwards++;
    check(legacyForwards == 50,
          "legacy code relayed the same unkeyable runt 50/50 times (the bug)");

    Cache c;
    int forwards = 0;
    for (int i = 0; i < 50; i++)
      if (relay::evaluate(c, runt, sizeof(runt), 1, 1000).verdict == relay::VERDICT_RELAY)
        forwards++;
    check(forwards == 0, "runt frames are now rejected outright");

    // And a well-formed frame is still deduplicated exactly once.
    Packet good(4, 7, 7ULL);
    Cache c2;
    int goodForwards = 0;
    for (int i = 0; i < 50; i++)
      if (relay::evaluate(c2, good.data(), good.size(), 1, 1000).verdict == relay::VERDICT_RELAY)
        goodForwards++;
    check(goodForwards == 1, "a valid packet is relayed exactly once per window");
  }

  group("regression: dead packets are still recorded (AUDIT ordering)");
  {
    // dedup runs before the TTL check, so a packet that arrives already dead
    // is recorded and a later, livelier copy of it is not relayed.
    Cache c;
    Packet dead(4, 1, 8ULL);
    checkVerdict(relay::evaluate(c, dead.data(), dead.size(), 1, 1000).verdict,
                 relay::VERDICT_TTL_EXHAUSTED, "ttl 1 arrives dead");
    Packet revived(4, 7, 8ULL);
    checkVerdict(relay::evaluate(c, revived.data(), revived.size(), 1, 1000).verdict,
                 relay::VERDICT_DUPLICATE, "same packet with a higher ttl is suppressed");
  }

  // -------------------------------------------------------------------------
  group("loop containment (two relays, one packet)");
  {
    // Model the case the dedup cache exists for: relay A and relay B are
    // linked to each other and to a phone. A packet must not circulate.
    Cache a, b;
    Packet p(4, 7, 4242ULL);
    std::vector<uint8_t> frame(p.b);

    int hops = 0;
    bool atA = true;
    for (int i = 0; i < 100; i++) {
      Cache& here = atA ? a : b;
      relay::Decision d = relay::evaluate(here, frame.data(), frame.size(), 2, 1000);
      if (d.verdict != relay::VERDICT_RELAY) break;
      frame[relay::OFF_TTL] = d.newTtl;
      hops++;
      atA = !atA;
    }
    check(hops == 2, "packet crosses each relay once, then stops");
    check(frame[relay::OFF_TTL] == 5, "ttl decremented once per relay");
  }

  // -------------------------------------------------------------------------
  printf("\n%s %d/%d checks passed\n",
         g_failed ? "\033[31mFAILED\033[0m" : "\033[32mOK\033[0m",
         g_checks - g_failed, g_checks);
  return g_failed ? 1 : 0;
}
