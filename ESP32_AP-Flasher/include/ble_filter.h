#pragma once
#ifdef HAS_BLE_WRITER
#include <stdint.h>

// One device seen during a scan. The NimBLE host task only copies reports into these; BLETask parses them after the
// scan (see BLE_startScan()).
struct BLE_ScanRecord {
    uint8_t addr[6];  // as NimBLE reports it: least significant byte first
    uint8_t addrType;  // BLE_ADDR_PUBLIC / BLE_ADDR_RANDOM
    int8_t rssi;
    uint8_t payloadLen;
    uint8_t payload[64];  // advertisement followed by the scan response (legacy advertising: at most 62 bytes)
};

uint8_t gicToOEPLtype(uint8_t gicType);
uint8_t wolinkToOEPLtype(uint16_t modelId, uint16_t hwVersion);
bool BLE_filter_add_device(const BLE_ScanRecord& device);
bool BLE_is_image_pending(uint8_t address[8]);
uint32_t compress_image(uint8_t address[8], uint8_t* buffer, uint32_t max_len);
uint32_t pack_wolink_image(uint8_t address[8], uint8_t* buffer, uint32_t max_len);
uint32_t get_ATC_BLE_OEPL_image(uint8_t address[8], uint8_t* buffer, uint32_t max_len, uint8_t* dataType, uint8_t* dataTypeArgument, uint16_t* nextCheckIn);

#endif
