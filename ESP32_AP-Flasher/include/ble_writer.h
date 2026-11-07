#pragma once
#ifdef HAS_BLE_WRITER

class BLEClient;
class BLEClientCallbacks;
class BLERemoteService;
class BLEUUID;

void BLETask(void* parameter);

// Use these instead of BLEDevice::createClient() / BLEClient::getService(): the client of the previous upload is
// freed once it is safely torn down (the Arduino-ESP32 2.x BLE library would otherwise leak it on every connection).
BLEClient* BLE_acquireClient(BLEClientCallbacks* callbacks);
BLERemoteService* BLE_getService(BLEClient* client, BLEUUID uuid);

#endif
