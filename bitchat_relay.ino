/* ============================================================================
 * bitchat BLE mesh relay  —  ESP32 + NimBLE-Arduino 2.x
 * ============================================================================
 *
 * A mains-powered, always-on relay node for the bitchat BLE mesh
 * (github.com/permissionlesstech/bitchat).
 *
 * It runs BOTH BLE roles at once:
 *   - PERIPHERAL: advertises bitchat's service UUID, so real phones running
 *     bitchat discover it and connect to it like any other peer.
 *   - CENTRAL: scans for other bitchat peers and connects out to them.
 *
 * Any packet arriving on one link gets its TTL decremented and is re-sent on
 * every OTHER link. That's the whole job.
 *
 * WHAT IT DELIBERATELY DOES NOT DO
 *   - Decrypt anything. Private messages stay opaque Noise ciphertext.
 *   - Verify signatures. That's the destination's job.
 *   - Reassemble fragments. Each fragment is relayed as its own packet.
 * A relay is a dumb pipe. It only reads enough of the outer header to route.
 *
 * WRITTEN FOR: NimBLE-Arduino 2.x. Every call in this sketch was checked
 *   against the 2.3.6 headers. 1.x will NOT compile — the callback signatures
 *   changed. See the README notes.
 *
 * The packet decision itself (header parsing, dedup, TTL policy) lives in
 * relay_core.h so that it can be run and tested on a host machine — see
 * test/. This file is the BLE plumbing around it.
 *
 * THREADING. This matters here and is easy to get wrong. NimBLE runs its own
 * FreeRTOS host task, and every callback below fires on that task, not on the
 * Arduino loop() task. Two consequences:
 *   - The link table is shared between the two tasks, so it is mutex guarded.
 *     Without that, a push_back from loop() can reallocate the vector while
 *     the host task is iterating it to forward a packet.
 *   - loop() must never hold that mutex across a blocking BLE call, or it
 *     stalls the entire host task. connectToPeer() is written to that rule.
 * ========================================================================== */

#include <NimBLEDevice.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>
#include <vector>

#include "relay_core.h"

// ---------------------------------------------------------------------------
// CONFIG — the few things you might want to change
// ---------------------------------------------------------------------------

// bitchat PRODUCTION mesh. There is a separate testnet UUID ending ...4B5A.
// Mixing them means you will never see real peers.
static const char* BITCHAT_SERVICE_UUID = "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C";
static const char* BITCHAT_CHAR_UUID    = "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D";

static const char*    DEVICE_NAME  = "bitchat-relay";
static const int8_t   TX_POWER_DBM = 9;    // 9 dBm = max on most ESP32s. 2.x takes dBm.
static const uint16_t DESIRED_MTU  = 517;  // 517 ATT MTU => 512 byte payload

// Set to 1 to dump the first 16 bytes of every received packet over serial.
// Turn this ON for first bring-up, then OFF for deployment. It is not just
// noisy: Serial.printf blocks the BLE host task, and at 115200 baud a hex dump
// per packet is enough to start dropping connections under load.
#define STRICT_HEADER_LOGGING 1

// Log every packet we decline to forward, and why. Cheap, and it is the
// difference between "the mesh is quiet" and "we are dropping everything".
#define LOG_DROPS 1

// Reject frames whose declared payloadLen cannot fit in the frame received.
// Leave on. Set to 0 only if a future protocol change makes the check
// over-strict and you would rather relay than drop while you investigate.
#define STRICT_LENGTH_CHECK 1

// Dedup ring size. 256 entries * 16 bytes is cheap; the real client keeps
// 1000. Raise if you deploy somewhere busy and see re-forwarded duplicates.
static const size_t DEDUP_CACHE_SIZE = 256;

// Scanning duty cycle. The radio is shared with every active connection, so
// scanning near-continuously is what makes a relay drop the links it already
// has. 40 % is a deliberate compromise: still finds peers within a second or
// two, still leaves the radio time to service nine connections.
static const uint16_t SCAN_INTERVAL_MS = 160;
static const uint16_t SCAN_WINDOW_MS   = 64;

