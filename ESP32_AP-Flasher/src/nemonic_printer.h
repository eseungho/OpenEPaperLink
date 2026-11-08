#pragma once
#ifdef HAS_NEMONIC_PRINTER

#include <stdint.h>

enum NemonicResult {
    NEMONIC_DELIVERED,  // the PRINT command was handed to the BLE stack: never print this label again
    NEMONIC_RETRY,      // failed before PRINT (connection, ACK, memory): try again; INIT discards partial frames
    NEMONIC_DROP,       // the label can't be printed (bad raster): drop it, keep the other queued labels
};

// Connect to a Nemonic MIP-201 BLE thermal label printer at the given
// 8-byte MAC (last two bytes ignored — see Wolink convention), pull the
// queued 1bpp raster (version BLE_curr_dataVer) from the tag queue, transmit
// the SDK split-frame sequence, and trigger the cut/print command.
// Synchronous; runs to completion inside the BLE task.
NemonicResult nemonic_upload(uint8_t address[8]);

#endif
