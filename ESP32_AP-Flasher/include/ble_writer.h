#pragma once
#ifdef HAS_BLE_WRITER

#include <stdint.h>

class NimBLEAddress;
class NimBLEClient;
class NimBLEClientCallbacks;

void BLETask(void* parameter);

// The one NimBLE client all uploads share. NimBLE clients are reusable: connect() with deleteAttributes drops the
// previous connection's service cache. Returned disconnected from any earlier peer, with the given callbacks.
NimBLEClient* BLE_getClient(NimBLEClientCallbacks* callbacks);

// BLE address of a tag, from its tagDB MAC (bytes 0..5 = address, least significant byte first; 6..7 = device type
// bytes), with the address type (public/random) it was last seen advertising with. NimBLE, unlike Bluedroid, needs
// the right type to connect.
NimBLEAddress BLE_tagAddress(const uint8_t mac[8]);

// Called by the scan filter for every supported device, to remember its address type for BLE_tagAddress().
void BLE_rememberAddressType(const uint8_t mac[8], uint8_t addressType);

// NimBLE starts the MTU exchange when it connects but doesn't wait for it. Large writes issued before it completes
// would go out as long (prepare/execute) writes, which simple tags may not support. Waits up to timeoutMs and returns
// the MTU in effect.
uint16_t BLE_waitForMtu(NimBLEClient* client, uint32_t timeoutMs);

#endif
