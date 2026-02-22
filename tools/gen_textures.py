"""
gen_textures.py — generate 32x32 placeholder PNG textures for VS13.
Uses only Python stdlib (struct + zlib); no Pillow required.
"""
import os, struct, zlib

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

# ── Drawing helpers ────────────────────────────────────────────────────────────

def solid(w, h, r, g, b):
    return [[(r, g, b)] * w for _ in range(h)]

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

# ── Texture manifest ──────────────────────────────────────────────────────────

W, H = 32, 32

TEXTURES = {
    # Voxel tiles
    "textures/tiles/floor_steel.png":  diamond_plate(W, H),
    "textures/tiles/wall.png":         grid(W, H, 70, 70, 80, 40, 45),
    "textures/tiles/rwall.png":        rwall_tex(W, H),
    "textures/tiles/window.png":       window_tex(W, H),
    "textures/tiles/plating.png":      cross_hatch(W, H, 90, 80, 70, 50, 45, 40),
    "textures/tiles/catwalk.png":      catwalk_tex(W, H),
    "textures/tiles/light_tube.png":   light_tube_tex(W, H),
    "textures/tiles/fallback.png":     fallback_magenta(W, H),

    # Items (wrench, screwdriver, crowbar, etc.) — solid tinted squares for now
    "textures/items/wrench.png":       solid(W, H, 150, 150, 160),
    "textures/items/screwdriver.png":  solid(W, H, 180, 160, 80),
    "textures/items/crowbar.png":      solid(W, H, 160, 60,  60),
    "textures/items/wirecutters.png":  solid(W, H, 80,  130, 80),
    "textures/items/toolbox.png":      solid(W, H, 60,  100, 160),
    "textures/items/stun_baton.png":   solid(W, H, 200, 180, 60),
    "textures/items/id_card.png":      solid(W, H, 60,  180, 200),

    # UI elements
    "textures/ui/slot.png":            ui_slot(W, H),
    "textures/ui/slot_active.png":     ui_slot(W, H, 60, 80, 100, 140),
    "textures/ui/hotbar_bg.png":       ui_hotbar_bg(W * 9 + 10, H + 10),
    "textures/ui/crosshair.png":       solid(W, H, 0, 0, 0),   # placeholder
    "textures/ui/cursor.png":          solid(16, 24, 200, 200, 200),
}

if __name__ == "__main__":
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    print(f"Generating textures under: {root}")
    for rel, pixels in TEXTURES.items():
        write_png(os.path.join(root, rel), pixels)
    print(f"\nDone — {len(TEXTURES)} textures written.")
