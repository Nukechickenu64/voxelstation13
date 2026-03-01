"""
gen_textures.py — generate 32x32 PNG textures for VS13.
Item sprites are sourced from legacysets/extracted/obj/ where available.
Tile/UI sprites use the built-in stdlib pixel-art generator.
Requires Pillow (pip install Pillow) for loading legacy sprites.
"""
import os, struct, zlib
from PIL import Image

# ── Legacy sprite loader ───────────────────────────────────────────────────────

LEGACY_BASE = os.path.normpath(
    os.path.join(os.path.dirname(__file__), "..", "legacysets", "extracted", "obj")
)
LEGACY_TURF_BASE = os.path.normpath(
    os.path.join(os.path.dirname(__file__), "..", "legacysets", "extracted", "turf")
)

def from_legacy(rel_path: str):
    """Load a legacy sprite from legacysets/extracted/obj/, resize to 32x32,
    and return an RGBA pixel grid: list[list[tuple[r,g,b,a]]]."""
    full = os.path.join(LEGACY_BASE, rel_path.replace("/", os.sep))
    with Image.open(full) as im:
        im = im.convert("RGBA")
        if im.size != (32, 32):
            im = im.resize((32, 32), Image.LANCZOS)
        raw = im.tobytes()
    data = [tuple(raw[i:i+4]) for i in range(0, len(raw), 4)]
    return [data[y * 32:(y + 1) * 32] for y in range(32)]

def from_legacy_turf(rel_path: str):
    """Load a legacy sprite from legacysets/extracted/turf/, resize to 32x32,
    and return an RGBA pixel grid."""
    full = os.path.join(LEGACY_TURF_BASE, rel_path.replace("/", os.sep))
    with Image.open(full) as im:
        im = im.convert("RGBA")
        if im.size != (32, 32):
            im = im.resize((32, 32), Image.LANCZOS)
        raw = im.tobytes()
    data = [tuple(raw[i:i+4]) for i in range(0, len(raw), 4)]
    return [data[y * 32:(y + 1) * 32] for y in range(32)]

def _rgba_to_img(pixels):
    im = Image.new("RGBA", (32, 32))
    flat = [c for row in pixels for px in row for c in px]
    im.frombytes(bytes(flat))
    return im

def _img_to_rgba_grid(im):
    raw = im.tobytes()
    data = [tuple(raw[i:i+4]) for i in range(0, len(raw), 4)]
    return [data[y * 32:(y + 1) * 32] for y in range(32)]

def compose_bitmask(base_rgba, overlays):
    """Compose a 16-variant bitmask sprite set.
    overlays is a dict keyed by bit value {1,2,4,8} with RGBA grids.
    """
    base_im = _rgba_to_img(base_rgba)
    overlay_imgs = {k: _rgba_to_img(v) for k, v in overlays.items()}
    out = []
    for mask in range(16):
        im = base_im.copy()
        for bit in (1, 2, 4, 8):
            if mask & bit:
                im.alpha_composite(overlay_imgs[bit])
        out.append(_img_to_rgba_grid(im))
    return out

# ── Tiny PNG writer ────────────────────────────────────────────────────────────

def _chunk(tag: bytes, data: bytes) -> bytes:
    c = tag + data
    return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

def write_png(path: str, pixels: list[list[tuple[int,int,int]]]):
    """pixels[y][x] = (r, g, b)  all 0-255."""
    h, w = len(pixels), len(pixels[0])
    sig  = b"\x89PNG\r\n\x1a\n"
    ihdr = _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    raw  = b"".join(b"\x00" + bytes(v for px in row for v in px)
                    for row in pixels)
    idat = _chunk(b"IDAT", zlib.compress(raw, 9))
    iend = _chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(sig + ihdr + idat + iend)
    print(f"  wrote {path}")

def write_png_rgba(path: str, pixels: list[list[tuple[int,int,int,int]]]):
    """pixels[y][x] = (r, g, b, a)  all 0-255."""
    h, w = len(pixels), len(pixels[0])
    sig  = b"\x89PNG\r\n\x1a\n"
    ihdr = _chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    raw  = b"".join(b"\x00" + bytes(v for px in row for v in px)
                    for row in pixels)
    idat = _chunk(b"IDAT", zlib.compress(raw, 9))
    iend = _chunk(b"IEND", b"")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(sig + ihdr + idat + iend)
    print(f"  wrote {path}")

# ── Drawing helpers ────────────────────────────────────────────────────────────

def solid(w, h, r, g, b):
    return [[(r, g, b)] * w for _ in range(h)]

def transparent_rgba(w, h):
    return [[(0, 0, 0, 0)] * w for _ in range(h)]

