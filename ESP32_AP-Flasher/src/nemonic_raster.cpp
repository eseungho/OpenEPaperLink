#ifdef HAS_NEMONIC_PRINTER

#include "nemonic_raster.h"
#include <string.h>

uint32_t nemonicCrc32(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            uint32_t mask = -(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint16_t nemonicTrimTrailingWhite(const uint8_t* buf, uint16_t widthBytes, uint16_t originalHeight) {
    if (originalHeight == 0) return 1;
    for (int32_t row = (int32_t)originalHeight - 1; row >= 0; row--) {
        const uint8_t* p = buf + (uint32_t)row * widthBytes;
        for (uint16_t b = 0; b < widthBytes; b++) {
            if (p[b] != 0x00) {
                return (uint16_t)(row + 1);
            }
        }
    }
    return 1;
}

uint8_t nemonicSplitCount(uint32_t payloadAllLen) {
    if (payloadAllLen == 0) return 1;
    uint32_t n = (payloadAllLen + NEMONIC_SPLIT_MAX - 1) / NEMONIC_SPLIT_MAX;
    if (n > 255) n = 255;
    return (uint8_t)n;
}

uint32_t nemonicBuildSplitFrame(uint8_t* out,
                                uint8_t total,
                                uint8_t idx_zero_based,
                                const uint8_t* payload,
                                uint32_t payloadLen,
                                bool crcIncludeLen) {
    out[0] = 0x1D;
    out[1] = 0x76;
    out[2] = 0x40;
    out[3] = 0x42;
    out[4] = total;
    out[5] = (uint8_t)(idx_zero_based + 1);
    out[6] = (uint8_t)(payloadLen & 0xFF);
    out[7] = (uint8_t)((payloadLen >> 8) & 0xFF);
    out[8] = (uint8_t)((payloadLen >> 16) & 0xFF);

    uint32_t crc;
    if (crcIncludeLen) {
        uint8_t lenHdr[3] = { out[6], out[7], out[8] };
        uint32_t c = 0xFFFFFFFFu;
        for (int i = 0; i < 3; i++) {
            c ^= lenHdr[i];
            for (int j = 0; j < 8; j++) {
                uint32_t mask = -(int32_t)(c & 1u);
                c = (c >> 1) ^ (0xEDB88320u & mask);
            }
        }
        for (uint32_t i = 0; i < payloadLen; i++) {
            c ^= payload[i];
            for (int j = 0; j < 8; j++) {
                uint32_t mask = -(int32_t)(c & 1u);
                c = (c >> 1) ^ (0xEDB88320u & mask);
            }
        }
        crc = ~c;
    } else {
        crc = nemonicCrc32(payload, payloadLen);
    }
    out[9]  = (uint8_t)(crc & 0xFF);
    out[10] = (uint8_t)((crc >> 8) & 0xFF);
    out[11] = (uint8_t)((crc >> 16) & 0xFF);
    out[12] = (uint8_t)((crc >> 24) & 0xFF);

    memcpy(out + NEMONIC_FRAME_HEADER, payload, payloadLen);
    return NEMONIC_FRAME_HEADER + payloadLen;
}

#endif
