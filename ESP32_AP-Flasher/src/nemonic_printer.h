#pragma once
#ifdef HAS_NEMONIC_PRINTER

#include <stdint.h>

// Connect to a Nemonic MIP-201 BLE thermal label printer at the given
// 8-byte MAC (last two bytes ignored — see Wolink convention), pull the
// pending 1bpp raster from the tag queue, transmit the SDK split-frame
// sequence, and trigger the cut/print command. Synchronous; runs to
// completion inside the BLE task. Returns true on success.
bool nemonic_upload(uint8_t address[8]);

#endif
