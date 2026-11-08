#ifdef HAS_BLE_WRITER
#include <Arduino.h>
#include <MD5Builder.h>
#include <NimBLEDevice.h>
#include <mbedtls/aes.h>

#include "ble_filter.h"
#include "ble_writer.h"
#include "newproto.h"
#include "supervisor.h"
#include "tag_db.h"
#include "web.h"
#ifdef HAS_NEMONIC_PRINTER
#include "nemonic_printer.h"
#endif

#define INTERVAL_BLE_SCANNING_SECONDS 60
#define INTERVAL_HANDLE_PENDING_SECONDS 10
#define BUFFER_MAX_SIZE_COMPRESSING 135000

#define BLE_MAIN_STATE_IDLE 0
#define BLE_MAIN_STATE_PREPARE 1
#define BLE_MAIN_STATE_CONNECT 2
#define BLE_MAIN_STATE_UPLOAD 3
#define BLE_MAIN_STATE_ATC_BLE_OEPL_UPLOAD 4

int ble_main_state = BLE_MAIN_STATE_IDLE;
uint32_t last_ble_scan = 0;

#define BLE_UPLOAD_STATE_INIT 0
#define BLE_UPLOAD_STATE_SIZE 1
#define BLE_UPLOAD_STATE_START 2
#define BLE_UPLOAD_STATE_UPLOAD 5
int BLE_upload_state = BLE_UPLOAD_STATE_INIT;

#define BLE_CMD_ACK_CMD 99
#define BLE_CMD_AVAILDATA 100
#define BLE_CMD_BLK_DATA 101
#define BLE_CMD_ERR_BLKPRT 196
#define BLE_CMD_ACK_BLKPRT 197
#define BLE_CMD_REQ 198
#define BLE_CMD_ACK 199
#define BLE_CMD_ACK_IS_SHOWN 200
#define BLE_CMD_ACK_FW_UPDATED 201

struct AvailDataInfo BLEavaildatainfo = {0};
struct blockRequest BLEblkRequst = {0};

bool BLE_connected = false;
bool BLE_new_notify = false;

static NimBLEUUID ATC_BLE_OEPL_ServiceUUID((uint16_t)0x1337);
static NimBLEUUID ATC_BLE_OEPL_CtrlUUID((uint16_t)0x1337);

static NimBLEUUID gicServiceUUID((uint16_t)0xfef0);
static NimBLEUUID gicCtrlUUID((uint16_t)0xfef1);
static NimBLEUUID gicImgUUID((uint16_t)0xfef2);

// Wolink/Zhsunyco BLE ESL service and characteristic UUIDs.
// Source: NickWaterton/Wolink wolink_ble.py.
static NimBLEUUID wolinkServiceUUID("30323032-4c53-4545-4c42-4b4e494c4f57");
static NimBLEUUID wolinkDataUUID("31323032-4c53-4545-4c42-4b4e494c4f57");
static NimBLEUUID wolinkAuthUUID("33323032-4c53-4545-4c42-4b4e494c4f57");
static NimBLEUUID wolinkStatusUUID("34323032-4c53-4545-4c42-4b4e494c4f57");

static const uint8_t WOLINK_AES_KEY[16] = {
    0x9B, 0x60, 0x9F, 0x28, 0xBC, 0x49, 0xE2, 0x57,
    0x29, 0xBD, 0x7B, 0x8D, 0xF2, 0x2B, 0x44, 0x20};

NimBLERemoteCharacteristic* ctrlChar = nullptr;
NimBLERemoteCharacteristic* imgChar = nullptr;
NimBLEClient* pClient = nullptr;

uint8_t BLE_notify_buffer[256] = {0};

uint32_t BLE_err_counter = 0;
uint64_t BLE_curr_dataVer = 0;
uint32_t BLE_curr_part = 0;
uint32_t BLE_max_block_parts = 0;
uint8_t BLE_mini_buff[256];

uint32_t BLE_last_notify = 0;
uint32_t BLE_last_pending_check = 0;
uint8_t BLE_curr_address[8] = {0};

uint32_t BLE_compressed_len = 0;
uint8_t* BLE_image_buffer = nullptr;

// Only BLETask allocates and frees this buffer. Always go through here so a pointer is never freed twice.
static void BLE_freeImageBuffer() {
    if (BLE_image_buffer != nullptr) {
        free(BLE_image_buffer);
        BLE_image_buffer = nullptr;
    }
}

static void notifyCallback(
    NimBLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData,
    size_t length,
    bool isNotify) {
    if (length > sizeof(BLE_notify_buffer) - 1) length = sizeof(BLE_notify_buffer) - 1;  // [0] holds the length
    Serial.print("Notify callback for characteristic ");
    Serial.print(pBLERemoteCharacteristic->getUUID().toString().c_str());
    Serial.print(" of data length ");
    Serial.println(length);
    Serial.print("data: ");
    for (int i = 0; i < length; i++) {
        Serial.printf("%02X", pData[i]);
        BLE_notify_buffer[1 + i] = pData[i];
    }
    BLE_notify_buffer[0] = length;
    Serial.println();
    BLE_new_notify = true;
}

class MyClientCallback : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient* pclient) override {
        Serial.println("BLE onConnect");
        BLE_connected = true;
    }

    void onDisconnect(NimBLEClient* pclient, int reason) override {
        Serial.printf("BLE onDisconnect, reason %d\r\n", reason);
        BLE_connected = false;
        ble_main_state = BLE_MAIN_STATE_IDLE;
    }
};

// One instance for all connections; the Bluedroid version `new`-ed one per connection and never freed it.
static MyClientCallback bleClientCallbacks;

