#ifdef HAS_NEMONIC_PRINTER

#include <Arduino.h>
#include <ArduinoJson.h>

#include <NimBLEDevice.h>

#include "ble_writer.h"
#include "nemonic_printer.h"
#include "nemonic_raster.h"
#include "newproto.h"
#include "storage.h"
#include "supervisor.h"
#include "tag_db.h"

// GATT UUIDs from Nemonic SDK (Nemonic.js:40-48).
static NimBLEUUID nemonicServiceUUID("3b790000-923e-4f69-b794-74f07d000000");
static NimBLEUUID nemonicNotifyUUID ("3b790001-923e-4f69-b794-74f07d000000");
static NimBLEUUID nemonicWriteUUID  ("3b790002-923e-4f69-b794-74f07d000000");

// Shared with ble_writer.cpp.
extern bool BLE_connected;

// ACK state: notify callback drops bytes here for the main loop to consume.
static volatile bool nemonic_ack_ready = false;
static volatile uint8_t nemonic_ack_buf[16] = {0};
static volatile uint8_t nemonic_ack_len = 0;

static void nemonicNotifyCb(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
    size_t n = length < sizeof(nemonic_ack_buf) ? length : sizeof(nemonic_ack_buf);
    for (size_t i = 0; i < n; i++) nemonic_ack_buf[i] = data[i];
    nemonic_ack_len = (uint8_t)n;
    nemonic_ack_ready = true;
    Serial.printf("Nemonic notify [%u]: ", (unsigned)length);
    for (size_t i = 0; i < length; i++) Serial.printf("%02X", data[i]);
    Serial.println();
}

class NemonicClientCallback : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient*) override {
        Serial.println("Nemonic BLE onConnect");
        BLE_connected = true;
    }
    void onDisconnect(NimBLEClient*, int reason) override {
        Serial.printf("Nemonic BLE onDisconnect, reason %d\r\n", reason);
        BLE_connected = false;
    }
};

// One instance for all prints; this used to be `new`-ed (and leaked) per print.
static NemonicClientCallback nemonicClientCallbacks;

// Chunk size of the write-without-response stream: NEMONIC_BLE_CHUNK (MTU 247) unless the negotiated MTU is smaller.
static uint16_t nemonic_chunk = NEMONIC_BLE_CHUNK;

