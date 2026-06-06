# Wolink / Zhsunyco BLE ESL — OpenEPaperLink support reference

End-to-end documentation of the Wolink/Zhsunyco Bluetooth-LE electronic shelf label support in this
OpenEPaperLink (OEPL) fork: architecture, data flow, device configuration, the BLE protocol, the
image packing, a file/function code map, and how it was reverse-engineered.

---

## 1. Overview — where BLE ESLs fit in OEPL

OEPL was built around a sub-GHz / 802.15.4 radio: an ESP32 **access point (AP)** drives a tag-radio
co-processor and talks to native OEPL tags. This fork adds **direct BLE ESL** support so the same
ESP32 AP can also drive third-party Bluetooth labels — **Gicisky** (`0xB*`), **Wolink/Zhsunyco**
(`0xD*`), **Nemonic** printer (`0xE6`) and **ATC_BLE_OEPL** — using the ESP32's built-in Bluetooth.

A BLE tag is **not** polled like a sub-GHz tag; instead the AP runs a dedicated FreeRTOS **BLE task**
that scans for advertisements and, when content is pending, connects out to the label and pushes the
image. Everything for Wolink lives in the AP firmware under `ESP32_AP-Flasher/`.

Wolink labels are **BWRY** (black / white / red / yellow, 2 bits per pixel). Three sizes are
supported: **2.13"** 250×128 (`0xD0`), **3.5"** 384×184 (`0xD1`), **7.5"** 800×480 (`0xD2`).

---

## 2. End-to-end data flow

```
 web UI / content system
      │  set content for a tag MAC
      ▼
 contentmanager.cpp ── jpg2buffer()/spr2color() (makeimage.cpp)
      │  render template → file on contentFS =
      │  [ B/W plane | colour plane ]  (each width*height/8 B, 1bpp, column-major)
      │  enqueue PendingItem(MAC, filename)
      ▼
 BLETask()  (ble_writer.cpp)         ← FreeRTOS task, runs continuously
   IDLE loop:
     • BLE_startScan() every INTERVAL_BLE_SCANNING_SECONDS
         └─ onResult → BLE_filter_add_device() (ble_filter.cpp)
              parse mfr data "AA BB …" → modelId/hwVersion/battery
              wolinkToOEPLtype() → hwType 0xD0/D1/D2 → processDataReq() (registers tag)
     • BLE_is_image_pending() every INTERVAL_HANDLE_PENDING_SECONDS
         └─ if (hwType & 0xF0)==0xD0 → wolink_upload(MAC)
                                          │
                                          ▼
 wolink_upload() (ble_writer.cpp):
   1 connect + setMTU(247)
   2 discover service 30323032-… ; get Data/Auth/Status characteristics
   3 AUTH: read 16-B challenge ← Auth char; AES-128-CBC(zero IV, key) → write 16-B response
   4 PACK: pack_wolink_image() reads the rendered dual-plane file → tag RAM buffer
   5 UPLOAD: Data char ← [00 A5][offset LE32] + ≤238 B  (repeat over the whole buffer)
   6 REFRESH: Data char ← [01 A5][total_len LE32]
   7 wait for Status notify (busy/idle, error) or disconnect
      │ success → processXferComplete() removes the pending item
      ▼
   label refreshes
```

---

## 3. Device detection & configuration

### Advertisement detection (`ble_filter.cpp :: BLE_filter_add_device`)
Wolink labels advertise manufacturer data starting with company id bytes **`AA BB`** (≥12 B). Layout:

| bytes | field | notes |
|---|---|---|
| 0..1 | company id | `AA BB` |
| 2..3 | flags | |
| 4..5 | model / PID | big-endian (`0x000E` = 2.13") |
| 6..7 | app version | big-endian |
| 8..9 | **hw version** | big-endian — selects the size |
| 10..11 | battery mV | big-endian |

`wolinkToOEPLtype(modelId, hwVersion)` maps hw version → OEPL hwType:

| hwVersion | size | hwType | tagtype |
|---|---|---|---|
| `0x0103` | 2.13" 250×128 | `WOLINK_BLE_EPD_213_BWRY` `0xD0` | `resources/tagtypes/D0.json` |
| `0x0201` | 3.5" 384×184 | `WOLINK_BLE_EPD_350_BWRY` `0xD1` | `D1.json` |
| `0x0203` | 7.5" 800×480 | `WOLINK_BLE_EPD_750_BWRY` `0xD2` | `D2.json` |
| other | unknown | `WOLINK_BLE_UNKNOWN` `0xDF` | `DF.json` |

### Tag type JSON (`resources/tagtypes/D2.json`)
Drives rendering/colour for the web UI and `makeimage`:
```json
{ "name": "Wolink/Zhsunyco BLE EPD BWRY 7.5\"", "width": 800, "height": 480,
  "rotatebuffer": 1, "bpp": 2,
  "colortable": { "white":[255,255,255], "black":[0,0,0], "red":[255,0,0], "yellow":[255,255,0] } }
```

### Adding a new Wolink model
1. `#define WOLINK_BLE_EPD_… 0xD?` in **`oepl-definitions.h`**.
2. Add the `(modelId,hwVersion)→hwType` case in **`wolinkToOEPLtype()`**.
3. Create **`resources/tagtypes/<HEX>.json`** (width/height/bpp/rotatebuffer/colortable).
4. Add a width/height case in **`pack_wolink_image()`**; pick the scan (small tags column-major,
   large panels row-major — see §5).

---

## 4. BLE protocol (GATT, auth, framing)

Reverse-engineered from the manufacturer **"WoPda"** Flutter app (`blutter` on `libapp.so`,
Flutter/Dart 3.9.2) plus the upstream `NickWaterton/Wolink` Python project. The app is a thin BLE
relay — **image rendering/packing happens on the vendor cloud** (`/eslpng/`, `/template/query`); the
app base64-decodes and streams it. So the **packing is not in the app**; it was found on hardware.

**GATT** — UUIDs are ASCII strings stored little-endian, so the bytes read reversed as
`WOLINKBLEESL2020…2025`:

| UUID | role | used by OEPL |
|---|---|---|
| `30323032-4C53-4545-4C42-4B4E494C4F57` | Service | yes |
| `31323032-…` | **Data** (image chunks + refresh) | yes |
| `32323032-…` | Config | no |
| `33323032-…` | **Auth** | yes |
| `34323032-…` | **Status** (notify) | yes |
| `35323032-…` | Battery/info | no |
| `3e3d1158-5656-4217-b715-266f37eb5000` | OTA service (`UpgradeService`) | no |

**Authentication** (`ble_writer.cpp :: wolink_aes_encrypt_challenge`) — AES-128-**CBC**, zero IV,
16-byte key, confirmed identical in the app's `genBleSecret` and in OEPL:
```
9B 60 9F 28 BC 49 E2 57 29 BD 7B 8D F2 2B 44 20
```
Read 16-B challenge from the Auth char → AES-CBC encrypt → write the first 16-B block back.

**Framing** (`wolink_upload`):
- image chunks → Data char: `[0x00, 0xA5] + offset(LE32) + payload` (≤238 B, MTU 247 − 9).
- refresh    → Data char: `[0x01, 0xA5] + total_len(LE32)`.
- status     ← Status char notify: 2 B `[busy/idle, error]` (0 = ok).

The manufacturer app additionally protects its commit frame with **CRC-16/MODBUS** (`tools.dart ::
crc16Cal`, dual 256-entry tables, init `0xFFFF`) and branches on firmware-version strings
(`01-00-00-xx`); OEPL's upload succeeds without these. (`DECOMPRESS_ERROR`/`OTA_ERROR` are tag-side
codes; the cloud may zlib-compress very large frames — not needed for the sizes here.)

---

## 5. Image rendering & packing

### Source: `makeimage.cpp`
`jpg2buffer()` renders the template into a sprite; `spr2color()` (Burkes/ordered dithering, palette
from the tagtype) writes a **dual 1-bit-plane** file: plane 1 = B/W, plane 2 = colour, each
`width*height/8` bytes, **column-major** (with `rotatebuffer:1` applied). Per source pixel:
`p1 (B/W)=1 for BLACK|YELLOW`, `p2 (colour)=1 for RED|YELLOW`.