// Tags are addressed by their tagDB MAC, which has no room for the BLE address type. NimBLE needs that type to
// connect (a random static address dialled as public never answers), so the scan filter records it here for every
// supported device. Unknown addresses fall back to public, which is what the Bluedroid code always used.
#define BLE_ADDRESS_TYPE_SLOTS 64
struct AddressTypeSlot {
    uint8_t mac[6];
    uint8_t type;
    bool used;
};
static AddressTypeSlot addressTypeSlots[BLE_ADDRESS_TYPE_SLOTS];
static uint8_t addressTypeNextSlot = 0;

void BLE_rememberAddressType(const uint8_t mac[8], uint8_t addressType) {
    // Only BLETask touches this table (scan results are parsed there).
    int slot = -1;
    for (int i = 0; i < BLE_ADDRESS_TYPE_SLOTS; i++) {
        if (addressTypeSlots[i].used && memcmp(addressTypeSlots[i].mac, mac, 6) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {  // new device: take the oldest slot
        slot = addressTypeNextSlot;
        addressTypeNextSlot = (addressTypeNextSlot + 1) % BLE_ADDRESS_TYPE_SLOTS;
        memcpy(addressTypeSlots[slot].mac, mac, 6);
        addressTypeSlots[slot].used = true;
    }
    addressTypeSlots[slot].type = addressType;
}

NimBLEAddress BLE_tagAddress(const uint8_t mac[8]) {
    uint8_t type = BLE_ADDR_PUBLIC;
    for (int i = 0; i < BLE_ADDRESS_TYPE_SLOTS; i++) {
        if (addressTypeSlots[i].used && memcmp(addressTypeSlots[i].mac, mac, 6) == 0) {
            type = addressTypeSlots[i].type;
            break;
        }
    }
    const uint8_t msbFirst[6] = {mac[5], mac[4], mac[3], mac[2], mac[1], mac[0]};
    return NimBLEAddress(msbFirst, type);
}

NimBLEClient* BLE_getClient(NimBLEClientCallbacks* callbacks) {
    static NimBLEClient* client = nullptr;
    if (client == nullptr) client = NimBLEDevice::createClient();
    if (client == nullptr) return nullptr;
    // isConnected() turns false as soon as disconnect() is called, but connect() refuses to start until the
    // disconnect event has released the connection handle.
    if (client->getConnHandle() != BLE_HS_CONN_HANDLE_NONE) {
        client->disconnect();
        const uint32_t t0 = millis();
        while (client->getConnHandle() != BLE_HS_CONN_HANDLE_NONE && millis() - t0 < 5000) {
            vTaskDelay(20 / portTICK_PERIOD_MS);
        }
        if (client->getConnHandle() != BLE_HS_CONN_HANDLE_NONE) {
            Serial.println("BLE: previous connection did not close, skipping");
            return nullptr;
        }
        vTaskDelay(20 / portTICK_PERIOD_MS);  // the host task marks the client disconnected right after the handle
    }
    client->setClientCallbacks(callbacks, false);  // our callback objects are static: the client must not delete them
    return client;
}

uint16_t BLE_waitForMtu(NimBLEClient* client, uint32_t timeoutMs) {
    const uint32_t t0 = millis();
    while (client->isConnected() && client->getMTU() <= 23 && millis() - t0 < timeoutMs) {  // 23 = default ATT MTU
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
    return client->getMTU();
}

enum BLE_CONNECTION_TYPE {
    BLE_TYPE_GICISKY = 0,
    BLE_TYPE_ATC_BLE_OEPL
};

bool BLE_connect(uint8_t addr[8], BLE_CONNECTION_TYPE conn_type) {
    // BLE_err_counter is only for block-part retries now; resetting it here kept the old give-up from ever firing
    ctrlChar = nullptr;  // they belong to the previous connection's attribute tree, which connect() deletes
    imgChar = nullptr;
    Serial.printf("BLE Connecting to: %02X:%02X:%02X:%02X:%02X:%02X\r\n", addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    pClient = BLE_getClient(&bleClientCallbacks);
    if (pClient == nullptr || !pClient->connect(BLE_tagAddress(addr), true)) {
        Serial.printf("BLE connection failed\r\n");
        return false;
    }
    uint32_t timeStart = millis();
    while (millis() - timeStart <= 5000) {  // We wait for a few seconds as otherwise the connection might not be ready!
        delay(100);
    }
    if (!BLE_connected)
        return false;
    Serial.printf("BLE starting to get service\r\n");
    NimBLERemoteService* pRemoteService = pClient->getService((conn_type == BLE_TYPE_GICISKY) ? gicServiceUUID : ATC_BLE_OEPL_ServiceUUID);
    if (pRemoteService == nullptr) {
        Serial.printf("BLE Service failed\r\n");
        pClient->disconnect();
        return false;
    }
    if (conn_type == BLE_TYPE_GICISKY) {
        imgChar = pRemoteService->getCharacteristic(gicImgUUID);
        if (imgChar == nullptr) {
            Serial.printf("BLE IMG Char failed\r\n");
            pClient->disconnect();
            return false;
        }
    }
    ctrlChar = pRemoteService->getCharacteristic((conn_type == BLE_TYPE_GICISKY) ? gicCtrlUUID : ATC_BLE_OEPL_CtrlUUID);
    if (ctrlChar == nullptr) {
        Serial.printf("BLE ctrl Char failed\r\n");
        pClient->disconnect();
        return false;
    }
    if (!ctrlChar->canNotify()) {
        Serial.printf("BLE Notify failed\r\n");
        pClient->disconnect();
        return false;
    }
    if (!ctrlChar->subscribe(true, notifyCallback)) {
        // registerForNotify() ignored a failed CCCD write too; notifications may still arrive.
        Serial.printf("BLE notify subscription failed (continuing)\r\n");
    }
    // Gicisky parts are 4 + 240 bytes, ATC block parts 2 + 3 + 230 bytes, plus 3 bytes ATT header. Below that NimBLE
    // would turn them into long writes, which it silently truncates if the tag rejects them.
    const uint16_t mtuNeeded = (conn_type == BLE_TYPE_GICISKY) ? (4 + 240 + 3) : (2 + 3 + 230 + 3);
    const uint16_t mtu = BLE_waitForMtu(pClient, 3000);
    if (mtu < mtuNeeded) {
        Serial.printf("BLE MTU %u too small (need %u)\r\n", mtu, mtuNeeded);
        pClient->disconnect();
        return false;
    }
    Serial.printf("BLE Connected fully to: %02X:%02X:%02X:%02X:%02X:%02X, MTU %u\r\n", addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], mtu);
    return true;
}

// Scan reports are only copied here by the NimBLE host task and parsed by BLETask once the scan is over. The host
// task also has to drain the controller's 8 advertising-report buffers; parsing in it (processDataReq, serial logging
// at 115200 baud) made it drop reports.
#define BLE_SCAN_RECORDS_MAX 128
static BLE_ScanRecord* scanRecords = nullptr;  // allocated once (PSRAM when available)
static uint16_t scanRecordCount = 0;
static bool scanRecordsOverflow = false;
static SemaphoreHandle_t scanRecordsMutex = nullptr;

class MyScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        if (scanRecords == nullptr || scanRecordsMutex == nullptr || xSemaphoreTake(scanRecordsMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
        const uint8_t* addr = advertisedDevice->getAddress().getVal();
        const std::vector<uint8_t>& payload = advertisedDevice->getPayload();
        const uint8_t len = payload.size() < sizeof(BLE_ScanRecord::payload) ? payload.size() : sizeof(BLE_ScanRecord::payload);
        BLE_ScanRecord* rec = nullptr;
        for (uint16_t i = 0; i < scanRecordCount; i++) {
            if (memcmp(scanRecords[i].addr, addr, 6) == 0) {
                rec = &scanRecords[i];
                break;
            }
        }
        if (rec == nullptr) {
            if (scanRecordCount < BLE_SCAN_RECORDS_MAX) {
                rec = &scanRecords[scanRecordCount++];
                memcpy(rec->addr, addr, 6);
                rec->payloadLen = 0;
            } else {
                scanRecordsOverflow = true;
            }
        }
        // Keep the most complete report: advertisement plus scan response beats either part alone.
        if (rec != nullptr && len >= rec->payloadLen) {
            rec->addrType = advertisedDevice->getAddress().getType();
            rec->rssi = advertisedDevice->getRSSI();
            rec->payloadLen = len;
            memcpy(rec->payload, payload.data(), len);
        }
        xSemaphoreGive(scanRecordsMutex);
    }
};

// One instance for all scans; the Bluedroid version `new`-ed (and leaked) one on every scan, i.e. once a minute.
static MyScanCallbacks bleScanCallbacks;

void BLE_startScan(uint32_t timeout) {
    if (scanRecordsMutex == nullptr) scanRecordsMutex = xSemaphoreCreateMutex();
    if (scanRecords == nullptr) scanRecords = static_cast<BLE_ScanRecord*>(malloc(sizeof(BLE_ScanRecord) * BLE_SCAN_RECORDS_MAX));
    if (scanRecords == nullptr || scanRecordsMutex == nullptr) return;

    xSemaphoreTake(scanRecordsMutex, portMAX_DELAY);
    scanRecordCount = 0;
    scanRecordsOverflow = false;
    xSemaphoreGive(scanRecordsMutex);

    NimBLEScan* pBLEScan = NimBLEDevice::getScan();
    // Controller duplicate filter off, as with the Bluedroid library: a report the host had to drop is replaced by the
    // device's next advertisement. Duplicates are merged in scanRecords instead.
    pBLEScan->setScanCallbacks(&bleScanCallbacks, true);
    pBLEScan->setInterval(1349);
    pBLEScan->setWindow(449);
    pBLEScan->setActiveScan(true);
    pBLEScan->setMaxResults(0);                   // everything goes through the callback, nothing is kept
    pBLEScan->getResults(timeout * 1000, false);  // blocks this task for the duration of the scan

    xSemaphoreTake(scanRecordsMutex, portMAX_DELAY);
    if (scanRecordsOverflow) Serial.printf("BLE scan: more than %d devices, the rest were ignored\r\n", BLE_SCAN_RECORDS_MAX);
    for (uint16_t i = 0; i < scanRecordCount; i++) {
        BLE_filter_add_device(scanRecords[i]);
        supervisorHeartbeat(SUPERVISED_BLE);
    }
    xSemaphoreGive(scanRecordsMutex);
}

#define BLOCK_DATA_SIZE_BLE 4096
#define BLOCK_PART_DATA_SIZE_BLE 230
uint8_t tempBlockBuffer[BLOCK_DATA_SIZE_BLE + 4];
uint8_t tempPacketBuffer[2 + 3 + BLOCK_PART_DATA_SIZE_BLE];
void ATC_BLE_OEPL_PrepareBlk(uint8_t indexBlockId) {
    if (BLE_image_buffer == nullptr) {
        return;
    }
    uint32_t bufferPosition = (BLOCK_DATA_SIZE_BLE * indexBlockId);
    uint32_t lenNow = BLOCK_DATA_SIZE_BLE;
    uint16_t crcCalc = 0;
    if ((BLE_compressed_len - bufferPosition) < BLOCK_DATA_SIZE_BLE)
        lenNow = (BLE_compressed_len - bufferPosition);
    tempBlockBuffer[0] = lenNow & 0xff;
    tempBlockBuffer[1] = (lenNow >> 8) & 0xff;
    for (uint16_t c = 0; c < lenNow; c++) {
        tempBlockBuffer[4 + c] = BLE_image_buffer[c + bufferPosition];
        crcCalc += tempBlockBuffer[4 + c];
    }
    tempBlockBuffer[2] = crcCalc & 0xff;
    tempBlockBuffer[3] = (crcCalc >> 8) & 0xff;
    BLE_max_block_parts = (4 + lenNow) / BLOCK_PART_DATA_SIZE_BLE;
    if ((4 + lenNow) % BLOCK_PART_DATA_SIZE_BLE)
        BLE_max_block_parts++;
    Serial.println("Preparing block: " + String(indexBlockId) + " BuffPos: " + String(bufferPosition) + " LenNow: " + String(lenNow) + " MaxBLEparts: " + String(BLE_max_block_parts));
    BLE_curr_part = 0;
}

void ATC_BLE_OEPL_SendPart(uint8_t indexBlockId, uint8_t indexPkt) {
    uint8_t crcCalc = indexBlockId + indexPkt;
    for (uint16_t c = 0; c < BLOCK_PART_DATA_SIZE_BLE; c++) {
        tempPacketBuffer[5 + c] = tempBlockBuffer[c + (BLOCK_PART_DATA_SIZE_BLE * indexPkt)];
        crcCalc += tempPacketBuffer[5 + c];
    }
    tempPacketBuffer[0] = 0x00;
    tempPacketBuffer[1] = 0x65;
    tempPacketBuffer[2] = crcCalc;
    tempPacketBuffer[3] = indexBlockId;
    tempPacketBuffer[4] = indexPkt;
    Serial.println("BLE Sending packet Len " + String(sizeof(tempPacketBuffer)));
    ctrlChar->writeValue(tempPacketBuffer, sizeof(tempPacketBuffer), true);
}

// Wolink upload runs to completion inside the BLE task (single-threaded).
// State for the status notify callback lives at file scope so we can capture
// the two-byte status word that the ESL emits while it processes the upload.
volatile bool wolink_status_received = false;
volatile uint8_t wolink_status[2] = {0xFF, 0xFF};

static void wolinkStatusNotifyCallback(
    NimBLERemoteCharacteristic* pBLERemoteCharacteristic,
    uint8_t* pData,
    size_t length,
    bool isNotify) {
    if (length >= 2) {
        wolink_status[0] = pData[0];
        wolink_status[1] = pData[1];
    } else if (length == 1) {
        wolink_status[0] = pData[0];
        wolink_status[1] = 0;
    }
    wolink_status_received = true;
    Serial.printf("Wolink status notify: ");
    for (size_t i = 0; i < length; i++) Serial.printf("%02X", pData[i]);
    Serial.println();
}

// AES-128-CBC encrypt the 16-byte challenge with the hardcoded Wolink key,
// zero IV, and PKCS#7 padding to 32 bytes; we keep only the first 16-byte
// ciphertext block as the response.
static void wolink_aes_encrypt_challenge(const uint8_t challenge[16], uint8_t out[16]) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, WOLINK_AES_KEY, 128);
    uint8_t iv[16] = {0};
    uint8_t padded[32];
    memcpy(padded, challenge, 16);
    memset(padded + 16, 0x10, 16);  // PKCS#7: full second block of 0x10
    uint8_t cipher[32];
    mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, 32, iv, padded, cipher);
    memcpy(out, cipher, 16);
    mbedtls_aes_free(&ctx);
}