// How long to leave a peer alone after a failed connection attempt, so that a
// device advertising the service UUID without the service behind it cannot pin
// the relay in a connect/fail loop.
static const uint32_t FAILURE_BACKOFF_MS = 60UL * 1000UL;
static const size_t   MAX_TRACKED_FAILURES = 16;

static const uint32_t HEARTBEAT_MS = 15000;

// Smallest useful ATT MTU. BLE guarantees 23; 3 of those bytes are the
// notification/write header, leaving 20 for us.
static const uint16_t MIN_ATT_MTU = 23;
static const uint16_t ATT_OVERHEAD = 3;

// ---------------------------------------------------------------------------
// SHARED STATE
//
// Everything in this block is touched from both the NimBLE host task and the
// loop() task, and is only safe to read or write while holding stateMux.
// ---------------------------------------------------------------------------
struct Link {
  uint16_t handle;
  bool     isClient;    // true  = we're central, we writeValue()
                        // false = we're peripheral, we notify()
  bool     subscribed;  // peripheral role only: peer has enabled notifications
  NimBLERemoteCharacteristic* remoteChr;   // only meaningful when isClient
};

struct PeerFailure {
  std::string addr;
  uint32_t    at;
};

static std::vector<Link>        links;
static std::vector<std::string> claimedAddrs;      // outbound: connected or connecting
static std::vector<PeerFailure> recentFailures;

static SemaphoreHandle_t stateMux = nullptr;

static NimBLEServer*         pServer    = nullptr;
static NimBLECharacteristic* pRelayChar = nullptr;

// RAII lock with a timeout. A BLE callback must never wait forever on the
// loop() task, so a failed acquisition drops the packet instead of deadlocking.
class StateLock {
 public:
  explicit StateLock(uint32_t waitMs = 200) : held_(false) {
    if (stateMux && xSemaphoreTakeRecursive(stateMux, pdMS_TO_TICKS(waitMs)) == pdTRUE)
      held_ = true;
  }
  ~StateLock() { if (held_) xSemaphoreGiveRecursive(stateMux); }
  bool held() const { return held_; }

  StateLock(const StateLock&) = delete;
  StateLock& operator=(const StateLock&) = delete;

 private:
  bool held_;
};

// ---------------------------------------------------------------------------
// LINK TABLE — callers must hold stateMux
// ---------------------------------------------------------------------------
static void addLinkLocked(uint16_t handle, bool isClient, NimBLERemoteCharacteristic* chr) {
  for (auto& l : links) if (l.handle == handle) return;   // already tracked
  Link l;
  l.handle     = handle;
  l.isClient   = isClient;
  l.subscribed = false;
  l.remoteChr  = chr;
  links.push_back(l);
  Serial.printf("[link] + handle=%u role=%s  (total %u)\n",
                handle, isClient ? "central" : "peripheral", (unsigned)links.size());
}

static void removeLinkLocked(uint16_t handle) {
  for (size_t i = 0; i < links.size(); i++) {
    if (links[i].handle == handle) {
      links.erase(links.begin() + (long)i);
      Serial.printf("[link] - handle=%u  (total %u)\n", handle, (unsigned)links.size());
      return;
    }
  }
}

static void addLink(uint16_t handle, bool isClient, NimBLERemoteCharacteristic* chr) {
  StateLock lock;
  if (!lock.held()) { Serial.println("[link] ! lock timeout adding link"); return; }
  addLinkLocked(handle, isClient, chr);
}

static void removeLink(uint16_t handle) {
  StateLock lock;
  if (!lock.held()) { Serial.println("[link] ! lock timeout removing link"); return; }
  removeLinkLocked(handle);
}

static size_t linkCount() {
  StateLock lock;
  if (!lock.held()) return 0;
  return links.size();
}

// ---------------------------------------------------------------------------
// OUTBOUND ADDRESS BOOKKEEPING — callers must hold stateMux
// ---------------------------------------------------------------------------
static bool addrClaimedLocked(const std::string& a) {
  for (auto& known : claimedAddrs) if (known == a) return true;
  return false;
}

