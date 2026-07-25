// Minimal NimBLE-Arduino 2.x surface for host compile-checking.
// Every signature transcribed from the real NimBLE-Arduino 2.3.6 headers.
// See stubs/README.md — this is a type-check harness, not a simulator.
#ifndef STUB_NIMBLEDEVICE_H
#define STUB_NIMBLEDEVICE_H

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#define BLE_HS_CONN_HANDLE_NONE 0xFFFF
#define BLE_ATT_ATTR_MAX_LEN    512

// NimBLELocalValueAttribute.h
typedef enum {
  READ         = 0x0002,
  READ_ENC     = 0x0004,
  READ_AUTHEN  = 0x0008,
  READ_AUTHOR  = 0x0010,
  WRITE        = 0x0008,
  WRITE_NR     = 0x0020,
  WRITE_ENC    = 0x0040,
  WRITE_AUTHEN = 0x0080,
  WRITE_AUTHOR = 0x0100,
  BROADCAST    = 0x0001,
  NOTIFY       = 0x0010,
  INDICATE     = 0x0020
} NIMBLE_PROPERTY;

class NimBLEClient;
class NimBLEServer;
class NimBLEService;
class NimBLECharacteristic;
class NimBLEAdvertising;
class NimBLEScan;
class NimBLERemoteService;
class NimBLERemoteCharacteristic;
class NimBLEAdvertisedDevice;
class NimBLEScanResults;

// --- NimBLEUUID -----------------------------------------------------------
class NimBLEUUID {
 public:
  NimBLEUUID() {}
  NimBLEUUID(const char*) {}
  std::string toString() const { return std::string(); }
};

// --- NimBLEAddress --------------------------------------------------------
class NimBLEAddress {
 public:
  NimBLEAddress() {}
  std::string toString() const { return std::string(); }
};

// --- NimBLEAttValue -------------------------------------------------------
class NimBLEAttValue {
 public:
  uint16_t       length() const { return 0; }
  const uint8_t* data() const { return nullptr; }
};

// --- NimBLEConnInfo -------------------------------------------------------
class NimBLEConnInfo {
 public:
  uint16_t getConnHandle() const { return 0; }
  uint16_t getMTU() const { return 0; }
};

// --- callbacks ------------------------------------------------------------
class NimBLECharacteristicCallbacks {
 public:
  virtual ~NimBLECharacteristicCallbacks() {}
  virtual void onRead(NimBLECharacteristic*, NimBLEConnInfo&) {}
  virtual void onWrite(NimBLECharacteristic*, NimBLEConnInfo&) {}
  virtual void onStatus(NimBLECharacteristic*, int) {}
  virtual void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t) {}
};

class NimBLEServerCallbacks {
 public:
  virtual ~NimBLEServerCallbacks() {}
  virtual void onConnect(NimBLEServer*, NimBLEConnInfo&) {}
  virtual void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) {}
  virtual void onMTUChange(uint16_t, NimBLEConnInfo&) {}
};

class NimBLEClientCallbacks {
 public:
  virtual ~NimBLEClientCallbacks() {}
  virtual void onConnect(NimBLEClient*) {}
  virtual void onConnectFail(NimBLEClient*, int) {}
  virtual void onDisconnect(NimBLEClient*, int) {}
  virtual void onMTUChange(NimBLEClient*, uint16_t) {}
};

class NimBLEScanCallbacks {
 public:
  virtual ~NimBLEScanCallbacks() {}
  virtual void onDiscovered(const NimBLEAdvertisedDevice*) {}
  virtual void onResult(const NimBLEAdvertisedDevice*) {}
  virtual void onScanEnd(const NimBLEScanResults&, int) {}
};

// --- GATT server side -----------------------------------------------------
class NimBLECharacteristic {
 public:
  void           setCallbacks(NimBLECharacteristicCallbacks* pCallbacks);
  bool           notify(const uint8_t* value, size_t length,
                        uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE) const;
  NimBLEAttValue getValue() const;
};

class NimBLEService {
 public:
  NimBLECharacteristic* createCharacteristic(
      const char* uuid,
      uint32_t    properties = NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE,
      uint16_t    max_len    = BLE_ATT_ATTR_MAX_LEN);
  bool start();
};

class NimBLEServer {
 public:
  void           setCallbacks(NimBLEServerCallbacks* pCallbacks, bool deleteCallbacks = true);
  NimBLEService* createService(const char* uuid);
  uint16_t       getPeerMTU(uint16_t connHandle) const;
  void           advertiseOnDisconnect(bool enable);
};

// --- advertising ----------------------------------------------------------
class NimBLEAdvertising {
 public:
  bool start(uint32_t duration = 0, const NimBLEAddress* dirAddr = nullptr);
  bool stop();
  void enableScanResponse(bool enable);
  bool setName(const std::string& name);
  bool addServiceUUID(const char* serviceUUID);
};

// --- scanning -------------------------------------------------------------
class NimBLEAdvertisedDevice {
 public:
  bool                 isAdvertisingService(const NimBLEUUID& uuid) const;
  const NimBLEAddress& getAddress() const;
  int8_t               getRSSI() const;
};

class NimBLEScanResults {};

class NimBLEScan {
 public:
  bool start(uint32_t duration, bool isContinue = false, bool restart = true);
  bool stop();
  void setScanCallbacks(NimBLEScanCallbacks* pScanCallbacks, bool wantDuplicates = false);
  void setActiveScan(bool active);
  void setInterval(uint16_t intervalMs);
  void setWindow(uint16_t windowMs);
};

// --- GATT client side -----------------------------------------------------
class NimBLERemoteCharacteristic {
 public:
  typedef std::function<void(NimBLERemoteCharacteristic*, uint8_t*, size_t, bool)> notify_callback;

  bool          canNotify() const;
  bool          canWrite() const;
  bool          subscribe(bool notifications = true,
                          const notify_callback notifyCallback = nullptr,
                          bool response = true) const;
  bool          writeValue(const uint8_t* data, size_t length, bool response = false) const;
  NimBLEClient* getClient() const;
};

class NimBLERemoteService {
 public:
  NimBLERemoteCharacteristic* getCharacteristic(const char* uuid) const;
  NimBLEClient*               getClient() const;
};

class NimBLEClient {
 public:
  bool                 connect(const NimBLEAddress& address, bool deleteAttributes = true,
                               bool asyncConnect = false, bool exchangeMTU = true);
  bool                 disconnect();
  bool                 isConnected();
  void                 setSelfDelete(bool deleteOnDisconnect, bool deleteOnConnectFail);
  void                 setClientCallbacks(NimBLEClientCallbacks* pClientCallbacks,
                                          bool deleteCallbacks = true);
  uint16_t             getMTU() const;
  uint16_t             getConnHandle() const;
  NimBLEAddress        getPeerAddress() const;
  NimBLERemoteService* getService(const char* uuid);
};

// --- device ---------------------------------------------------------------
class NimBLEDevice {
 public:
  static bool               init(const std::string& deviceName);
  static bool               setPower(int8_t dbm);
  static bool               setMTU(uint16_t mtu);
  static NimBLEServer*      createServer();
  static NimBLEAdvertising* getAdvertising();
  static NimBLEScan*        getScan();
  static NimBLEClient*      createClient();
  static bool               deleteClient(NimBLEClient* pClient);
};

#endif  // STUB_NIMBLEDEVICE_H