// Run a complete Wolink image upload: connect, authenticate, send pixels,
// trigger refresh, wait for the EPD to report idle. Returns true on success.
static bool wolink_upload(uint8_t address[8]) {
    Serial.printf("Wolink connecting to %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                  address[5], address[4], address[3], address[2], address[1], address[0]);

    pClient = BLE_getClient(&bleClientCallbacks);
    if (pClient == nullptr || !pClient->connect(BLE_tagAddress(address), true)) {
        Serial.println("Wolink connect failed");
        return false;
    }

    uint32_t t0 = millis();
    while (!BLE_connected && millis() - t0 < 5000) delay(50);
    if (!BLE_connected) {
        Serial.println("Wolink connect did not settle");
        pClient->disconnect();
        return false;
    }
    // Chunks are sized from the MTU below. Too small an MTU (exchange failed) would make the upload crawl.
    const uint16_t mtu = BLE_waitForMtu(pClient, 3000);
    if (mtu < 100) {
        Serial.printf("Wolink MTU %u too small\r\n", mtu);
        pClient->disconnect();
        return false;
    }

    NimBLERemoteService* svc = pClient->getService(wolinkServiceUUID);
    if (!svc) {
        Serial.println("Wolink service not found");
        pClient->disconnect();
        return false;
    }
    NimBLERemoteCharacteristic* dataChar = svc->getCharacteristic(wolinkDataUUID);
    NimBLERemoteCharacteristic* authChar = svc->getCharacteristic(wolinkAuthUUID);
    NimBLERemoteCharacteristic* statusChar = svc->getCharacteristic(wolinkStatusUUID);
    if (!dataChar || !authChar || !statusChar) {
        Serial.println("Wolink characteristic discovery failed");
        pClient->disconnect();
        return false;
    }
    if (statusChar->canNotify()) {
        wolink_status_received = false;
        wolink_status[0] = 0xFF;
        wolink_status[1] = 0xFF;
        if (!statusChar->subscribe(true, wolinkStatusNotifyCallback)) {
            Serial.println("Wolink status notify subscription failed (continuing)");
        }
    } else {
        Serial.println("Wolink status char does not notify");
    }

    // 1) Authenticate: read 16-byte challenge, write AES-CBC response.
    NimBLEAttValue challenge = authChar->readValue();
    if (challenge.size() < 16) {
        Serial.printf("Wolink auth challenge too short (%u)\r\n", (unsigned)challenge.size());
        pClient->disconnect();
        return false;
    }
    uint8_t resp[16];
    wolink_aes_encrypt_challenge(challenge.data(), resp);
    Serial.printf("Wolink auth challenge=");
    for (int i = 0; i < 16; i++) Serial.printf("%02X", challenge.data()[i]);
    Serial.printf(" resp=");
    for (int i = 0; i < 16; i++) Serial.printf("%02X", resp[i]);
    Serial.println();
    authChar->writeValue(resp, 16, false);

    // 2) Pack the pixel buffer.
    BLE_image_buffer = (uint8_t*)malloc(BUFFER_MAX_SIZE_COMPRESSING);
    if (BLE_image_buffer == nullptr) {
        Serial.println("Wolink: pixel buffer alloc failed");
        pClient->disconnect();
        return false;
    }
    BLE_compressed_len = pack_wolink_image(address, BLE_image_buffer, BUFFER_MAX_SIZE_COMPRESSING);
    if (BLE_compressed_len == 0) {
        Serial.println("Wolink: pack failed");
        BLE_freeImageBuffer();
        pClient->disconnect();
        return false;
    }
    Serial.printf("Wolink packed %u bytes\r\n", (unsigned)BLE_compressed_len);

    // 3) Upload chunks of (mtu-9) bytes with [0x00,0xA5] + offset(LE32) header, at most 238 (MTU 247) as before.
    //    Writes longer than MTU-3 would become long writes, which NimBLE truncates if the tag rejects them.
    const size_t chunk_size = (mtu - 9) < 238 ? (mtu - 9) : 238;
    uint8_t pkt[6 + 238];
    for (uint32_t off = 0; off < BLE_compressed_len; off += chunk_size) {
        size_t this_n = (BLE_compressed_len - off) < chunk_size ? (BLE_compressed_len - off) : chunk_size;
        pkt[0] = 0x00;
        pkt[1] = 0xA5;
        pkt[2] = off & 0xFF;
        pkt[3] = (off >> 8) & 0xFF;
        pkt[4] = (off >> 16) & 0xFF;
        pkt[5] = (off >> 24) & 0xFF;
        memcpy(pkt + 6, BLE_image_buffer + off, this_n);
        if (!dataChar->writeValue(pkt, 6 + this_n, true) && !pClient->isConnected()) {
            // NimBLE fails writes on a dropped link right away (Bluedroid could hang instead). Without this check
            // the remaining chunks would "send" instantly and the disconnect below would pass for success.
            Serial.printf("Wolink: link lost at offset %u\r\n", (unsigned)off);
            BLE_freeImageBuffer();
            return false;
        }
        supervisorHeartbeat(SUPERVISED_BLE);  // a 7.5" image is ~400 chunks
        delay(15);
    }

    // 4) Trigger refresh: [0x01,0xA5] + total_len(LE32).
    uint8_t refresh[6];
    refresh[0] = 0x01;
    refresh[1] = 0xA5;
    refresh[2] = BLE_compressed_len & 0xFF;
    refresh[3] = (BLE_compressed_len >> 8) & 0xFF;
    refresh[4] = (BLE_compressed_len >> 16) & 0xFF;
    refresh[5] = (BLE_compressed_len >> 24) & 0xFF;
    dataChar->writeValue(refresh, 6, true);
    Serial.println("Wolink refresh triggered, waiting for first status notify or disconnect");

    // 5) Wait for the FIRST status notify, OR a disconnect. Per upstream
    // wolink_ble.py decode_status(): status[0] is busy(0xFF)/idle(0x00) and
    // status[1] is the error code (0=ok, 1..5=EPD/OTA/unlock errors). The
    // ESL routinely emits one busy notify mid-upload and then disconnects
    // immediately after refresh without ever emitting an idle notify, so
    // we mirror Python's "first notify ends the wait" semantics: any
    // notify with err==0 is success, otherwise error. A disconnect with
    // chunks already sent is treated as success too (refresh accepted).
    bool ok = false;
    uint32_t t_start = millis();
    while (millis() - t_start < 30000) {
        if (wolink_status_received) {
            ok = (wolink_status[1] == 0x00);
            Serial.printf("Wolink upload %s (status %02X %02X)\r\n",
                          ok ? "OK" : "ERR", wolink_status[0], wolink_status[1]);
            break;
        }
        if (!BLE_connected) {
            ok = true;
            Serial.println("Wolink upload OK (disconnected after refresh, no final notify)");
            break;
        }
        delay(50);
    }
    if (!ok && millis() - t_start >= 30000) {
        Serial.println("Wolink: status wait timed out");
    }

    BLE_freeImageBuffer();
    pClient->disconnect();
    return ok;
}

#define BLE_MAX_TRIES 5  // per queued item and device

// The item BLETask sent (BLE_curr_dataVer) reached the device: count the update, start the tries afresh.
static void BLE_reportDelivered() {
    struct espXferComplete reportStruct;
    memcpy((uint8_t*)&reportStruct.src, BLE_curr_address, 8);
    processXferComplete(&reportStruct, true, BLE_curr_dataVer);
    BLE_clear_attempts(BLE_curr_address);
}

// Give the item up. Reporting it complete, as before, made it the image the AP believes the device shows (hash,
// preview, update count), so the same content was never sent again. A display gets processXferTimeout(): its queue
// is cleared and its hash stays. The label printer only loses this label; its other queued labels stay.
static void BLE_giveUp() {
    char buffer[96];
    snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X%02X%02X%02X%02X BLE upload given up after %d tries",
             BLE_curr_address[7], BLE_curr_address[6], BLE_curr_address[5], BLE_curr_address[4], BLE_curr_address[3],
             BLE_curr_address[2], BLE_curr_address[1], BLE_curr_address[0], BLE_MAX_TRIES);
    Serial.println(buffer);
    wsErr(buffer);
    BLE_clear_attempts(BLE_curr_address);
    const tagRecord* taginfo = tagRecord::findByMAC(BLE_curr_address);
    if (taginfo != nullptr && taginfo->hwType == NEMONIC_BLE_MIP201) {
        dropQueueItem(BLE_curr_address, BLE_curr_dataVer);
        return;
    }
    struct espXferComplete reportStruct;
    memcpy((uint8_t*)&reportStruct.src, BLE_curr_address, 8);
    processXferTimeout(&reportStruct, true);
}