// Reserve an address before we start connecting to it. Without this the scan
// callback can queue the same peer twice while the first connect is still in
// flight, and we end up with two links to one device.
static bool claimAddr(const std::string& a) {
  StateLock lock;
  if (!lock.held() || addrClaimedLocked(a)) return false;
  claimedAddrs.push_back(a);
  return true;
}

static void releaseAddrLocked(const std::string& a) {
  for (size_t i = 0; i < claimedAddrs.size(); i++) {
    if (claimedAddrs[i] == a) { claimedAddrs.erase(claimedAddrs.begin() + (long)i); return; }
  }
}

static void releaseAddr(const std::string& a) {
  StateLock lock;
  if (!lock.held()) return;
  releaseAddrLocked(a);
}

static bool inBackoffLocked(const std::string& a, uint32_t now) {
  for (auto& f : recentFailures)
    if (f.addr == a && (uint32_t)(now - f.at) < FAILURE_BACKOFF_MS) return true;
  return false;
}

static void noteFailure(const std::string& a, uint32_t now) {
  StateLock lock;
  if (!lock.held()) return;
  for (auto& f : recentFailures) {
    if (f.addr == a) { f.at = now; return; }
  }
  if (recentFailures.size() >= MAX_TRACKED_FAILURES)
    recentFailures.erase(recentFailures.begin());
  PeerFailure f;
  f.addr = a;
  f.at   = now;
  recentFailures.push_back(f);
}

// ---------------------------------------------------------------------------
// FORWARDING
// ---------------------------------------------------------------------------

// How many payload bytes this link can actually carry right now.
//
// A link that has not finished MTU exchange is stuck at the 23-byte BLE
// default, which leaves 20 usable bytes — less than the 22-byte minimum
// bitchat frame. Sending anyway does not fail loudly, it truncates, and the
// far end gets a packet whose declared length no longer matches its body. So
// we skip the link and say so.
static size_t egressCapacity(const Link& l) {
  uint16_t mtu = 0;
  if (l.isClient) {
    NimBLEClient* c = l.remoteChr ? l.remoteChr->getClient() : nullptr;
    if (c) mtu = c->getMTU();
  } else if (pServer) {
    mtu = pServer->getPeerMTU(l.handle);
  }
  if (mtu < MIN_ATT_MTU) mtu = MIN_ATT_MTU;
  return (size_t)(mtu - ATT_OVERHEAD);
}

// Send one packet out on every link except the one it arrived on.
//
// The lock is held across the sends. That is intentional: the only other
// contender is loop(), which holds it for microseconds, and holding it here
// means no link can be torn down midway through the iteration.
static void forwardToAllExcept(uint16_t ingressHandle, const uint8_t* buf, size_t len) {
  StateLock lock;
  if (!lock.held()) {
    Serial.println("[relay] ! lock timeout, packet dropped");
    return;
  }

  int sent = 0, tooBig = 0, unsubscribed = 0, failed = 0;

  for (auto& l : links) {
    if (l.handle == ingressHandle) continue;          // never echo back the way it came

    if (len > egressCapacity(l)) { tooBig++; continue; }

    bool ok = false;
    if (l.isClient) {
      // write-no-response on purpose: the with-response path blocks the
      // calling task on a semaphore, and we are on the BLE host task here.
      if (l.remoteChr) ok = l.remoteChr->writeValue(buf, len, false);
    } else {
      // notify() with an explicit handle does not check subscription state, so
      // we check it ourselves — otherwise every unsubscribed peer counts as a
      // successful send and the bring-up log lies to you.
      if (!l.subscribed) { unsubscribed++; continue; }
      ok = pRelayChar && pRelayChar->notify(buf, len, l.handle);
    }
    if (ok) sent++; else failed++;
  }

  Serial.printf("[relay] forwarded %u byte packet to %d link(s)", (unsigned)len, sent);
  if (failed)       Serial.printf("  failed=%d", failed);
  if (tooBig)       Serial.printf("  skipped-mtu=%d", tooBig);
  if (unsubscribed) Serial.printf("  skipped-unsubscribed=%d", unsubscribed);
  Serial.println();
}

