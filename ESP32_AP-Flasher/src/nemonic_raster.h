#pragma once
#ifdef HAS_NEMONIC_PRINTER

#include <stdint.h>
#include <stddef.h>

// Hardware fixed values for the Nemonic MIP-201 80mm label printer.
#define NEMONIC_WIDTH_DOTS   576
#define NEMONIC_WIDTH_BYTES  (NEMONIC_WIDTH_DOTS / 8)   // 72
#define NEMONIC_MAX_HEIGHT   2000                       // SDK MAX_PRINT_HEIGHT; firmware-confirm in Phase 1
#define NEMONIC_SPLIT_MAX    65536                      // SDK split frame payload cap (bytes)
#define NEMONIC_FIRST_PREPEND 4                         // [wBytes_lo][wBytes_hi][h_lo][h_hi]
#define NEMONIC_FRAME_HEADER 13                         // 1D 76 40 42 + total + idx + len[3] + crc[4]
#define NEMONIC_BLE_CHUNK    244                        // MTU 247 - 3B ATT overhead

// IEEE 802.3 / zlib CRC32, reflected, poly 0xEDB88320, init/final XOR 0xFFFFFFFF.
uint32_t nemonicCrc32(const uint8_t* data, uint32_t len);

// Trim trailing all-white rows (all bytes 0x00 with bit=1=black convention).
// Returns the actual content height (>=1; if everything is white, returns 1).
uint16_t nemonicTrimTrailingWhite(const uint8_t* buf, uint16_t widthBytes, uint16_t originalHeight);

// Compute the number of split frames required for a payload of `payloadAllLen`
// bytes total. The first frame carries up to NEMONIC_SPLIT_MAX bytes including
// the 4-byte image header; subsequent frames each carry up to NEMONIC_SPLIT_MAX.
uint8_t nemonicSplitCount(uint32_t payloadAllLen);

// Build ONE split frame in-place into `out`. Returns total bytes written
// (header NEMONIC_FRAME_HEADER + payloadLen). Caller must size `out` to at
// least NEMONIC_FRAME_HEADER + payloadLen.
//
// The `payload` for the first frame (idx==0) must already include the
// 4-byte image header `[wBytes_lo][wBytes_hi][h_lo][h_hi]` followed by the
// raster slice. Subsequent frames carry raster bytes only.
//
// CRC32 is computed over the payload bytes only (scope to be confirmed by
// golden vector in Phase 0; switch via `crcIncludeLen` if needed).
uint32_t nemonicBuildSplitFrame(uint8_t* out,
                                uint8_t total,
                                uint8_t idx_zero_based,
                                const uint8_t* payload,
                                uint32_t payloadLen,
                                bool crcIncludeLen = false);

#endif
