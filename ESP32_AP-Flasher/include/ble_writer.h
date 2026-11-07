#pragma once
#ifdef HAS_BLE_WRITER

class BLEClient;
class BLEClientCallbacks;
class BLERemoteService;
class BLEUUID;

void BLETask(void* parameter);

// Use these instead of BLEDevice::createClient() / BLEClient::getService(): clients of earlier uploads are freed once
// the BT stack can no longer reach them (the Arduino-ESP32 2.x BLE library would otherwise leak one per connection).
BLEClient* BLE_acquireClient(BLEClientCallbacks* callbacks);
BLERemoteService* BLE_getService(BLEClient* client, BLEUUID uuid);

#endif