// ---------------------------------------------------------------------------
// THE RELAY DECISION — every received packet, from either role, lands here
//
// Runs on the NimBLE host task only (server write callback or client notify
// callback), which is why the dedup cache and the scratch buffer below need no
// lock of their own: there is exactly one task in here at a time.
// ---------------------------------------------------------------------------
static relay::DedupCache<DEDUP_CACHE_SIZE> dedupCache;
static uint8_t txScratch[relay::MAX_PACKET_SIZE];

static void handleIncomingPacket(uint16_t ingressHandle, const uint8_t* buf, size_t len) {
#if STRICT_HEADER_LOGGING
  Serial.printf("[rx  ] handle=%u len=%u hdr=", ingressHandle, (unsigned)len);
  for (size_t i = 0; i < len && i < 16; i++) Serial.printf("%02X ", buf[i]);
  if (len >= relay::V1_HEADER_SIZE)
    Serial.printf(" | ver=%u type=%u ttl=%u",
                  buf[relay::OFF_VERSION], buf[relay::OFF_TYPE], buf[relay::OFF_TTL]);
  Serial.println();
#endif

  const relay::Decision d = relay::evaluate(dedupCache, buf, len, linkCount(),
                                            millis(), STRICT_LENGTH_CHECK != 0);

  if (d.verdict != relay::VERDICT_RELAY) {
#if LOG_DROPS
    // Duplicates are the common, healthy case in a mesh — logging every one
    // would drown out the drops that actually indicate a problem.
    if (d.verdict != relay::VERDICT_DUPLICATE)
      Serial.printf("[drop ] handle=%u len=%u reason=%s\n",
                    ingressHandle, (unsigned)len, relay::verdictName(d.verdict));
#endif
    return;
  }

  if (relay::trailerLooksWrong(buf, len))
    Serial.printf("[warn ] len=%u but payloadLen=%u — check offsets against the app\n",
                  (unsigned)len, relay::declaredPayloadLen(buf));

  // Mutate ONLY the TTL byte. This is safe even for signed packets: bitchat
  // computes signatures EXCLUDING the TTL byte precisely so that relays can
  // decrement it in place without invalidating anything.
  memcpy(txScratch, buf, len);
  txScratch[relay::OFF_TTL] = d.newTtl;

  forwardToAllExcept(ingressHandle, txScratch, len);
}

// ---------------------------------------------------------------------------
// PERIPHERAL ROLE — phones connect to us and write packets into our char
// ---------------------------------------------------------------------------
class RelayCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    const NimBLEAttValue v = pChar->getValue();
    handleIncomingPacket(connInfo.getConnHandle(), v.data(), v.length());
  }

  // Track who actually wants notifications, so forwarding can tell a real
  // delivery from a write into the void.
  void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    const bool wantsNotify = (subValue & 0x0001) != 0;
    StateLock lock;
    if (!lock.held()) return;
    for (auto& l : links) {
      if (l.handle == connInfo.getConnHandle() && !l.isClient) {
        l.subscribed = wantsNotify;
        Serial.printf("[srv ] handle=%u notifications %s\n",
                      l.handle, wantsNotify ? "on" : "off");
        return;
      }
    }
  }
};

class RelayServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
    Serial.printf("[srv ] peer connected to us, handle=%u\n", connInfo.getConnHandle());
    addLink(connInfo.getConnHandle(), false, nullptr);
    // Required. NimBLE does not restart advertising after a successful
    // connection, and a relay wants many inbound links, not one.
    NimBLEDevice::getAdvertising()->start();
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("[srv ] peer disconnected, handle=%u reason=%d\n",
                  connInfo.getConnHandle(), reason);
    removeLink(connInfo.getConnHandle());
    // No advertising restart here: advertiseOnDisconnect(true) means the
    // library does it for us as soon as this callback returns. Doing it twice
    // just logs an error on every disconnect.
  }

  void onMTUChange(uint16_t MTU, NimBLEConnInfo& connInfo) override {
    Serial.printf("[srv ] MTU now %u on handle %u\n", MTU, connInfo.getConnHandle());
  }
};

// ---------------------------------------------------------------------------
// CENTRAL ROLE — we scan for peers, connect out, subscribe to their notifies
// ---------------------------------------------------------------------------

