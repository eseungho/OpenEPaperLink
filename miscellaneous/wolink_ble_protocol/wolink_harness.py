#!/usr/bin/env python3
"""
Wolink/Zhsunyco 7.5" BWRY packing harness (offline, no hardware).

Models the tag as a configurable decoder and our packer as the matching encoder,
so a candidate packing can be previewed before flashing. Findings reproduced here:
  * 2bpp interleaved white (0x55) read as 1bpp -> fine stipple  (1st symptom)
  * column-major data read by a row-major panel -> vertical "comb teeth" tiling
    + a 90-deg look (2nd symptom, matches the real 7.5" photo)
  * ROW-MAJOR dual 1bpp planes -> clean image (the fix; ble_filter.cpp default)

Source-plane layout = makeimage.cpp output for a rotatebuffer=1 BWRY tag,
column-major: pixel(x,y) bit at off = x*(H/8) + y//8, mask 0x80>>(y%8);
p1 (B/W) = 1 for BLACK|YELLOW, p2 (colour) = 1 for RED|YELLOW.
"""
import sys
WHITE, BLACK, RED, YELLOW = (255,255,255),(0,0,0),(255,0,0),(255,255,0)
COMBO = {(0,0):WHITE,(1,0):BLACK,(0,1):RED,(1,1):YELLOW}

def classify(rgb):
    r,g,b = rgb
    if r>180 and g>180 and b>180: return WHITE
    if r<80 and g<80 and b<80:    return BLACK
    if r>150 and g<100 and b<100: return RED
    return YELLOW

def image_to_planes(img, W, H):
    bpl=H//8; bw=bytearray(W*bpl); cl=bytearray(W*bpl)
    for x in range(W):
        for y in range(H):
            c=classify(img[y][x]); off=x*bpl+(y>>3); m=0x80>>(y&7)
            if c in (BLACK,YELLOW): bw[off]|=m
            if c in (RED,YELLOW):   cl[off]|=m
    return bw,cl

def _pos(px, py, W, H, row_major, msb):
    if row_major:
        return py*(W//8)+(px>>3), (0x80>>(px&7)) if msb else (1<<(px&7))
    return px*(H//8)+(py>>3), (0x80>>(py&7)) if msb else (1<<(py&7))

def pack_dual(bw, cl, W, H, row_major, color_first=0, inv_bw=1, inv_color=0, xflip=0, yflip=0, msb=1):
    bpl=H//8; oplane=(W*H)//8; out=bytearray(2*oplane)
    A = oplane if color_first else 0; Bo = 0 if color_first else oplane
    for x in range(W):
        px=(W-1-x) if xflip else x
        for y in range(H):
            off=x*bpl+(y>>3); m=0x80>>(y&7)
            p1=1 if bw[off]&m else 0; p2=1 if cl[off]&m else 0
            b=(0 if p1 else 1) if inv_bw else p1
            c=(0 if p2 else 1) if inv_color else p2
            py=(H-1-y) if yflip else y
            ob,om=_pos(px,py,W,H,row_major,msb)
            if b: out[A+ob]|=om
            if c: out[Bo+ob]|=om
    return bytes(out)

def panel_decode(buf, W, H, row_major, color_first=0, inv_bw=1, inv_color=0, xflip=0, yflip=0, msb=1):
    oplane=(W*H)//8; A=oplane if color_first else 0; Bo=0 if color_first else oplane
    img=[[WHITE]*W for _ in range(H)]
    for x in range(W):
        px=(W-1-x) if xflip else x
        for y in range(H):
            py=(H-1-y) if yflip else y
            ob,om=_pos(px,py,W,H,row_major,msb)
            sb=1 if buf[A+ob]&om else 0; sc=1 if buf[Bo+ob]&om else 0
            p1=(0 if sb else 1) if inv_bw else sb
            p2=(0 if sc else 1) if inv_color else sc
            img[y][x]=COMBO[(p1,p2)]
    return img

def write_ppm(p,img,W,H):
    open(p,'wb').write(f"P6\n{W} {H}\n255\n".encode()+bytes(v for r in img for px in r for v in px))

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
    write_ppm("/tmp/wolink_original.ppm", img, W, H)
    # panel is ROW-MAJOR. Our OLD column-major emit on it -> comb-teeth (matches the photo):
    write_ppm("/tmp/wolink_colmajor_on_panel.ppm",
              panel_decode(pack_dual(bw,cl,W,H,row_major=0), W,H, row_major=1), W,H)
    # ROW-MAJOR emit on the same panel -> clean (the fix shipped in ble_filter.cpp):
    write_ppm("/tmp/wolink_rowmajor_fix.ppm",
              panel_decode(pack_dual(bw,cl,W,H,row_major=1), W,H, row_major=1), W,H)
    print(f"{W}x{H}: wrote original / colmajor_on_panel (comb teeth) / rowmajor_fix (clean) PPMs to /tmp")
    print("Round-trip note: flips/polarity/order cancel in this harness, so it confirms SCAN ORDER")
    print("(row vs column major); absolute flip/colour are settled on-device via the WOLINK75_* toggles.")
