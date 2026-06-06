# Wolink / Zhsunyco BLE ESL — protocol notes (from manufacturer app RE)

Reverse-engineered from the manufacturer Android app **"WoPda"** (pkg `d29lc2w.new_pda`),
a Flutter app. Static analysis with [blutter](https://github.com/worawit/blutter) against
`lib/arm64-v8a/libapp.so` (Flutter **3.9.2** / Dart **3.9.2**, snapshot `97ff04a7…`, not obfuscated).
This complements the original RE in `NickWaterton/Wolink` (`wolink_ble.py`) that the OEPL
`ble_filter.cpp` / `ble_writer.cpp` Wolink support is based on.

## TL;DR — what the app does and does NOT contain
- The app is a **thin BLE relay**. The label image is **rendered AND packed on the vendor cloud**
  (endpoints `/eslpng/`, `/template/query?type=epd`, `/mobile/ack/ble`). The app fetches it
  (`API::get` / `DioService`), **base64-decodes** it (`Base64Codec.decode`), and streams the bytes
  over BLE. **No pixel packing, dithering, colour mapping, or image compression exists anywhere in
  the decompiled app** (verified: no image-size constants, no `decodeImage`/`getBytes`/`ZLibCodec`
  in the BLE path).
- ⇒ The **pixel-packing / colour matrix ("노브 조합") is NOT extractable from the APK** — it lives on
  the server. The APK *is* authoritative for the **BLE transport** (UUIDs, auth, framing, CRC,
  firmware-version handling).

## Tech stack (from `asm/<pkg>/` dirs)
`flutter_blue_plus` (BLE) · `flutter_nfc_kit`/`ndef` (NFC path) · `encrypt`+`pointycastle` (AES) ·
`dio` (cloud API) · GetX (state). App code under `app/utils/ble.dart`, `app/utils/tools.dart`,
`app/modules/skyline/{ble,nfc,esl,esl_wifi,pad}/…`.

## GATT service & characteristics
Vendor UUIDs are ASCII strings stored little-endian, so the bytes read reversed as
`WOLINKBLEESL2020…2025`:

| UUID | Role (app) | Used by OEPL? |
|---|---|---|
| `30323032-4C53-4545-4C42-4B4E494C4F57` | **Service** (…ESL2020) | yes |
| `31323032-…` (2021) | **Data** (image chunks) | yes (`wolinkDataUUID`) |
| `32323032-…` (2022) | **Config** | **no** |
| `33323032-…` (2023) | **Auth** (challenge/response) | yes (`wolinkAuthUUID`) |
| `34323032-…` (2024) | **Status** (notify) | yes (`wolinkStatusUUID`) |
| `35323032-…` (2025) | **Battery / info** | **no** |
| `3e3d1158-5656-4217-b715-266f37eb5000` | second service (OTA/`UpgradeService`, `OTA_ERROR`) | no |

## Authentication — CONFIRMED, key matches OEPL
`tools.dart :: genBleSecret` builds the 16-byte key **inline** (as Dart smi immediates, value =
`imm >> 1`) then `Key()/IV()/AES()` (`encrypt` pkg), AES-128-**CBC**, zero IV:

```
key = 9B 60 9F 28 BC 49 E2 57 29 BD 7B 8D F2 2B 44 20   ← identical to OEPL WOLINK_AES_KEY
```
Flow: read 16-byte challenge from Auth char → AES-CBC encrypt → write 16-byte response.
**OEPL's auth is therefore correct** (this is also why scan/connect/transfer already work).

## Framing
`ble.dart :: transceive` is the single BLE I/O routine:
1. `discoverServices`, `mtuNow`.
2. Auth (above).
3. Read device **firmware-version** string and branch via `String.contains` on:
   `01-00-00-01 / -03 / -06 / -07 / -09 / -0C / -12 / -13` — **per-version handling = the real
   "model↔mode" matrix**. OEPL has **no** version branching.
4. Image bytes (base64-decoded) sent in MTU-sized pieces (`Uint8List.sublist` loop → 11×
   `BluetoothCharacteristic.write`).
5. A commit/refresh frame protected by **`tools.dart :: crc16Cal`** — dual 256-entry tables,
   init `0xFFFF` ⇒ **CRC-16/MODBUS** (poly 0x8005 reflected / 0xA001). **OEPL sends no CRC.**

Other RX helpers: `tools.dart :: decompress` (GZip/ZLib) + `aesDecode` — used for data the app
*receives* (tag/cloud), consistent with the tag's `DECOMPRESS_ERROR` code (tag decompresses what
it is sent).

## Error codes (`BleError.toString`)
`OK, EPD_INIT_ERROR, EPD_WRITE_ERROR, DECOMPRESS_ERROR, OTA_ERROR, UNLOCK ERROR, ACK_ERROR …`

## 7.5" BWRY corruption — diagnosis & fix
The 7.5" panel format was found on hardware by iterating against field photos; each stage is
reproduced offline by `wolink_harness.py` (this dir). The panel reads **2 bpp, ROW-major**
(00=B 01=W 10=Y 11=R). The diagnosis chain:

| pack attempt | how the 2bpp row-major panel renders it | photo |
|---|---|---|
| 2bpp **column**-major (original) | "comb teeth"/90° + white stipple (scan transposed) | yes |
| 1bpp **dual-plane** row-major | image readable but **2×2 tiled, colours wrong** (panel is 2bpp, not 1bpp) | yes |
| **2bpp row-major** | **correct** | — |

Why each symptom: column-major data read row-major transposes → combs; white `01` packed 2bpp = `0x55`
read as the wrong scan = stipple; sending two 1bpp planes to a 2bpp panel halves the horizontal rate
and maps the two planes to the top/bottom screen halves → four tiles with the B/W bits reinterpreted
as 2-bit colour codes.

**Fix (shipped):** `ble_filter.cpp :: pack_wolink_image()` packs the 7.5" as **2 bpp row-major**
(`WOLINK75_DUAL_PLANE=0, ROW_MAJOR=1, XFLIP=1`). Output stays 96000 B so `wolink_upload()` framing is
unchanged; the 2.13"/3.5" column-major path is untouched. Any residual single global transform
(mirror / upside-down / red↔yellow) is one `WOLINK75_*` toggle — see the in-code symptom guide.

Secondary (not implicated by the observed symptoms): the cloud may **zlib-compress** large panels
(`DECOMPRESS_ERROR`) and the commit frame carries **CRC-16/MODBUS** — last resort only.

## Definitive verification (no BLE sniffer needed)
Enable **Android Developer Options → "Bluetooth HCI snoop log"** on a phone running WoPda, push a
label to the real 7.5" tag, pull `btsnoop_hci.log`, open in Wireshark. This captures the **exact
packed bytes + framing (CRC, opcodes, compression)** directly — the one source that resolves the
server-side packing.