// Notification arriving from a peer we connected TO.
static void clientNotifyCB(NimBLERemoteCharacteristic* chr,
                           uint8_t* data, size_t len, bool /*isNotify*/) {
  uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
  NimBLEClient* c = chr ? chr->getClient() : nullptr;
  if (c) handle = c->getConnHandle();
  handleIncomingPacket(handle, data, len);
}

class RelayClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    Serial.printf("[cli ] connected out to %s\n", c->getPeerAddress().toString().c_str());
  }

  void onDisconnect(NimBLEClient* c, int reason) override {
    const std::string a = c->getPeerAddress().toString();
    Serial.printf("[cli ] lost %s reason=%d\n", a.c_str(), reason);
    // getConnHandle() is still valid here — NimBLE clears it after this
    // callback returns, not before.
    removeLink(c->getConnHandle());
    releaseAddr(a);
    // The client object itself is freed by the stack: see setSelfDelete()
    // in connectToPeer(). Do not delete it here.
  }
};
static RelayClientCallbacks clientCallbacks;

// One-at-a-time connect queue. Doing the connect inside the scan callback is
// unsafe, so we just flag it and let loop() do the work.
static volatile bool doConnect = false;
static NimBLEAddress targetAddr;

class RelayScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    static const NimBLEUUID kBitchatService(BITCHAT_SERVICE_UUID);
    if (!dev->isAdvertisingService(kBitchatService)) return;
    if (doConnect) return;                                  // already have one queued

    const std::string a   = dev->getAddress().toString();
    const uint32_t    now = millis();

    {
      StateLock lock;
      if (!lock.held()) return;
      if (addrClaimedLocked(a)) return;         // already linked or connecting
      if (inBackoffLocked(a, now)) return;      // recently failed, leave it alone
    }

    Serial.printf("[scan] found bitchat peer %s rssi=%d\n", a.c_str(), dev->getRSSI());
    targetAddr = dev->getAddress();
    doConnect  = true;                          // set last: loop() reads it as the gate
  }
};
static RelayScanCallbacks scanCallbacks;

