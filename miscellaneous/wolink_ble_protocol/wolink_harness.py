#!/usr/bin/env python3
"""
Wolink/Zhsunyco 7.5" BWRY packing harness (offline, no hardware).

Reproduces, from a synthetic image, each stage of the on-hardware diagnosis and
the final fix. The panel was determined to be **2bpp, row-major** (00=B 01=W
10=Y 11=R). Run: python3 wolink_harness.py [W H]  (default 800 480). Outputs PPMs
to /tmp; convert with PIL/ImageMagick to view.

Diagnosis chain (each step matched a field photo):
  pack 2bpp column-major  -> read by 2bpp row-major panel = "comb teeth"/90deg + stipple
  pack 1bpp dual-plane row-> read by 2bpp row-major panel = 2x2 tiling + wrong colours
  pack 2bpp row-major      -> read by 2bpp row-major panel = correct   (shipped default)

Source planes = makeimage.cpp output for a rotatebuffer=1 BWRY tag, column-major:
pixel(x,y) bit at off = x*(H/8)+y//8, mask 0x80>>(y%8); p1(B/W)=1 for BLACK|YELLOW,
p2(colour)=1 for RED|YELLOW.
"""
import sys
WHITE,BLACK,RED,YELLOW=(255,255,255),(0,0,0),(255,0,0),(255,255,0)

def classify(rgb):
    r,g,b=rgb
    if r>180 and g>180 and b>180: return WHITE
    if r<80 and g<80 and b<80:    return BLACK
    if r>150 and g<100 and b<100: return RED
    return YELLOW

def planes(img,W,H):
    bpl=H//8; bw=bytearray(W*bpl); cl=bytearray(W*bpl)
    for x in range(W):
        for y in range(H):
            c=classify(img[y][x]); o=x*bpl+(y>>3); m=0x80>>(y&7)
            if c in (BLACK,YELLOW): bw[o]|=m
            if c in (RED,YELLOW):   cl[o]|=m
    return bw,cl

def pack_2bpp(bw,cl,W,H,row_major=1,xflip=1,yflip=0,msb=1,swap_ry=0):
    bpl=H//8; out=bytearray((W*H)//4)
    for x in range(W):
        px=(W-1-x) if xflip else x
        for y in range(H):
            o=x*bpl+(y>>3); m=0x80>>(y&7)
            p1=1 if bw[o]&m else 0; p2=1 if cl[o]&m else 0
            if swap_ry and p2: p1^=1
            color=((p2&1)<<1)|(0 if p1 else 1)
            py=(H-1-y) if yflip else y
            if row_major: ob=py*(W//4)+(px>>2); sh=(6-(px&3)*2) if msb else ((px&3)*2)
            else:         ob=px*(H//4)+(py>>2); sh=(6-(py&3)*2) if msb else ((py&3)*2)
            out[ob]|=color<<sh
    return bytes(out)

def pack_dual_row(bw,cl,W,H,inv_bw=1):
    bpl=H//8; op=(W*H)//8; out=bytearray(2*op)
    for x in range(W):
        for y in range(H):
            o=x*bpl+(y>>3); m=0x80>>(y&7)
            p1=1 if bw[o]&m else 0; p2=1 if cl[o]&m else 0
            b=(0 if p1 else 1) if inv_bw else p1
            ob=y*(W//8)+(x>>3); om=0x80>>(x&7)
            if b: out[ob]|=om
            if p2: out[op+ob]|=om
    return bytes(out)

def panel_2bpp_row(buf,W,H,xflip=1):
    """How the real panel (2bpp row-major) renders any buffer."""
    img=[[WHITE]*W for _ in range(H)]
    for X in range(W):
        px=(W-1-X) if xflip else X
        for Y in range(H):
            ob=Y*(W//4)+(px>>2); sh=6-(px&3)*2
            img[Y][X]={0:BLACK,1:WHITE,2:YELLOW,3:RED}[(buf[ob]>>sh)&3]
    return img

def wppm(p,img,W,H): open(p,'wb').write(f"P6\n{W} {H}\n255\n".encode()+bytes(v for r in img for px in r for v in px))

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
    img=test_image(W,H); bw,cl=planes(img,W,H)
    wppm("/tmp/wolink_original.ppm", img, W, H)
    # the bug: dual-1bpp output as the 2bpp panel renders it (= 2x2 tiling, bad colours):
    wppm("/tmp/wolink_bug_dual_on_2bpp.ppm", panel_2bpp_row(pack_dual_row(bw,cl,W,H), W,H), W,H)
    # the fix: 2bpp row-major as the same panel renders it (= clean):
    wppm("/tmp/wolink_fix_2bpp_row.ppm", panel_2bpp_row(pack_2bpp(bw,cl,W,H), W,H), W,H)
    print(f"{W}x{H}: wrote original / bug_dual_on_2bpp (2x2 tiles) / fix_2bpp_row (clean) to /tmp")
