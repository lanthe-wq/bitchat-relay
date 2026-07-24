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
 * WRITTEN FOR: NimBLE-Arduino 2.x (tested against 2.5.x API docs).
 *   1.x will NOT compile — the callback signatures changed. See README notes.
 *
 * PROTOCOL OFFSETS: taken from BinaryProtocol.swift in the bitchat repo
 * (v1HeaderSize = 14, senderIDSize = 8, all multi-byte fields big-endian).
 * These CAN change when the app updates. Verify with STRICT_HEADER_LOGGING
 * before you trust a deployed unit. See the guide's "Verify" section.
 * ========================================================================== */

#include <NimBLEDevice.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// CONFIG — the few things you might want to change
// ---------------------------------------------------------------------------

// bitchat PRODUCTION mesh. There is a separate testnet UUID ending ...4B5A.
// Mixing them means you will never see real peers.
static const char* BITCHAT_SERVICE_UUID = "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C";
static const char* BITCHAT_CHAR_UUID    = "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D";

static const char*  DEVICE_NAME   = "bitchat-relay";
static const int8_t TX_POWER_DBM  = 9;    // 9 dBm = max on most ESP32s. 2.x takes dBm.
static const uint16_t DESIRED_MTU = 517;  // 517 ATT MTU => 512 byte payload

// Set to 1 to dump the first 16 bytes of every received packet over serial.
// Turn this ON for first bring-up, then OFF for deployment (it's noisy).
#define STRICT_HEADER_LOGGING 1

// ---------------------------------------------------------------------------
// WIRE FORMAT — from BinaryProtocol.swift
// ---------------------------------------------------------------------------
static const size_t V1_HEADER_SIZE     = 14;  // version,type,ttl,timestamp(8),flags,payloadLen(2)
static const size_t SENDER_ID_SIZE     = 8;
static const size_t OFF_VERSION        = 0;
static const size_t OFF_TYPE           = 1;
static const size_t OFF_TTL            = 2;   // the ONLY byte we ever modify
static const size_t OFF_TIMESTAMP      = 3;   // 8 bytes, big-endian
static const size_t OFF_FLAGS          = 11;
static const size_t OFF_PAYLOAD_LEN    = 12;  // 2 bytes, big-endian
static const size_t OFF_SENDER_ID      = V1_HEADER_SIZE;

// Relay policy. The real client clamps TTL by link density: dense graphs
// (>= 6 links) cap broadcast TTL at 5, thin chains relay at full depth.
static const size_t  DENSE_LINK_THRESHOLD = 6;
static const uint8_t DENSE_TTL_CAP        = 5;

// ---------------------------------------------------------------------------
// LINKS — one entry per live BLE connection, whichever role opened it.
// NimBLE connection handles are unique across the whole stack, so we can use
// the handle alone as a global link ID regardless of role.
// ---------------------------------------------------------------------------
struct Link {
  uint16_t handle;
  bool     isClient;                       // true  = we're central, we writeValue()
                                           // false = we're peripheral, we notify()
  NimBLERemoteCharacteristic* remoteChr;   // only meaningful when isClient
};

static std::vector<Link>        links;
static std::vector<std::string> connectedAddrs;   // dedupe outbound connections

static NimBLEServer*         pServer    = nullptr;
static NimBLECharacteristic* pRelayChar = nullptr;

static void addLink(uint16_t handle, bool isClient, NimBLERemoteCharacteristic* chr) {
  for (auto& l : links) if (l.handle == handle) return;   // already tracked
  links.push_back({handle, isClient, chr});
  Serial.printf("[link] + handle=%u role=%s  (total %u)\n",
                handle, isClient ? "central" : "peripheral", (unsigned)links.size());
}

static void removeLink(uint16_t handle) {
  for (size_t i = 0; i < links.size(); i++) {
    if (links[i].handle == handle) {
      links.erase(links.begin() + i);
      Serial.printf("[link] - handle=%u  (total %u)\n", handle, (unsigned)links.size());
      return;
    }
  }
}

// Send one packet out on every link except the one it arrived on.
static void forwardToAllExcept(uint16_t ingressHandle, const uint8_t* buf, size_t len) {
  int sent = 0;
  for (auto& l : links) {
    if (l.handle == ingressHandle) continue;          // never echo back the way it came
    bool ok = false;
    if (l.isClient) {
      if (l.remoteChr) ok = l.remoteChr->writeValue(buf, len, false);  // false = write-no-response
    } else {
      ok = pRelayChar->notify(buf, len, l.handle);    // notify only this connection
    }
    if (ok) sent++;
  }
  Serial.printf("[relay] forwarded %d byte packet to %d link(s)\n", (int)len, sent);
}