static void connectToPeer(const NimBLEAddress& addr) {
  const std::string a = addr.toString();

  // Claim the address first. If the scanner sees this peer again while the
  // connect below is still in flight, it will now skip it.
  if (!claimAddr(a)) return;

  NimBLEClient* c = NimBLEDevice::createClient();
  if (!c) {
    // Almost always the connection limit, and almost always because the
    // nimconfig.h step was skipped. It can also mean clients are being
    // leaked — this sketch relies on setSelfDelete() below to avoid that.
    Serial.println("[cli ] no free client slots — raise max connections");
    releaseAddr(a);
    noteFailure(a, millis());
    return;
  }

  c->setClientCallbacks(&clientCallbacks, false);
  // Hand the client's lifetime to the stack. Without this every disconnect
  // permanently consumes one of the MAX_CONNECTIONS client slots, and the
  // relay stops being able to connect outward after a handful of peers come
  // and go. Note this also means a failed connect() self-deletes, so we must
  // not call deleteClient() ourselves anywhere below.
  c->setSelfDelete(true, true);

  if (!c->connect(addr, true, false, true)) {
    Serial.printf("[cli ] connect to %s failed\n", a.c_str());
    releaseAddr(a);
    noteFailure(a, millis());
    return;                                  // c is already gone
  }

  NimBLERemoteService* svc = c->getService(BITCHAT_SERVICE_UUID);
  if (!svc) {
    Serial.printf("[cli ] %s has no bitchat service\n", a.c_str());
    releaseAddr(a);
    noteFailure(a, millis());
    c->disconnect();                         // self-delete cleans up
    return;
  }

  NimBLERemoteCharacteristic* chr = svc->getCharacteristic(BITCHAT_CHAR_UUID);
  if (!chr) {
    Serial.printf("[cli ] %s has no bitchat characteristic\n", a.c_str());
    releaseAddr(a);
    noteFailure(a, millis());
    c->disconnect();
    return;
  }

  // Without a subscription this link is write-only: we could push packets to
  // the peer but never hear anything back, which is half a relay. Worth
  // keeping — outbound still helps — but worth saying out loud.
  if (chr->canNotify()) {
    if (!chr->subscribe(true, clientNotifyCB))
      Serial.printf("[cli ] %s subscribe failed — link is send-only\n", a.c_str());
  } else {
    Serial.printf("[cli ] %s characteristic cannot notify — link is send-only\n", a.c_str());
  }

  addLink(c->getConnHandle(), true, chr);
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== bitchat relay booting ===");

  stateMux = xSemaphoreCreateRecursiveMutex();
  if (!stateMux) {
    Serial.println("[boot] FATAL: could not create state mutex");
    while (true) delay(1000);
  }

  NimBLEDevice::init(DEVICE_NAME);
  if (!NimBLEDevice::setPower(TX_POWER_DBM))   // 2.x: plain dBm, NOT ESP_PWR_LVL_*
    Serial.printf("[boot] warning: could not set TX power to %d dBm\n", TX_POWER_DBM);
  NimBLEDevice::setMTU(DESIRED_MTU);

  // ---- peripheral side ----
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new RelayServerCallbacks());
  pServer->advertiseOnDisconnect(true);     // 2.x does NOT do this automatically

  NimBLEService* svc = pServer->createService(BITCHAT_SERVICE_UUID);
  pRelayChar = svc->createCharacteristic(
      BITCHAT_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY,
      relay::MAX_PACKET_SIZE);
  pRelayChar->setCallbacks(new RelayCharCallbacks());
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();

  // ORDER MATTERS. A BLE advertisement holds 31 bytes. The 128-bit service
  // UUID needs 18 of them and the device name needs 15, so both cannot fit.
  // enableScanResponse() must come first, because setName() only diverts the
  // name into the scan response when scan response is already enabled. Get
  // this backwards and the name takes the advertisement slot, the service UUID
  // silently spills into the scan response instead, and phones filtering on
  // the service UUID — especially iOS scanning in the background — may never
  // discover the relay at all.
  adv->enableScanResponse(true);            // 2.x: off by default
  adv->setName(DEVICE_NAME);                // -> scan response
  if (!adv->addServiceUUID(BITCHAT_SERVICE_UUID))   // -> advertisement proper
    Serial.println("[boot] FATAL: service UUID did not fit in the advertisement");
  if (!adv->start())
    Serial.println("[boot] FATAL: advertising failed to start");
  else
    Serial.println("[srv ] advertising as bitchat peer");

  // ---- central side ----
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);                // needed: name lives in scan response
  scan->setInterval(SCAN_INTERVAL_MS);      // 2.x: milliseconds
  scan->setWindow(SCAN_WINDOW_MS);
  scan->start(0, false);                    // 0 = scan forever
  Serial.println("[scan] scanning for bitchat peers");
}

static uint32_t lastBeat = 0;

void loop() {
  if (doConnect) {
    // Copy the target before clearing the gate. The scan callback runs on
    // another task and will overwrite targetAddr as soon as doConnect is
    // false, so reading it after that point can connect us to a peer we never
    // decided on.
    const NimBLEAddress target = targetAddr;
    doConnect = false;

    NimBLEDevice::getScan()->stop();
    connectToPeer(target);                  // blocking, and deliberately unlocked
    NimBLEDevice::getScan()->start(0, false);
  }

  const uint32_t now = millis();
  if ((uint32_t)(now - lastBeat) > HEARTBEAT_MS) {
    lastBeat = now;
    size_t nLinks = 0, nClaimed = 0;
    {
      StateLock lock;
      if (lock.held()) {
        nLinks   = links.size();
        nClaimed = claimedAddrs.size();
        // Expire stale backoff entries so a peer that has since recovered
        // gets another chance.
        for (size_t i = recentFailures.size(); i > 0; i--) {
          if ((uint32_t)(now - recentFailures[i - 1].at) >= FAILURE_BACKOFF_MS)
            recentFailures.erase(recentFailures.begin() + (long)(i - 1));
        }
      }
    }
    Serial.printf("[stat] links=%u  outbound=%u  heap=%u\n",
                  (unsigned)nLinks, (unsigned)nClaimed, (unsigned)ESP.getFreeHeap());
  }

  delay(20);
}