// A try failed: no connection, a refused part, no notify for 30 s, a dropped link. BLE_is_image_pending() counted
// it and waits before the next one; after BLE_MAX_TRIES the item is given up.
static void BLE_attemptFailed(const char* why) {
    BLE_freeImageBuffer();
    if (pClient != nullptr && pClient->isConnected()) pClient->disconnect();
    ble_main_state = BLE_MAIN_STATE_IDLE;
    BLE_last_pending_check = millis();
    BLE_curr_part = 0;
    const uint32_t tries = BLE_get_attempts(BLE_curr_address);
    Serial.printf("BLE upload failed (%s), try %u of %u\r\n", why, (unsigned)tries, (unsigned)BLE_MAX_TRIES);
    if (tries >= BLE_MAX_TRIES) BLE_giveUp();
}

// The item can't be delivered at all (e.g. a label raster of the wrong size): drop just that item.
static void BLE_dropItem(const char* why) {
    char buffer[112];
    snprintf(buffer, sizeof(buffer), "%02X%02X%02X%02X%02X%02X%02X%02X BLE item dropped: %s",
             BLE_curr_address[7], BLE_curr_address[6], BLE_curr_address[5], BLE_curr_address[4], BLE_curr_address[3],
             BLE_curr_address[2], BLE_curr_address[1], BLE_curr_address[0], why);
    Serial.println(buffer);
    wsErr(buffer);
    dropQueueItem(BLE_curr_address, BLE_curr_dataVer);
    BLE_clear_attempts(BLE_curr_address);
    BLE_last_pending_check = millis();
}