// Writes without response fail at once while NimBLE's transmit buffers are full. Wait and retry the same chunk then,
// instead of skipping it, which would corrupt the frame and cost an 8.5 s ACK timeout per attempt.
static bool nemonic_write_chunked(NimBLERemoteCharacteristic* c, const uint8_t* data, uint32_t len) {
    for (uint32_t off = 0; off < len; off += nemonic_chunk) {
        const uint32_t n = (len - off) < nemonic_chunk ? (len - off) : nemonic_chunk;
        uint16_t tries = 0;
        while (!c->writeValue(data + off, n, false)) {  // write without response
            if (!BLE_connected || ++tries > 100) {
                Serial.printf("Nemonic: write failed at %u/%u\r\n", (unsigned)off, (unsigned)len);
                return false;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        if (((off / nemonic_chunk) & 63) == 63) supervisorHeartbeat(SUPERVISED_BLE);  // frames are up to 64 KB
        if (off + n < len) vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    return true;
}

static bool nemonic_wait_ack(uint32_t timeoutMs) {
    uint32_t start = millis();
    while (!nemonic_ack_ready) {
        if (millis() - start > timeoutMs) {
            Serial.printf("Nemonic ACK timeout after %u ms\r\n", (unsigned)(millis() - start));
            return false;
        }
        if (!BLE_connected) {
            Serial.println("Nemonic disconnected while waiting for ACK");
            return false;
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    nemonic_ack_ready = false;
    return true;
}

// Read user-configurable print options from the tag's modeConfigJson, with
// sensible defaults. Keys: "copies" (int, 1..255), "cut" (0|1), "noprint"
// (0|1, debug mode that skips the final PRINT cmd).
struct NemonicOpts {
    uint8_t copies;
    bool cut;
    bool noprint;
};
static NemonicOpts nemonic_parse_opts(tagRecord* tag) {
    NemonicOpts o = { 1, true, false };
    if (tag == nullptr || tag->modeConfigJson.length() == 0) return o;
    JsonDocument doc;
    if (deserializeJson(doc, tag->modeConfigJson) != DeserializationError::Ok) return o;
    if (doc["copies"].is<int>())  o.copies  = (uint8_t)constrain(doc["copies"].as<int>(), 1, 255);
    if (doc["copies"].is<const char*>()) o.copies = (uint8_t)constrain(atoi(doc["copies"].as<const char*>()), 1, 255);
    if (doc["cut"].is<int>())     o.cut     = doc["cut"].as<int>() != 0;
    if (doc["cut"].is<const char*>())    o.cut     = atoi(doc["cut"].as<const char*>()) != 0;
    if (doc["noprint"].is<int>()) o.noprint = doc["noprint"].as<int>() != 0;
    return o;
}

bool nemonic_upload(uint8_t address[8]) {
    // 1) Fetch the rendered 1bpp raster from the pending queue.
    PendingItem* item = getQueueItem(address, 0);
    if (item == nullptr) {
        Serial.println("Nemonic: no queue item, canceling");
        prepareCancelPending(address);
        return false;
    }
    if (item->data == nullptr) {
        fs::File f = contentFS->open(item->filename);
        if (!f) {
            Serial.printf("Nemonic: missing file %s, canceling\r\n", item->filename);
            prepareCancelPending(address);
            return false;
        }
        item->data = getDataForFile(f);
        f.close();
    }
    if (item->data == nullptr || item->len == 0) {
        Serial.println("Nemonic: empty raster, canceling");
        prepareCancelPending(address);
        return false;
    }

    // 2) Derive height; trim trailing white rows; sanity check.
    if (item->len % NEMONIC_WIDTH_BYTES != 0) {
        Serial.printf("Nemonic: raster size %u not a multiple of %u (width %u dots)\r\n",
                      (unsigned)item->len, NEMONIC_WIDTH_BYTES, NEMONIC_WIDTH_DOTS);
        prepareCancelPending(address);
        return false;
    }
    uint16_t fullHeight = (uint16_t)(item->len / NEMONIC_WIDTH_BYTES);
    if (fullHeight == 0 || fullHeight > NEMONIC_MAX_HEIGHT) {
        Serial.printf("Nemonic: invalid height %u (max %u)\r\n", fullHeight, NEMONIC_MAX_HEIGHT);
        prepareCancelPending(address);
        return false;
    }
    uint16_t height = nemonicTrimTrailingWhite(item->data, NEMONIC_WIDTH_BYTES, fullHeight);
    uint32_t rasterBytes = (uint32_t)NEMONIC_WIDTH_BYTES * height;
    uint32_t totalPayload = NEMONIC_FIRST_PREPEND + rasterBytes;
    uint8_t  totalFrames  = nemonicSplitCount(totalPayload);
    Serial.printf("Nemonic: %ux%u (trimmed from %u), %u payload bytes, %u frames\r\n",
                  NEMONIC_WIDTH_DOTS, height, fullHeight,
                  (unsigned)totalPayload, totalFrames);

    // 3) Parse user options.
    tagRecord* tag = tagRecord::findByMAC(address);
    NemonicOpts opts = nemonic_parse_opts(tag);
    Serial.printf("Nemonic opts: copies=%u cut=%d noprint=%d\r\n",
                  opts.copies, (int)opts.cut, (int)opts.noprint);

    // 4) Allocate frame buffer (header + max split payload).
    uint8_t* frameBuf = (uint8_t*)heap_caps_malloc(NEMONIC_FRAME_HEADER + NEMONIC_SPLIT_MAX,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (frameBuf == nullptr) frameBuf = (uint8_t*)malloc(NEMONIC_FRAME_HEADER + NEMONIC_SPLIT_MAX);
    if (frameBuf == nullptr) {
        Serial.println("Nemonic: frameBuf alloc failed");
        prepareCancelPending(address);
        return false;
    }

    // 5) BLE connect. tag_db stores the address LSB-first; BLE_tagAddress() converts it, this is just for the log.
    uint8_t reversed[6] = { address[5], address[4], address[3], address[2], address[1], address[0] };
    Serial.printf("Nemonic connecting to %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                  reversed[0], reversed[1], reversed[2], reversed[3], reversed[4], reversed[5]);

    NimBLEClient* client = BLE_getClient(&nemonicClientCallbacks);
    if (client == nullptr || !client->connect(BLE_tagAddress(address), true)) {
        Serial.println("Nemonic: connect() returned false");
        free(frameBuf);
        return false;
    }
    uint32_t t0 = millis();
    while (!BLE_connected && millis() - t0 < 5000) vTaskDelay(50 / portTICK_PERIOD_MS);
    if (!BLE_connected) {
        Serial.println("Nemonic: connection did not settle");
        client->disconnect();
        free(frameBuf);
        return false;
    }
    // NimBLE negotiates up to 255 on connect. Chunks longer than MTU-3 would become long writes, which NimBLE
    // silently truncates if the printer rejects them, so size them from the MTU.
    const uint16_t mtu = BLE_waitForMtu(client, 3000);
    if (mtu < 100) {
        Serial.printf("Nemonic: MTU %u too small\r\n", mtu);
        client->disconnect();
        free(frameBuf);
        return false;
    }
    nemonic_chunk = (mtu - 3) < NEMONIC_BLE_CHUNK ? (mtu - 3) : NEMONIC_BLE_CHUNK;

    NimBLERemoteService* svc = client->getService(nemonicServiceUUID);
    if (svc == nullptr) {
        Serial.println("Nemonic: service not found");
        client->disconnect();
        free(frameBuf);
        return false;
    }
    NimBLERemoteCharacteristic* writeChar  = svc->getCharacteristic(nemonicWriteUUID);
    NimBLERemoteCharacteristic* notifyChar = svc->getCharacteristic(nemonicNotifyUUID);
    if (writeChar == nullptr || notifyChar == nullptr) {
        Serial.println("Nemonic: characteristic discovery failed");
        client->disconnect();
        free(frameBuf);
        return false;
    }
    nemonic_ack_ready = false;
    if (!notifyChar->canNotify()) {
        Serial.println("Nemonic: notify characteristic does not support notify");
        client->disconnect();
        free(frameBuf);
        return false;
    }
    if (!notifyChar->subscribe(true, nemonicNotifyCb)) {
        // registerForNotify() ignored a failed CCCD write too; notifications may still arrive.
        Serial.println("Nemonic: notify subscription failed (continuing)");
    }

    auto fail = [&](const char* why) {
        Serial.printf("Nemonic FAIL: %s\r\n", why);
        client->disconnect();
        free(frameBuf);
        return false;
    };

    // 6) INIT
    {
        uint8_t init[2] = { 0x1B, 0x40 };
        if (!nemonic_write_chunked(writeChar, init, sizeof(init))) return fail("INIT write failed");
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    // 7) SET_COPIES
    {
        uint8_t setc[3] = { 0x1B, 0x43, opts.copies };
        if (!nemonic_write_chunked(writeChar, setc, sizeof(setc))) return fail("SET_COPIES write failed");
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    // 8) Split frames. Stream raster directly from item->data; build each
    //    frame in frameBuf and ship it.
    uint32_t cursor = 0;
    for (uint8_t idx = 0; idx < totalFrames; idx++) {
        uint32_t remaining = totalPayload - cursor;
        uint32_t thisLen   = remaining < NEMONIC_SPLIT_MAX ? remaining : NEMONIC_SPLIT_MAX;
        uint8_t* payload = frameBuf + NEMONIC_FRAME_HEADER;

        if (idx == 0) {
            payload[0] = (uint8_t)(NEMONIC_WIDTH_BYTES & 0xFF);
            payload[1] = (uint8_t)((NEMONIC_WIDTH_BYTES >> 8) & 0xFF);
            payload[2] = (uint8_t)(height & 0xFF);
            payload[3] = (uint8_t)((height >> 8) & 0xFF);
            uint32_t rasterChunk = thisLen - NEMONIC_FIRST_PREPEND;
            memcpy(payload + NEMONIC_FIRST_PREPEND, item->data, rasterChunk);
        } else {
            uint32_t rasterOffset = cursor - NEMONIC_FIRST_PREPEND;
            memcpy(payload, item->data + rasterOffset, thisLen);
        }
        cursor += thisLen;

        uint32_t frameLen = nemonicBuildSplitFrame(frameBuf, totalFrames, idx, payload, thisLen, false);
        Serial.printf("Nemonic frame %u/%u: payload %u, total %u\r\n",
                      (unsigned)(idx + 1), (unsigned)totalFrames,
                      (unsigned)thisLen, (unsigned)frameLen);

        nemonic_ack_ready = false;
        if (!nemonic_write_chunked(writeChar, frameBuf, frameLen)) return fail("frame write failed");
        supervisorHeartbeat(SUPERVISED_BLE);  // long labels are many frames, each waiting up to 8.5 s for an ACK

        if (!nemonic_wait_ack(8500)) {
            return fail("ACK timeout for split frame");
        }
        if (nemonic_ack_len < 6 || nemonic_ack_buf[5] != 0x30) {
            Serial.printf("Nemonic bad ACK byte[5]=0x%02X (len %u)\r\n",
                          nemonic_ack_buf[5], nemonic_ack_len);
            return fail("ACK status != 0x30");
        }
    }

    // 9) PRINT (cut or no-cut), or skip if debug noprint set.
    if (!opts.noprint) {
        uint8_t printCmd[2] = { 0x1B, (uint8_t)(opts.cut ? 0x50 : 0x51) };
        if (!nemonic_write_chunked(writeChar, printCmd, sizeof(printCmd))) return fail("PRINT write failed");
        vTaskDelay(50 / portTICK_PERIOD_MS);
    } else {
        Serial.println("Nemonic: noprint=1, skipping final PRINT");
    }

    // 10) Done.
    client->disconnect();
    free(frameBuf);
    return true;
}

#endif