// ---------------------------------------------------------------------------
// DEDUP CACHE
// Spec: LRU seen-set, 1000 entries, 5 min expiry, keyed by sender + timestamp
// + type + payload digest. We approximate with a ring buffer sized for ESP32
// RAM. Not bit-identical to the real bloom filter, but it kills relay loops.
// ---------------------------------------------------------------------------
static const size_t   DEDUP_CACHE_SIZE = 256;
static const uint32_t DEDUP_WINDOW_MS  = 5UL * 60UL * 1000UL;

struct SeenEntry { uint64_t key; uint32_t at; bool used; };
static SeenEntry seenCache[DEDUP_CACHE_SIZE] = {};
static size_t    seenNext = 0;

static uint32_t fnv1a(const uint8_t* d, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
  return h;
}

static uint64_t dedupKey(const uint8_t* buf, size_t len) {
  if (len < OFF_SENDER_ID + SENDER_ID_SIZE) return 0;
  uint64_t ts = 0;
  for (int i = 0; i < 8; i++) ts = (ts << 8) | buf[OFF_TIMESTAMP + i];   // big-endian
  uint32_t senderDigest  = fnv1a(buf + OFF_SENDER_ID, SENDER_ID_SIZE);
  size_t   restOff       = OFF_SENDER_ID + SENDER_ID_SIZE;
  uint32_t payloadDigest = (len > restOff) ? fnv1a(buf + restOff, len - restOff) : 0;
  return ((uint64_t)senderDigest << 32) ^ ts
       ^ ((uint64_t)buf[OFF_TYPE] << 56) ^ payloadDigest;
}

static bool alreadySeen(const uint8_t* buf, size_t len) {
  uint64_t k = dedupKey(buf, len);
  if (k == 0) return false;                 // can't key it — let it through
  uint32_t now = millis();
  for (size_t i = 0; i < DEDUP_CACHE_SIZE; i++) {
    if (seenCache[i].used && seenCache[i].key == k) {
      if (now - seenCache[i].at < DEDUP_WINDOW_MS) return true;
      seenCache[i].at = now;
      return false;
    }
  }
  seenCache[seenNext] = { k, now, true };
  seenNext = (seenNext + 1) % DEDUP_CACHE_SIZE;
  return false;
}

// ---------------------------------------------------------------------------
// THE RELAY DECISION — every received packet, from either role, lands here
// ---------------------------------------------------------------------------
static void handleIncomingPacket(uint16_t ingressHandle, const uint8_t* buf, size_t len) {
#if STRICT_HEADER_LOGGING
  Serial.printf("[rx  ] handle=%u len=%u hdr=", ingressHandle, (unsigned)len);
  for (size_t i = 0; i < len && i < 16; i++) Serial.printf("%02X ", buf[i]);
  if (len >= V1_HEADER_SIZE)
    Serial.printf(" | ver=%u type=%u ttl=%u", buf[OFF_VERSION], buf[OFF_TYPE], buf[OFF_TTL]);
  Serial.println();
#endif

  if (len < V1_HEADER_SIZE)     return;    // malformed, drop
  if (alreadySeen(buf, len))    return;    // loop protection

  uint8_t ttl = buf[OFF_TTL];
  if (ttl == 0) return;                    // end of life, nothing to forward

  uint8_t newTtl = ttl - 1;
  if (links.size() >= DENSE_LINK_THRESHOLD && newTtl > DENSE_TTL_CAP) newTtl = DENSE_TTL_CAP;
  if (newTtl == 0) return;                 // would die on arrival anyway

  // Mutate ONLY the TTL byte. This is safe even for signed packets: bitchat
  // computes signatures EXCLUDING the TTL byte precisely so that relays can
  // decrement it in place without invalidating anything.
  std::vector<uint8_t> out(buf, buf + len);
  out[OFF_TTL] = newTtl;

  forwardToAllExcept(ingressHandle, out.data(), out.size());
}

// ---------------------------------------------------------------------------
// PERIPHERAL ROLE — phones connect to us and write packets into our char
// ---------------------------------------------------------------------------
class RelayCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pChar, NimBLEConnInfo& connInfo) override {
    NimBLEAttValue v = pChar->getValue();
    handleIncomingPacket(connInfo.getConnHandle(), v.data(), v.length());
  }
};

class RelayServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    Serial.printf("[srv ] peer connected to us, handle=%u\n", connInfo.getConnHandle());
    addLink(connInfo.getConnHandle(), false, nullptr);
    // Keep advertising so more peers can still find us (we want many links).
    NimBLEDevice::getAdvertising()->start();
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    Serial.printf("[srv ] peer disconnected, handle=%u reason=%d\n",
                  connInfo.getConnHandle(), reason);
    removeLink(connInfo.getConnHandle());
    NimBLEDevice::getAdvertising()->start();
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
                           uint8_t* data, size_t len, bool isNotify) {
  uint16_t handle = 0;
  const NimBLERemoteService* svc = chr->getRemoteService();
  if (svc && svc->getClient()) handle = svc->getClient()->getConnHandle();
  handleIncomingPacket(handle, data, len);
}

class RelayClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* c) override {
    Serial.printf("[cli ] connected out to %s\n", c->getPeerAddress().toString().c_str());
  }
  void onDisconnect(NimBLEClient* c, int reason) override {
    Serial.printf("[cli ] lost %s reason=%d\n",
                  c->getPeerAddress().toString().c_str(), reason);
    removeLink(c->getConnHandle());
    std::string a = c->getPeerAddress().toString();
    for (size_t i = 0; i < connectedAddrs.size(); i++)
      if (connectedAddrs[i] == a) { connectedAddrs.erase(connectedAddrs.begin() + i); break; }
  }
};
static RelayClientCallbacks clientCallbacks;

// One-at-a-time connect queue. Doing the connect inside the scan callback is
// unsafe, so we just flag it and let loop() do the work.
static volatile bool  doConnect = false;
static NimBLEAddress  targetAddr;

class RelayScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (!dev->isAdvertisingService(NimBLEUUID(BITCHAT_SERVICE_UUID))) return;
    if (doConnect) return;                                  // already have one queued
    std::string a = dev->getAddress().toString();
    for (auto& known : connectedAddrs) if (known == a) return;   // already linked
    Serial.printf("[scan] found bitchat peer %s rssi=%d\n", a.c_str(), dev->getRSSI());
    targetAddr = dev->getAddress();
    doConnect  = true;
  }
};
static RelayScanCallbacks scanCallbacks;

static void connectToPeer(const NimBLEAddress& addr) {
  NimBLEClient* c = NimBLEDevice::createClient();
  if (!c) { Serial.println("[cli ] no free client slots — raise max connections"); return; }
  c->setClientCallbacks(&clientCallbacks, false);

  if (!c->connect(addr, true, false, true)) {
    Serial.println("[cli ] connect failed");
    NimBLEDevice::deleteClient(c);
    return;
  }
  NimBLERemoteService* svc = c->getService(BITCHAT_SERVICE_UUID);
  if (!svc) { Serial.println("[cli ] no bitchat service"); c->disconnect(); return; }

  NimBLERemoteCharacteristic* chr = svc->getCharacteristic(BITCHAT_CHAR_UUID);
  if (!chr) { Serial.println("[cli ] no bitchat characteristic"); c->disconnect(); return; }

  if (chr->canNotify() && !chr->subscribe(true, clientNotifyCB)) {
    Serial.println("[cli ] subscribe failed");
  }

  addLink(c->getConnHandle(), true, chr);
  connectedAddrs.push_back(addr.toString());
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== bitchat relay booting ===");

  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(TX_POWER_DBM);     // 2.x: plain dBm, NOT ESP_PWR_LVL_*
  NimBLEDevice::setMTU(DESIRED_MTU);

  // ---- peripheral side ----
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new RelayServerCallbacks());
  pServer->advertiseOnDisconnect(true);     // 2.x does NOT do this automatically

  NimBLEService* svc = pServer->createService(BITCHAT_SERVICE_UUID);
  pRelayChar = svc->createCharacteristic(
      BITCHAT_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY,
      512);
  pRelayChar->setCallbacks(new RelayCharCallbacks());
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->setName(DEVICE_NAME);                // 2.x: name is not advertised by default
  adv->addServiceUUID(BITCHAT_SERVICE_UUID);
  adv->enableScanResponse(true);            // 2.x: off by default
  adv->start();
  Serial.println("[srv ] advertising as bitchat peer");

  // ---- central side ----
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(90);                      // near-continuous; we're mains powered
  scan->start(0, false);                    // 0 = scan forever
  Serial.println("[scan] scanning for bitchat peers");
}

static uint32_t lastBeat = 0;

void loop() {
  if (doConnect) {
    doConnect = false;
    NimBLEDevice::getScan()->stop();
    connectToPeer(targetAddr);
    NimBLEDevice::getScan()->start(0, false);
  }

  if (millis() - lastBeat > 15000) {
    lastBeat = millis();
    Serial.printf("[stat] links=%u  heap=%u\n",
                  (unsigned)links.size(), (unsigned)ESP.getFreeHeap());
  }
  delay(20);
}