def grid(w, h, r, g, b, gr=20, gr2=25, step=8):
    """Solid colour with a darker grid pattern."""
    px = solid(w, h, r, g, b)
    for y in range(h):
        for x in range(w):
            if x % step == 0 or y % step == 0:
                px[y][x] = (gr, gr, gr2)
    return px

def checker(w, h, c1, c2, size=8):
    return [[c1 if (x // size + y // size) % 2 == 0 else c2
             for x in range(w)] for y in range(h)]

def cross_hatch(w, h, r, g, b, lr, lg, lb, step=8):
    px = solid(w, h, r, g, b)
    for y in range(h):
        for x in range(w):
            if x % step < 2 or y % step < 2:
                px[y][x] = (lr, lg, lb)
    return px

def diamond_plate(w, h):
    """Steel-ish diamond plate pattern."""
    base = (100, 100, 110)
    hi   = (160, 160, 175)
    lo   = (60,  60,  70)
    px   = solid(w, h, *base)
    for y in range(h):
        for x in range(w):
            lx, ly = x % 8, y % 8
            if ly == 0 and lx < 4:
                px[y][x] = hi
            elif ly == 4 and lx >= 4:
                px[y][x] = hi
            elif ly == 7:
                px[y][x] = lo
    return px

def window_tex(w, h):
    px = solid(w, h, 140, 200, 230)
    for i in range(w):
        px[0][i] = px[h-1][i] = (80, 130, 160)
        px[i][0] = px[i][w-1] = (80, 130, 160)
    # cross dividers
    mid = w // 2
    for y in range(h):
        px[y][mid] = (80, 130, 160)
    for x in range(w):
        px[mid][x] = (80, 130, 160)
    # highlight sheen
    for i in range(1, 6):
        if i < w and i < h:
            px[i][i] = (200, 230, 255)
    return px

def catwalk_tex(w, h):
    """Orange-ish catwalk grating."""
    px = solid(w, h, 170, 100, 50)
    for y in range(h):
        for x in range(w):
            if x % 4 < 2 and y % 4 < 2:
                px[y][x] = (20, 20, 20)
    return px

def rwall_tex(w, h):
    px = solid(w, h, 55, 55, 65)
    # rivet pattern
    for ry in range(1, h, 8):
        for rx in range(1, w, 8):
            for dy in range(-1, 2):
                for dx in range(-1, 2):
                    ny, nx = ry + dy, rx + dx
                    if 0 <= ny < h and 0 <= nx < w:
                        px[ny][nx] = (90, 90, 100)
    return px

def light_tube_tex(w, h):
    px = solid(w, h, 240, 240, 200)
    for y in range(h):
        for x in range(w):
            d = abs(x - w // 2)
            glow = max(0, 255 - d * 20)
            px[y][x] = (min(255, glow + 200), min(255, glow + 200), min(255, glow + 100))
    return px

def fallback_magenta(w, h):
    return checker(w, h, (255, 0, 255), (0, 0, 0), 4)

def ui_slot(w, h, r=40, g=40, b=50, border=70):
    px = solid(w, h, r, g, b)
    for i in range(w):
        px[0][i] = px[h-1][i] = (border, border, border)
        px[i][0] = px[i][w-1] = (border, border, border)
    return px

def ui_hotbar_bg(w, h):
    px = solid(w, h, 20, 20, 25)
    for i in range(w):
        px[0][i] = (80, 80, 100)
        px[h-1][i] = (80, 80, 100)
    return px

# ── Item sprite helpers ────────────────────────────────────────────────────────

def _bg():
    return solid(W, H, 22, 22, 28)

def _s(px, x, y, c):
    if 0 <= y < H and 0 <= x < W:
        px[y][x] = c

def _r(px, x, y, w, h, c):
    for dy in range(h):
        for dx in range(w):
            _s(px, x+dx, y+dy, c)

def _l(px, x0, y0, x1, y1, c, t=1):
    dx=abs(x1-x0); dy=abs(y1-y0)
    sx=1 if x0<x1 else -1; sy=1 if y0<y1 else -1
    err=dx-dy; x,y=x0,y0
    while True:
        for tx in range(-(t//2), t//2+1):
            for ty in range(-(t//2), t//2+1):
                _s(px,x+tx,y+ty,c)
        if x==x1 and y==y1: break
        e2=2*err
        if e2>-dy: err-=dy; x+=sx
        if e2<dx:  err+=dx; y+=sy

def _c(px, cx, cy, r, c, filled=True):
    for dy in range(-r, r+1):
        for dx in range(-r, r+1):
            inside = dx*dx+dy*dy <= r*r
            if (inside if filled else abs(dx*dx+dy*dy - r*r) <= r+1):
                _s(px, cx+dx, cy+dy, c)

# ── Item sprite functions ──────────────────────────────────────────────────────

def px_wrench():
    px=_bg(); c=(175,170,180); s=(110,105,115)
    _r(px,3,4,2,10,c); _r(px,4,4,5,2,c); _r(px,4,12,5,2,c)  # C-jaw
    _r(px,7,7,2,4,c)                                           # jaw inner
    _l(px,9,9,25,25,c,3)                                       # handle
    _r(px,23,24,7,5,c); _r(px,24,25,5,3,s)                    # box end
    return px

def px_screwdriver():
    px=_bg(); h=(190,110,45); s=(205,195,185); tip=(165,163,170)
    _r(px,4,17,10,11,h); _r(px,5,18,8,9,(225,138,65))         # handle
    _l(px,13,20,26,7,s,2)                                       # shaft
    _s(px,27,6,tip); _s(px,28,5,tip)                            # tip
    return px

def px_crowbar():
    px=_bg(); c=(185,58,58); hi=(225,95,95)
    _l(px,6,26,23,9,c,3)                                        # main bar
    _r(px,21,5,8,3,c); _r(px,21,7,4,4,c); _r(px,21,5,8,1,hi)  # top hook
    _r(px,3,24,6,4,c); _r(px,3,24,5,1,hi)                      # bottom hook
    return px

def px_wirecutters():
    px=_bg(); m=(92,92,98); r=(195,52,52)
    _r(px,2,18,5,12,r); _r(px,8,20,5,10,r)                     # handles
    _c(px,12,17,4,m)                                             # pivot
    _l(px,12,14,23,5,m,3); _l(px,13,16,25,8,(135,135,145),2)   # blades
    return px

def px_toolbox():
    px=_bg(); c=(68,108,168); hi=(100,150,210); s=(42,72,125); la=(225,205,55)
    _r(px,3,13,26,15,c); _r(px,4,14,24,13,hi); _r(px,3,13,26,2,s)
    _r(px,11,8,10,7,c); _r(px,9,9,2,5,c); _r(px,21,9,2,5,c)   # handle
    _r(px,11,8,10,2,hi); _r(px,11,13,10,2,s)
    _r(px,13,19,6,5,la)                                          # latch
    return px

def px_stun_baton():
    px=_bg(); g=(58,58,63); e=(205,182,52); hi=(242,222,82)
    _r(px,5,19,8,11,g); _r(px,6,20,6,9,(72,72,77))             # grip
    _r(px,7,10,4,11,e); _r(px,8,11,2,9,hi)                      # shaft
    _r(px,5,5,10,7,e); _r(px,6,6,8,5,hi)                        # tip
    return px

def px_id_card():
    px=_bg(); b=(238,242,247); st=(42,122,205); tx=(92,92,98)
    _r(px,3,7,26,18,b); _r(px,3,7,26,4,st)                      # body + stripe
    _r(px,7,14,8,5,(182,187,192))                                 # photo area
    for y in [20,23]: _r(px,17,y,10,1,tx)                       # text lines
    return px

def px_fire_extinguisher():
    px=_bg(); c=(208,42,42); hi=(242,85,85); s=(142,22,22); top=(205,198,202)
    _r(px,9,9,14,19,c); _r(px,10,10,12,17,hi); _r(px,11,10,1,17,(228,105,105))
    _r(px,9,27,14,3,s)                                            # base
    _r(px,11,4,10,6,top); _r(px,14,2,4,3,top)                    # valve & nozzle
    _l(px,16,2,23,5,top,2)                                        # hose
    _r(px,9,17,14,4,(242,242,242)); _r(px,10,18,12,2,(205,42,42))# label
    return px

def px_flashlight():
    px=_bg(); c=(222,212,92); s=(162,152,52); lens=(165,205,242); rim=(122,122,132)
    _r(px,11,17,10,11,c); _r(px,12,18,8,9,s)                    # grip
    _c(px,16,11,7,rim); _c(px,16,11,5,lens); _c(px,16,11,2,(205,232,255))
    return px

def px_medipen():
    px=_bg(); c=(82,188,222); hi=(132,212,242); g=(62,62,72)
    _l(px,15,4,15,27,c,4); _l(px,15,4,15,27,hi,1)
    _r(px,12,21,8,6,g)                                            # grip band
    for i in range(3): _r(px,13,22+i*2,1,1,(102,102,112))
    _s(px,15,3,(212,212,218)); _s(px,16,3,(212,212,218))          # needle
    _c(px,15,28,3,(102,102,112))                                   # base cap
    return px

def px_pill_bottle():
    px=_bg(); cc=(222,148,58); body=(238,238,238)
    _r(px,9,4,14,7,cc); _r(px,10,5,12,5,(202,122,42))            # cap
    _r(px,7,11,18,17,body); _r(px,8,12,16,15,(248,248,248))      # body
    _r(px,8,16,16,6,(212,62,62)); _r(px,9,17,14,4,(238,82,82))   # label
    return px

def px_hardsuit():
    px=_bg(); c=(67,92,77); hi=(102,132,112); s=(42,62,52); vis=(32,132,182)
    _r(px,10,2,12,6,c); _r(px,8,4,16,5,c); _r(px,11,4,10,4,vis) # helmet
    _r(px,8,9,16,12,c); _r(px,9,10,14,10,hi); _r(px,12,13,8,5,s)# body
    _r(px,2,9,6,10,c); _r(px,24,9,6,10,c)                        # arms
    _r(px,8,21,6,10,c); _r(px,18,21,6,10,c)                      # legs
    return px

def px_hardsuit_helmet():
    px=_bg(); c=(57,77,67); hi=(87,117,97); vis=(32,152,202)
    _c(px,16,15,12,c); _c(px,16,15,10,hi)
    _r(px,6,13,20,7,vis); _r(px,7,14,18,5,(52,172,212))          # visor
    _r(px,4,12,2,9,(37,57,47)); _r(px,26,12,2,9,(37,57,47))      # edges
    return px

def px_mining_helmet():
    px=_bg(); c=(222,192,42); hi=(252,222,72); brim=(182,152,32); lamp=(255,242,162)
    _c(px,16,16,12,c); _c(px,16,16,10,hi)
    _r(px,4,23,24,5,brim)                                          # brim
    _r(px,21,14,7,5,(102,92,87)); _r(px,22,15,5,3,lamp)           # lamp
    _r(px,12,26,2,5,(142,112,32)); _r(px,18,26,2,5,(142,112,32)) # strap
    return px

def px_welding_helmet():
    px=_bg(); c=(162,97,47); hi=(202,132,72); vis=(28,28,33)
    _r(px,4,6,24,22,c); _r(px,5,7,22,20,hi)
    _r(px,7,10,18,11,vis); _r(px,8,11,16,9,(12,18,12))           # visor
    _r(px,8,27,16,4,(112,62,27))                                   # chin
    return px

def px_welding_goggles():
    px=_bg(); o=(182,118,57); m=(82,82,87); lc=(27,57,32)
    _r(px,13,14,6,4,m)                                             # bridge
    for cx in [9,23]:
        _c(px,cx,15,7,o); _c(px,cx,15,5,m); _c(px,cx,15,4,lc)
    _r(px,2,14,2,3,o); _r(px,28,14,2,3,o)                        # straps
    return px

def px_jetpack():
    px=_bg(); c=(47,77,168); hi=(77,118,208); s=(27,52,122); st=(92,82,72)
    _r(px,3,5,10,22,c); _r(px,4,6,8,20,hi)
    _r(px,19,5,10,22,c); _r(px,20,6,8,20,hi)
    _r(px,5,26,6,4,s); _r(px,21,26,6,4,s)                        # nozzles
    _r(px,13,9,6,14,(62,62,72)); _r(px,14,10,4,12,(82,82,92))    # plate
    _l(px,3,9,13,9,st,2); _l(px,19,9,29,9,st,2)                  # straps
    return px

def px_backpack():
    px=_bg(); c=(132,97,67); hi=(167,132,97); s=(92,62,37); st=(107,77,52)
    _r(px,6,5,20,24,c); _r(px,7,6,18,22,hi)
    _r(px,9,11,14,8,s); _r(px,10,12,12,6,(152,112,82))           # pocket
    _r(px,9,11,14,1,(172,172,182))                                 # zipper
    _r(px,7,5,2,24,st); _r(px,23,5,2,24,st)                      # straps
    return px

def px_toolbelt():
    px=_bg(); c=(97,67,32); hi=(142,102,57); bk=(62,52,42); po=(112,82,47)
    _r(px,1,12,30,8,c); _r(px,1,13,30,6,hi)
    _r(px,13,11,6,10,bk); _r(px,14,12,4,8,(182,162,62))          # buckle
    for bx in [3,8,20,25]:
        _r(px,bx,20,5,8,po); _r(px,bx+1,21,3,6,hi)
    return px

def px_rubber_gloves():
    px=_bg(); c=(47,137,118); hi=(72,172,150); s=(27,97,82)
    _r(px,8,9,16,19,c); _r(px,9,10,14,17,hi)
    _r(px,3,13,6,10,c); _r(px,4,14,4,8,hi)                       # thumb
    for fx in [7,11,15,19]:
        _r(px,fx,5,4,6,c); _r(px,fx+1,6,2,4,hi)                  # fingers
    _r(px,8,27,16,3,s); _r(px,8,28,16,1,(62,157,137))            # cuff
    return px

def px_magboots():
    px=_bg(); c=(82,92,112); hi=(112,127,152); s=(52,62,82); sole=(42,182,232); toe=(122,122,132)
    _r(px,4,11,24,17,c); _r(px,5,12,22,15,hi)
    _r(px,4,11,24,5,s)                                             # ankle collar
    _c(px,22,21,5,toe)                                             # toe
    for sy in [27,29]: _r(px,2,sy,28,2,sole)                     # mag sole
    for bx in [6,14,22]: _c(px,bx,15,2,s)                        # bolts
    return px

def px_boots():
    px=_bg(); c=(117,87,57); hi=(157,122,87); s=(77,52,32); sole=(42,37,32)
    _r(px,8,3,16,18,c); _r(px,9,4,14,16,hi)
    _r(px,4,19,24,10,c); _r(px,5,20,22,8,hi)
    _c(px,24,23,5,c); _r(px,4,27,26,4,sole)
    for ly in range(8,18,3): _l(px,10,ly,22,ly,(62,52,42),1)     # laces
    return px

def px_headset():
    px=_bg(); e=(37,37,42); p=(57,52,60); m=(72,72,77)
    for x in range(5,27):                                          # headband
        y=4+int(6*((x-16)**2)/(11**2))
        _s(px,x,y,e); _s(px,x,y+1,e)
    _c(px,6,16,7,p); _c(px,6,16,5,e)
    _c(px,26,16,7,p); _c(px,26,16,5,e)                            # ear cups
    _l(px,6,22,4,27,m,2); _c(px,3,28,2,m)                        # mic arm
    return px

def px_gas_mask():
    px=_bg(); c=(77,107,77); hi=(107,142,107); f=(57,57,62); st=(42,42,42)
    _c(px,16,14,12,c); _c(px,16,14,10,hi)
    _r(px,9,8,14,8,(32,102,132)); _r(px,10,9,12,6,(47,132,162))  # visor
    _c(px,8,22,5,f); _c(px,24,22,5,f)
    _c(px,8,22,3,(72,72,77)); _c(px,24,22,3,(72,72,77))           # filters
    _r(px,2,11,3,8,st); _r(px,27,11,3,8,st)                      # straps
    return px

def px_sunglasses():
    px=_bg(); f=(22,47,82); lc=(32,62,112); br=(62,62,72); arm=(27,27,32)
    _r(px,2,11,12,10,f); _r(px,3,12,10,8,lc); _r(px,4,13,8,6,(17,37,67))
    _r(px,18,11,12,10,f); _r(px,19,12,10,8,lc); _r(px,20,13,8,6,(17,37,67))
    _r(px,14,15,4,2,br)                                            # bridge
    _l(px,2,11,0,15,arm); _l(px,30,11,32,15,arm)                 # arms
    return px

def px_satchel():
    px=_bg(); c=(167,132,92); hi=(202,172,127); fl=(152,117,80)
    _r(px,5,12,22,17,c); _r(px,6,13,20,15,hi)
    _r(px,5,5,22,9,fl); _r(px,6,6,20,7,(182,147,107))            # flap
    _r(px,13,14,6,4,(52,52,57)); _r(px,14,15,4,2,(202,172,52))   # buckle
    _r(px,14,3,4,4,c)                                              # strap
    return px

def px_duffel_bag():
    px=_bg(); c=(82,102,72); hi=(112,142,97); z=(222,217,202)
    _r(px,3,10,26,15,c); _r(px,4,11,24,13,hi)
    _c(px,6,17,8,c); _c(px,26,17,8,c)                             # end caps
    _r(px,8,10,16,2,z)                                             # zipper
    _r(px,10,5,12,3,(57,72,47)); _r(px,11,6,10,1,hi)             # handle
    return px

def px_briefcase():
    px=_bg(); c=(87,64,44); hi=(122,92,64); s=(57,40,24); latch=(202,187,52)
    _r(px,3,11,26,17,c); _r(px,4,12,24,15,hi)
    _r(px,3,18,26,1,s)                                             # divider
    _r(px,11,7,10,5,c); _r(px,10,10,2,3,c); _r(px,20,10,2,3,c)
    _r(px,12,8,8,3,hi)                                             # handle
    _r(px,13,18,6,4,latch)                                         # clasp
    return px

def px_secure_briefcase():
    px=_bg(); c=(64,64,77); hi=(92,92,107); combo=(177,167,52)
    _r(px,3,11,26,17,c); _r(px,4,12,24,15,hi)
    _r(px,3,18,26,1,(42,42,52))
    _r(px,11,7,10,5,c); _r(px,10,10,2,3,c); _r(px,20,10,2,3,c)
    _r(px,12,8,8,3,hi)
    for dx in [9,12,16,19]: _c(px,dx,20,2,combo)                  # combo dials
    return px

def px_medical_kit():
    px=_bg(); b=(238,238,242); cr=(202,32,32)
    _r(px,3,7,26,21,b); _r(px,4,8,24,19,(248,248,252))
    _r(px,3,27,26,2,(188,188,192))
    _r(px,11,4,10,5,(202,202,207)); _r(px,12,5,8,3,(222,222,227)) # handle
    _r(px,13,11,6,13,cr); _r(px,8,15,16,5,cr)                     # cross
    _r(px,14,12,4,11,(222,42,42)); _r(px,9,16,14,3,(222,42,42))  # cross hi
    return px

def px_storage_crate():
    px=_bg(); c=(147,122,92); pl=(112,90,64); metal=(152,150,157)
    _r(px,2,5,28,24,c); _r(px,3,6,26,22,(167,140,107))
    _l(px,2,5,30,29,pl,2); _l(px,30,5,2,29,pl,2)                 # X planks
    _r(px,2,5,28,2,metal); _r(px,2,27,28,2,metal)
    _r(px,2,5,2,24,metal); _r(px,28,5,2,24,metal)                # frame
    for bx,by in [(2,5),(26,5),(2,25),(26,25)]: _c(px,bx+2,by+1,2,metal)
    return px

def px_junk_box():
    px=_bg(); c=(137,114,84); hi=(167,142,107); fl=(122,102,74); tape=(182,202,222)
    _r(px,3,12,26,17,c); _r(px,4,13,24,15,hi)
    _l(px,3,12,16,5,fl,3); _l(px,29,12,16,5,fl,3)                # flaps
    _r(px,13,12,6,17,tape); _r(px,14,12,4,17,(192,212,232))      # tape
    return px

def px_evidence_bag():
    px=_bg(); b=(217,212,187); z=(202,102,32); seal=(222,52,52)
    _r(px,4,7,24,21,b); _r(px,5,8,22,19,(228,222,198))
    _r(px,4,7,24,4,z); _r(px,5,8,22,2,(232,132,52))              # zipper
    _r(px,8,24,16,3,seal); _r(px,9,25,14,1,(242,82,82))          # seal
    _r(px,9,12,14,8,(197,194,172))                                 # interior
    return px

def px_glass_sheet():
    px=_bg(); b=(147,207,237); hi=(182,227,250); sh=(97,157,187); e=(72,132,167)
    _r(px,5,3,22,26,b); _r(px,6,4,20,24,hi)
    _l(px,7,4,17,24,(202,237,250),2)                              # sheen
    _r(px,5,3,22,1,e); _r(px,5,28,22,1,e)
    _r(px,5,3,1,26,e); _r(px,26,3,1,26,e)                        # edges
    return px

# ── Texture manifest ──────────────────────────────────────────────────────────

W, H = 32, 32

wire_base = from_legacy("pipes_n_cables/layer_cable/l1-noconnection.png")
wire_overlays = {
    1: from_legacy("pipes_n_cables/layer_cable/l1-1.png"),
    2: from_legacy("pipes_n_cables/layer_cable/l1-2.png"),
    4: from_legacy("pipes_n_cables/layer_cable/l1-4.png"),
    8: from_legacy("pipes_n_cables/layer_cable/l1-8.png"),
}
wire_variants = compose_bitmask(wire_base, wire_overlays)

pipe_base = transparent_rgba(W, H)
pipe_overlays = {
    1: from_legacy("pipes_n_cables/pipe_underlays/intact_1_1.png"),
    2: from_legacy("pipes_n_cables/pipe_underlays/intact_2_1.png"),
    4: from_legacy("pipes_n_cables/pipe_underlays/intact_4_1.png"),
    8: from_legacy("pipes_n_cables/pipe_underlays/intact_8_1.png"),
}
pipe_variants = compose_bitmask(pipe_base, pipe_overlays)

TEXTURES = {
    # Voxel tiles
    "textures/tiles/floor_steel.png":  diamond_plate(W, H),
    "textures/tiles/floor_monk_steel.png": from_legacy_turf("floors/catwalk_plating/iron_above.png"),
    "textures/tiles/floor_monk_maint.png": from_legacy_turf("floors/catwalk_plating/maint_above.png"),
    "textures/tiles/wall.png":         grid(W, H, 70, 70, 80, 40, 45),
    "textures/tiles/rwall.png":        rwall_tex(W, H),
    "textures/tiles/window.png":       window_tex(W, H),
    "textures/tiles/plating.png":      cross_hatch(W, H, 90, 80, 70, 50, 45, 40),
    "textures/tiles/plating_monk.png": from_legacy_turf("floors/catwalk_plating/iron_below.png"),
    "textures/tiles/catwalk.png":      catwalk_tex(W, H),
    "textures/tiles/light_tube.png":   light_tube_tex(W, H),
    "textures/tiles/fallback.png":     fallback_magenta(W, H),

    # Items — legacy sprites from legacysets/extracted/obj/
    "textures/items/wrench.png":           from_legacy("tools/wrench.png"),
    "textures/items/screwdriver.png":      from_legacy("tools/screwdriver.png"),
    "textures/items/crowbar.png":          from_legacy("tools/crowbar.png"),
    "textures/items/wirecutters.png":      from_legacy("tools/cutters.png"),
    "textures/items/toolbox.png":          from_legacy("storage/toolbox/toolbox_default.png"),
    "textures/items/stun_baton.png":       from_legacy("weapons/baton/stunbaton.png"),
    "textures/items/id_card.png":          from_legacy("card/assigned.png"),
    "textures/items/fire_extinguisher.png":from_legacy("tools/fire_extinguisher0.png"),
    "textures/items/cable_coil.png":       solid(W, H, 220, 200, 40),
    "textures/items/metal_sheet.png":      solid(W, H, 160, 170, 180),
    "textures/items/glass_sheet.png":      from_legacy("stack_objects/sheet-glass.png"),
    "textures/items/flashlight.png":       from_legacy("lighting/flashdark.png"),
    "textures/items/medipen.png":          from_legacy("medical/syringe/medipen.png"),
    "textures/items/pill_bottle.png":      from_legacy("storage/box/pillbox.png"),
    "textures/items/bandage.png":          solid(W, H, 240, 230, 220),
    # Equipment
    "textures/items/hardsuit.png":         from_legacy("clothing/suits/spacesuit/space.png"),
    "textures/items/hardsuit_helmet.png":  from_legacy("clothing/head/spacehelm/space.png"),
    "textures/items/mining_helmet.png":    from_legacy("clothing/head/utility/hardhat0_orange.png"),
    "textures/items/welding_helmet.png":   from_legacy("clothing/head/utility/welding.png"),
    "textures/items/welding_goggles.png":  from_legacy("clothing/glasses/welding-g.png"),
    "textures/items/jetpack.png":          from_legacy("canisters/jetpack.png"),
    "textures/items/oxygen_tank.png":      solid(W, H, 200, 220, 240),
    "textures/items/backpack.png":         from_legacy("storage/backpack/backpack.png"),
    "textures/items/toolbelt.png":         from_legacy("clothing/belts/ebelt.png"),
    "textures/items/insulated_gloves.png": solid(W, H, 220, 200, 50),
    "textures/items/rubber_gloves.png":    from_legacy("clothing/gloves/latex.png"),
    "textures/items/magboots.png":         from_legacy("clothing/shoes/magboots0.png"),
    "textures/items/boots.png":            from_legacy("clothing/shoes/workboots.png"),
    "textures/items/headset.png":          from_legacy("clothing/headsets/headset.png"),
    "textures/items/gas_mask.png":         from_legacy("clothing/masks/gas_mask.png"),
    "textures/items/sunglasses.png":       from_legacy("clothing/glasses/bigsunglasses.png"),
    # Containers
    "textures/items/satchel.png":          from_legacy("storage/backpack/satchel-norm.png"),
    "textures/items/duffel_bag.png":       from_legacy("storage/backpack/duffel.png"),
    "textures/items/briefcase.png":        from_legacy("storage/case/briefcase.png"),
    "textures/items/secure_briefcase.png": from_legacy("storage/case/secure.png"),
    "textures/items/medical_kit.png":      from_legacy("storage/medkit/medkit.png"),
    "textures/items/storage_crate.png":    from_legacy("storage/crates/cargo.png"),
    "textures/items/junk_box.png":         from_legacy("storage/box/box.png"),
    "textures/items/evidence_bag.png":     from_legacy("storage/storage/evidenceobj.png"),

    # --- New tools ---
    "textures/items/welder.png":           from_legacy("tools/welder.png"),
    "textures/items/multitool.png":        from_legacy("devices/tool/multitool.png"),
    "textures/items/drill.png":            from_legacy("tools/drill.png"),
    "textures/items/jaws_of_life.png":     from_legacy("tools/jaws.png"),
    "textures/items/boxcutter.png":        from_legacy("tools/boxcutter.png"),
    "textures/items/rcd.png":              from_legacy("tools/rcd.png"),
    "textures/items/health_analyzer.png":  solid(W, H, 60,  200, 120),
    "textures/items/pda.png":              from_legacy("devices/modular_pda/pda.png"),
    "textures/items/radio.png":            from_legacy("devices/circuitry_n_data/radio.png"),
    "textures/items/pickaxe.png":          from_legacy("mining/pickaxe.png"),
    "textures/items/mining_drill.png":     solid(W, H, 90,  70,  50),
    "textures/items/shovel.png":           from_legacy("mining/shovel.png"),
    # --- New weapons ---
    "textures/items/baseball_bat.png":     from_legacy("weapons/bat/baseball_bat.png"),
    "textures/items/hunting_knife.png":    solid(W, H, 140, 110, 60),
    "textures/items/survival_knife.png":   solid(W, H, 120, 100, 55),
    "textures/items/claymore.png":         solid(W, H, 160, 160, 175),
    "textures/items/katana.png":           from_legacy("weapons/sword/katana.png"),
    "textures/items/energy_sword.png":     solid(W, H, 100, 200, 255),
    "textures/items/classic_baton.png":    from_legacy("weapons/baton/classic_baton.png"),
    "textures/items/riot_shield.png":      solid(W, H, 50,  80,  140),
    "textures/items/bow.png":              from_legacy("weapons/bows/bows/bow.png"),
    "textures/items/fireaxe.png":          from_legacy("wallmounts/fireaxe.png"),
    # --- New medical ---
    "textures/items/syringe.png":          from_legacy("medical/syringe/syringe_0.png"),
    "textures/items/beaker.png":           from_legacy("medical/chemical/beaker.png"),
    "textures/items/defib.png":            solid(W, H, 240, 200, 40),
    "textures/items/scalpel.png":          from_legacy("medical/surgery_tools/scalpel.png"),
    "textures/items/blood_pack.png":       from_legacy("medical/bloodpack/bloodpack.png"),
    "textures/items/firstaid_kit.png":     solid(W, H, 240, 240, 240),
    # --- New equipment ---
    "textures/items/body_armor.png":       from_legacy("clothing/suits/armor/armor.png"),
    "textures/items/security_armor.png":   from_legacy("clothing/suits/armor/armor_sec.png"),
    "textures/items/hazard_vest.png":      solid(W, H, 230, 150, 20),
    "textures/items/combat_helmet.png":    solid(W, H, 60,  70,  60),
    "textures/items/hard_hat.png":         from_legacy("clothing/head/utility/hardhat0_yellow.png"),
    # --- New containers ---
    "textures/items/bodybag.png":          solid(W, H, 50,  50,  55),
    "textures/items/mining_satchel.png":   from_legacy("mining/satchel.png"),
    "textures/items/ore_box.png":          solid(W, H, 100, 85,  60),

    # UI elements
    "textures/ui/slot.png":            ui_slot(W, H),
    "textures/ui/slot_active.png":     ui_slot(W, H, 60, 80, 100, 140),
    "textures/ui/hotbar_bg.png":       ui_hotbar_bg(W * 9 + 10, H + 10),
    "textures/ui/crosshair.png":       solid(W, H, 0, 0, 0),   # placeholder
    "textures/ui/cursor.png":          solid(16, 24, 200, 200, 200),
}

for i, tex in enumerate(wire_variants):
    TEXTURES[f"textures/tiles/wire_{i}.png"] = tex

for i, tex in enumerate(pipe_variants):
    TEXTURES[f"textures/tiles/pipe_{i}.png"] = tex

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--force", action="store_true", help="Overwrite existing textures")
    args = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    print(f"Generating textures under: {root}")
    skipped = 0
    written = 0
    for rel, pixels in TEXTURES.items():
        path = os.path.join(root, rel)
        if not args.force and os.path.exists(path):
            skipped += 1
            continue
        # Detect RGBA (4-tuple) vs RGB (3-tuple) by inspecting first pixel
        if pixels and pixels[0] and len(pixels[0][0]) == 4:
            write_png_rgba(path, pixels)
        else:
            write_png(path, pixels)
        written += 1
    print(f"\nDone — {written} textures written, {skipped} skipped (already exist). Pass --force to overwrite.")