void BLETask(void* parameter) {
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    Serial.println("BLE task started");
    NimBLEDevice::init("");
    NimBLEDevice::setMTU(255);  // preferred ATT MTU, exchanged on every connect
    while (1) {
        // NimBLE completes every GATT operation: the host fails pending ones when a link drops and times out
        // unanswered ones after 30 s (the Bluedroid library could wait forever here). The heartbeat stays as a
        // backstop: if this task still stops, the supervisor restarts the AP.
        supervisorHeartbeat(SUPERVISED_BLE);
        switch (ble_main_state) {
            default:
            case BLE_MAIN_STATE_IDLE:
                // An upload that ended through the disconnect callback (which only flips the state back to IDLE)
                // still owns its 135 KB image buffer: the link dropped in the middle of it. That is a failed try
                // (BLE_attemptFailed() frees the buffer); no upload is running in IDLE.
                if (BLE_image_buffer != nullptr) BLE_attemptFailed("link dropped during the upload");
                // First scan right away: uploads need the address types it records (see BLE_tagAddress()).
                if (last_ble_scan == 0 || millis() - last_ble_scan > (INTERVAL_BLE_SCANNING_SECONDS * 1000)) {
                    last_ble_scan = millis();
                    Serial.println("Doing the BLE Scan");
                    BLE_startScan(10);  // timeout in seconds, this is blocking but only for this thread!
                }
                if (millis() - BLE_last_pending_check >= (INTERVAL_HANDLE_PENDING_SECONDS * 1000)) {
                    if (BLE_is_image_pending(BLE_curr_address)) {
                        Serial.println("BLE Image is pending but we wait a bit");
                        delay(5000);                                                       // We better wait here, since the pending image needs to be created first
                        {
                            PendingItem* curItem = getQueueItem(BLE_curr_address);
                            BLE_curr_dataVer = (curItem != nullptr) ? curItem->pendingdata.availdatainfo.dataVer : 0;
                        }
                        tagRecord* curTag = tagRecord::findByMAC(BLE_curr_address);
                        uint8_t curHwType = (curTag != nullptr) ? curTag->hwType : 0;
                        if (curHwType != 0 && (curHwType & 0xF0) == 0xD0) {
                            // Wolink/Zhsunyco family. The whole upload runs synchronously inside this task, so we
                            // stay in IDLE and report the result directly when wolink_upload() returns.
                            if (wolink_upload(BLE_curr_address)) {
                                BLE_reportDelivered();
                            } else {
                                BLE_attemptFailed("Wolink upload failed");
                            }
                            BLE_last_pending_check = millis();
#ifdef HAS_NEMONIC_PRINTER
                        } else if (curHwType == NEMONIC_BLE_MIP201) {
                            // Nemonic MIP-201 BLE label printer. Same synchronous pattern as Wolink: connect, push
                            // the SDK split-frame sequence, trigger PRINT, disconnect.
                            switch (nemonic_upload(BLE_curr_address)) {
                                case NEMONIC_DELIVERED:
                                    BLE_reportDelivered();
                                    break;
                                case NEMONIC_DROP:
                                    BLE_dropItem("the label can't be printed");
                                    break;
                                default:
                                    BLE_attemptFailed("Nemonic print failed");
                                    break;
                            }
                            BLE_last_pending_check = millis();
#endif
                        } else if (BLE_curr_address[7] == 0x13 && BLE_curr_address[6] == 0x37) {  // This is an ATC BLE OEPL display
                            // Here we create the compressed buffer
                            BLE_image_buffer = (uint8_t*)malloc(BUFFER_MAX_SIZE_COMPRESSING);
                            if (BLE_image_buffer == nullptr) {
                                Serial.println("BLE Could not create buffer!");
                                BLE_compressed_len = 0;
                                BLE_attemptFailed("no memory for the image buffer");
                            } else {
                                uint8_t dataType = 0x00;
                                uint8_t dataTypeArgument = 0x00;
                                uint16_t nextCheckin = 0x00;
                                BLE_compressed_len = get_ATC_BLE_OEPL_image(BLE_curr_address, BLE_image_buffer, BUFFER_MAX_SIZE_COMPRESSING, &dataType, &dataTypeArgument, &nextCheckin);
                                Serial.printf("BLE data Length: %i\r\n", BLE_compressed_len);
                                // then we connect to BLE to send the compressed data
                                if (BLE_compressed_len && BLE_connect(BLE_curr_address, BLE_TYPE_ATC_BLE_OEPL)) {
                                    BLE_err_counter = 0;
                                    BLE_curr_part = 0;
                                    memset(BLE_notify_buffer, 0x00, sizeof(BLE_notify_buffer));

                                    uint8_t md5bytes[16];
                                    MD5Builder md5;
                                    md5.begin();
                                    md5.add(BLE_image_buffer, BLE_compressed_len);
                                    md5.calculate();
                                    md5.getBytes(md5bytes);

                                    BLEavaildatainfo.dataType = dataType;
                                    BLEavaildatainfo.dataVer = *((uint64_t*)md5bytes);
                                    BLEavaildatainfo.dataSize = BLE_compressed_len;
                                    BLEavaildatainfo.dataTypeArgument = dataTypeArgument;
                                    BLEavaildatainfo.nextCheckIn = nextCheckin;
                                    BLEavaildatainfo.checksum = 0;
                                    for (uint16_t c = 1; c < sizeof(struct AvailDataInfo); c++) {
                                        BLEavaildatainfo.checksum += (uint8_t)((uint8_t*)&BLEavaildatainfo)[c];
                                    }
                                    BLE_upload_state = BLE_UPLOAD_STATE_INIT;
                                    ble_main_state = BLE_MAIN_STATE_ATC_BLE_OEPL_UPLOAD;
                                    BLE_new_notify = true;  // trigger the upload here
                                } else {
                                    BLE_attemptFailed(BLE_compressed_len ? "connection failed" : "no image data");
                                }
                            }
                        } else {  // This is a Gicisky display
                            // Here we create the compressed buffer
                            BLE_image_buffer = (uint8_t*)malloc(BUFFER_MAX_SIZE_COMPRESSING);
                            if (BLE_image_buffer == nullptr) {
                                Serial.println("BLE Could not create buffer!");
                                BLE_compressed_len = 0;
                                BLE_attemptFailed("no memory for the image buffer");
                            } else {
                                BLE_compressed_len = compress_image(BLE_curr_address, BLE_image_buffer, BUFFER_MAX_SIZE_COMPRESSING);
                                Serial.printf("BLE Compressed Length: %i\r\n", BLE_compressed_len);
                                // then we connect to BLE to send the compressed data
                                if (BLE_compressed_len && BLE_connect(BLE_curr_address, BLE_TYPE_GICISKY)) {
                                    BLE_err_counter = 0;
                                    BLE_curr_part = 0;
                                    memset(BLE_notify_buffer, 0x00, sizeof(BLE_notify_buffer));
                                    BLE_upload_state = BLE_UPLOAD_STATE_INIT;
                                    ble_main_state = BLE_MAIN_STATE_UPLOAD;
                                    BLE_new_notify = true;  // trigger the upload here
                                } else {
                                    BLE_attemptFailed(BLE_compressed_len ? "connection failed" : "no image data");
                                }
                            }
                        }
                        BLE_last_pending_check = millis();
                    }
                }
                break;
            case BLE_MAIN_STATE_UPLOAD: {
                if (BLE_connected && BLE_new_notify) {
                    BLE_new_notify = false;
                    BLE_last_notify = millis();
                    BLE_upload_state = BLE_notify_buffer[1];

                    switch (BLE_upload_state) {
                        default:
                        case BLE_UPLOAD_STATE_INIT:
                            BLE_mini_buff[0] = 1;
                            ctrlChar->writeValue(BLE_mini_buff, 1, false);
                            break;
                        case BLE_UPLOAD_STATE_SIZE:
                            BLE_mini_buff[0] = 0x02;
                            BLE_mini_buff[1] = BLE_compressed_len & 0xff;
                            BLE_mini_buff[2] = (BLE_compressed_len >> 8) & 0xff;
                            BLE_mini_buff[3] = (BLE_compressed_len >> 16) & 0xff;
                            BLE_mini_buff[4] = (BLE_compressed_len >> 24) & 0xff;
                            BLE_mini_buff[5] = 0x00;
                            ctrlChar->writeValue(BLE_mini_buff, 6, false);
                            break;
                        case BLE_UPLOAD_STATE_START:
                            BLE_mini_buff[0] = 0x03;
                            ctrlChar->writeValue(BLE_mini_buff, 1, false);
                            break;
                        case BLE_UPLOAD_STATE_UPLOAD:
                            if (BLE_notify_buffer[2] == 0x08) {
                                // the buffer goes first, so the disconnect callback doesn't count a dropped link
                                BLE_freeImageBuffer();
                                pClient->disconnect();
                                ble_main_state = BLE_MAIN_STATE_IDLE;
                                BLE_last_pending_check = millis();
                                // Done and the image is refreshing now
                                BLE_reportDelivered();
                                BLE_err_counter = 0;
                                BLE_curr_part = 0;
                            } else {
                                // little-endian part number in bytes 3..6 (the old code shifted bytes 4..6 all by 24,
                                // so every upload with more than 256 parts aborted here)
                                uint32_t req_curr_part = ((uint32_t)BLE_notify_buffer[6] << 24) | ((uint32_t)BLE_notify_buffer[5] << 16) | ((uint32_t)BLE_notify_buffer[4] << 8) | BLE_notify_buffer[3];
                                if (req_curr_part != BLE_curr_part) {
                                    Serial.printf("Something went wrong, expected req part: %i but got: %i we better abort here.\r\n", req_curr_part, BLE_curr_part);
                                    BLE_attemptFailed("unexpected part request");
                                    break;  // previously fell through and sent a part from the buffer it had just freed
                                }
                                uint32_t curr_len = 240;
                                if (BLE_compressed_len - (BLE_curr_part * 240) < 240)
                                    curr_len = BLE_compressed_len - (BLE_curr_part * 240);
                                BLE_mini_buff[0] = BLE_curr_part & 0xff;
                                BLE_mini_buff[1] = (BLE_curr_part >> 8) & 0xff;
                                BLE_mini_buff[2] = (BLE_curr_part >> 16) & 0xff;
                                BLE_mini_buff[3] = (BLE_curr_part >> 24) & 0xff;
                                memcpy((uint8_t*)&BLE_mini_buff[4], (uint8_t*)&BLE_image_buffer[BLE_curr_part * 240], curr_len);
                                imgChar->writeValue(BLE_mini_buff, curr_len + 4, false);
                                Serial.printf("BLE sending part: %i\r\n", BLE_curr_part);
                                BLE_curr_part++;
                            }
                            break;
                    }
                } else {
                    if (millis() - BLE_last_notify > 30000) {  // Something odd, better reset connection!
                        Serial.println("BLE err going back to IDLE");
                        BLE_err_counter = 0;
                        BLE_attemptFailed("no notify for 30 s");
                    }
                }
                break;
            }
            case BLE_MAIN_STATE_ATC_BLE_OEPL_UPLOAD: {
                if (BLE_connected && BLE_new_notify) {
                    BLE_new_notify = false;
                    BLE_last_notify = millis();
                    switch (BLE_upload_state) {
                        default:
                        case BLE_UPLOAD_STATE_INIT:
                            BLE_mini_buff[0] = 0x00;
                            BLE_mini_buff[1] = 0x64;
                            memcpy((uint8_t*)&BLE_mini_buff[2], &BLEavaildatainfo, sizeof(struct AvailDataInfo));
                            ctrlChar->writeValue(BLE_mini_buff, sizeof(struct AvailDataInfo) + 2, false);
                            BLE_upload_state = BLE_UPLOAD_STATE_UPLOAD;
                            break;
                        case BLE_UPLOAD_STATE_UPLOAD: {
                            uint8_t notifyLen = BLE_notify_buffer[0];
                            uint16_t notifyCMD = (BLE_notify_buffer[1] << 8) | BLE_notify_buffer[2];
                            Serial.println("BLE CMD " + String(notifyCMD));
                            switch (notifyCMD) {
                                case BLE_CMD_REQ:
                                    if (notifyLen == (sizeof(struct blockRequest) + 2)) {
                                        Serial.println("We got a request for a BLK");
                                        memcpy(&BLEblkRequst, &BLE_notify_buffer[3], sizeof(struct blockRequest));
                                        BLE_curr_part = 0;
                                        ATC_BLE_OEPL_PrepareBlk(BLEblkRequst.blockId);
                                        ATC_BLE_OEPL_SendPart(BLEblkRequst.blockId, BLE_curr_part);
                                    }
                                    break;
                                case BLE_CMD_ACK_BLKPRT:
                                    BLE_curr_part++;
                                    BLE_err_counter = 0;
                                case BLE_CMD_ERR_BLKPRT:
                                    if (BLE_curr_part <= BLE_max_block_parts && BLE_err_counter++ < 15) {
                                        ATC_BLE_OEPL_SendPart(BLEblkRequst.blockId, BLE_curr_part);
                                        break;
                                    }
                                    // too many errors on one block: the display didn't get the image (as upstream
                                    // 5632ac3; this used to fall through and report the upload as done)
                                    BLE_err_counter = 0;
                                    BLE_max_block_parts = 0;
                                    BLE_attemptFailed("too many block part errors");
                                    break;
                                case BLE_CMD_ACK:
                                case BLE_CMD_ACK_IS_SHOWN:
                                case BLE_CMD_ACK_FW_UPDATED:
                                    Serial.println("BLE Upload done");
                                    // the buffer goes first, so the disconnect callback doesn't count a dropped link
                                    BLE_freeImageBuffer();
                                    pClient->disconnect();
                                    ble_main_state = BLE_MAIN_STATE_IDLE;
                                    BLE_last_pending_check = millis();
                                    // Done and the image is refreshing now
                                    BLE_reportDelivered();
                                    BLE_err_counter = 0;
                                    BLE_max_block_parts = 0;
                                    BLE_curr_part = 0;
                                    break;
                            }
                        } break;
                    }
                } else {
                    if (millis() - BLE_last_notify > 30000) {  // Something odd, better reset connection!
                        Serial.println("BLE err going back to IDLE");
                        BLE_err_counter = 0;
                        BLE_attemptFailed("no notify for 30 s");
                    }
                }
                break;
            }
        }
        vTaskDelay(15 / portTICK_PERIOD_MS);
    }
}

#endif