### Pack: `ble_filter.cpp :: pack_wolink_image()`
Converts the dual-plane source into the tag's RAM layout. Output is always `width*height/4` bytes.

- **2.13" / 3.5"**: 2 bpp, **column-major**, both axes flipped, 4 px/byte MSB-first. *(unchanged,
  proven)*
- **7.5" 800×480**: 2 bpp, **row-major** (native panel scan), `XFLIP=1`. **This is the working
  format.** Colour code (both): `hi=p2, lo=~p1` ⇒ **`00=black 01=white 10=yellow 11=red`**.

The 7.5" format was found by iterating against field photos (each stage reproduced offline by
`wolink_harness.py`):

| pack attempt | result on the real (2bpp row-major) panel |
|---|---|
| 2 bpp **column**-major (first try) | "comb teeth" / 90° + white stipple (scan transposed) |
| 1 bpp **dual-plane** row-major | image readable but **2×2 tiled, colours wrong** (panel is 2bpp) |
| **2 bpp row-major** | **correct** |

Compile-time knobs at the top of `pack_wolink_image()` (apply to the **7.5" only**) let you correct a
single residual global transform without re-deriving anything:

| symptom | toggle |
|---|---|
| mirrored left/right | `WOLINK75_XFLIP` |
| upside down | `WOLINK75_YFLIP` |
| red ↔ yellow swapped | `WOLINK75_SWAP_RY` |
| bits within byte reversed | `WOLINK75_MSB_FIRST` |
| (diagnostic) two 1bpp planes instead of 2bpp | `WOLINK75_DUAL_PLANE` |
| (diagnostic) column- vs row-major | `WOLINK75_ROW_MAJOR` |

Defaults: `DUAL_PLANE=0, ROW_MAJOR=1, XFLIP=1, YFLIP=0, MSB_FIRST=1, SWAP_RY=0`. Each pack logs its
active config over serial: `Wolink 7.5" pack: 2BPP scan=row xflip=1 …`.

---

## 6. Code map

| file | responsibility |
|---|---|
| `ESP32_AP-Flasher/src/ble_filter.cpp` | advertisement scan & detection (`BLE_filter_add_device`), `wolinkToOEPLtype()`, **packing** `pack_wolink_image()` |
| `ESP32_AP-Flasher/src/ble_writer.cpp` | BLE task `BLETask()`, connect/MTU, **`wolink_upload()`** (auth/chunk/refresh), UUIDs, AES key |
| `ESP32_AP-Flasher/src/makeimage.cpp` | render template → dual 1bpp plane file (`jpg2buffer`, `spr2color`) |
| `oepl-definitions.h` | `WOLINK_BLE_EPD_*` hwType constants (`0xD0`/`D1`/`D2`/`DF`) |
| `resources/tagtypes/D0–D2,DF.json` | per-size specs (dimensions, bpp, rotatebuffer, colours) |
| `miscellaneous/wolink_ble_protocol/` | this reference + `wolink_harness.py` |

---

## 7. How it was reverse-engineered (reproducible)

The packing isn't in the app (server-side), so it was solved with **(a)** static RE of the BLE
transport and **(b)** an offline pixel-packing simulator validated against field photos.

- **Static RE:** `blutter` (Dart AOT decompiler) on `lib/arm64-v8a/libapp.so`. It recovered class /
  method names (un-obfuscated), the object pool, and disassembly — yielding the GATT roles, the AES
  key in `genBleSecret`, the CRC-16/MODBUS in `crc16Cal`, and the proof that packing is server-side.
- **Offline packing simulator:** `wolink_harness.py` models the panel as a decoder and our packer as
  the encoder. Feeding our output through a candidate panel model reproduces each field-photo symptom
  (stipple → comb teeth → 2×2 tiling) and confirms the fix renders cleanly — so candidate fixes are
  vetted before flashing. Run: `python3 wolink_harness.py 800 480` (writes PPMs to `/tmp`).

No Bluetooth sniffer or rooted phone is required by this method.
