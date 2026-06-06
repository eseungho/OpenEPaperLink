#!/usr/bin/env python3
"""
Wolink/Zhsunyco BWRY packing harness (offline, no hardware).

Two jobs:
  1. VALIDATE the diagnosis: pack a white area as the current 2bpp interleaved
     format, then show what it looks like if the panel reads it as a 1bpp plane
     -> reproduces the "white background = fine black/white stipple" symptom.
  2. PREVIEW the dual-1bpp-plane replacement (matches ble_filter.cpp toggles)
     so plane order / polarity / flips can be reasoned about before flashing.

Source-plane layout (makeimage.cpp output for a rotatebuffer=1 BWRY tag),
column-major, per pixel (x,y): off = x*(H/8) + y//8 ; mask = 0x80 >> (y%8)
  p1 (B/W plane)   = 1 for BLACK or YELLOW
  p2 (colour plane)= 1 for RED  or YELLOW
"""
import sys
WHITE, BLACK, RED, YELLOW = (255,255,255),(0,0,0),(255,0,0),(255,255,0)

def classify(rgb):
    r,g,b = rgb
    if r>180 and g>180 and b>180: return WHITE
    if r<80 and g<80 and b<80:    return BLACK
    if r>150 and g<100 and b<100: return RED
    return YELLOW

def image_to_planes(img, W, H):
    bpl = H//8; plane = W*bpl
    bw = bytearray(plane); cl = bytearray(plane)
    for x in range(W):
        for y in range(H):
            c = classify(img[y][x])
            off = x*bpl + (y>>3); mask = 0x80 >> (y&7)
            if c in (BLACK, YELLOW): bw[off] |= mask
            if c in (RED,   YELLOW): cl[off] |= mask
    return bw, cl

# ---- current OEPL 2bpp interleaved (map hi=p2, lo=~p1 -> B=00 W=01 Y=10 R=11) ----
def pack_2bpp(bw, cl, W, H, x_flip=True):
    bpl=H//8; out=bytearray(W*(H//4))
    for x in range(W):
        px=(W-1-x) if x_flip else x
        for y in range(H):
            off=x*bpl+(y>>3); m=0x80>>(y&7)
            p1=1 if bw[off]&m else 0; p2=1 if cl[off]&m else 0
            color=((p2&1)<<1)|(0 if p1 else 1)
            py=H-1-y; ob=px*(H//4)+(py>>2); sh=6-(py&3)*2
            out[ob]|=color<<sh
    return bytes(out)

# ---- NEW dual 1bpp planes (mirrors ble_filter.cpp WOLINK75_* defaults) ----
def pack_dual_plane(bw, cl, W, H, color_first=0, inv_bw=1, inv_color=0,
                    xflip=0, yflip=1, msb=1):
    bpl=H//8; obpl=H//8; oplane=W*obpl
    out=bytearray(2*oplane)
    A = oplane if color_first else 0        # bw plane offset
    Bo = 0 if color_first else oplane       # colour plane offset
    for x in range(W):
        px=(W-1-x) if xflip else x
        for y in range(H):
            off=x*bpl+(y>>3); m=0x80>>(y&7)
            p1=1 if bw[off]&m else 0; p2=1 if cl[off]&m else 0
            b=(0 if p1 else 1) if inv_bw else p1
            c=(0 if p2 else 1) if inv_color else p2
            py=H-1-y if yflip else y
            ob=px*obpl+(py>>3); om=(0x80>>(py&7)) if msb else (1<<(py&7))
            if b: out[A+ob]|=om
            if c: out[Bo+ob]|=om
    return bytes(out)

def decode_first_plane_as_1bpp(buf, W, H, x_flip=True):
    """Read the first 1bpp plane of a 2bpp column-scan buffer as B/W (1=white).
    Applied to the 2bpp buffer this reproduces the stipple symptom on white."""
    img=[[WHITE]*W for _ in range(H)]
    for x in range(W):
        px=(W-1-x) if x_flip else x
        for y in range(H):
            py=H-1-y; bit_index=px*H + py        # 1bpp: H bits per column
            byte=bit_index>>3; sh=7-(bit_index&7)
            img[y][x]= WHITE if (buf[byte]>>sh)&1 else BLACK
    return img

def write_ppm(path,img,W,H):
    with open(path,'wb') as f:
        f.write(f"P6\n{W} {H}\n255\n".encode())
        f.write(bytes(v for r in img for px in r for v in px))

def test_image(W,H):
    img=[[WHITE]*W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            if x<W*0.06 and y<H*0.5: img[y][x]=BLACK
            elif H*0.44<y<H*0.5 and x<W*0.3: img[y][x]=BLACK
            elif y<H*0.05: img[y][x]=RED
            elif x>W*0.8 and y>H*0.8: img[y][x]=YELLOW
    return img

if __name__=="__main__":
    W,H=(int(sys.argv[1]),int(sys.argv[2])) if len(sys.argv)>2 else (800,480)
    img=test_image(W,H); bw,cl=image_to_planes(img,W,H)
    buf2=pack_2bpp(bw,cl,W,H,x_flip=True)
    bufD=pack_dual_plane(bw,cl,W,H)
    write_ppm("/tmp/wolink_original.ppm",img,W,H)
    write_ppm("/tmp/wolink_2bpp_misread_1bpp.ppm",
              decode_first_plane_as_1bpp(buf2,W,H),W,H)   # should show STIPPLE on white
    stipple_rows = sum(1 for y in range(H)
                       if [img[y][x] for x in range(W)].count(WHITE) > W*0.7)
    print(f"{W}x{H}: 2bpp_len={len(buf2)} dualplane_len={len(bufD)} mostly-white rows={stipple_rows}")
    print("wrote /tmp/wolink_original.ppm and /tmp/wolink_2bpp_misread_1bpp.ppm")
    print("If 'misread_1bpp' shows fine stripes over the white area, that confirms")
    print("the panel reads 1bpp planes -> use the dual-plane packer in ble_filter.cpp.")
