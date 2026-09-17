"""Host-side city map renderer -- proof of concept, offline.

Renders the live 120x100 map from a WRAM dump using tile graphics out of a
VRAM dump, following the chain in docs/REFERENCE_map_format.md:

    v  = cell & 0x03FF                      10-bit tile index
    tp = u16(TILE_ADDR + v*2)               background tile
    tu = u16(TILU_ADDR + v*2)               overlay tile (0x300 = empty)
    tile CHR: 4bpp SNES, tc & 0x3FF = tile, (tc >> 10) & 7 = palette
    palette:  BGR555 at $7E2440

Deliberately offline first. The point is to prove the renderer reproduces what
the guest draws BEFORE any of it goes near the host -- the framebuffer is the
oracle, and this project has repeatedly paid for skipping that step.

STATUS: the tile chain works -- the output is a legible map, confirmed from
play ("clearly visible"). What is NOT yet right is agreement with the guest:
sliding the render over the framebuffer for best alignment scores ~11% exact
pixels on a real in-game frame (savestate_5, Las Vegas).

Two false alarms already burned, both worth not repeating:

  - The first comparison used savestate_3 believing it was Las Vegas. The user
    had re-saved slots 1-4 that day, so it was parked on the DISASTER MENU --
    the framebuffer held a menu, not a map, and 8.5% was measuring nothing.
    Check $01df (screen) and $0040 (scenario) in the dump before comparing.
  - A whole-screen exact-pixel score is a poor metric regardless: the guest
    frame also carries the HUD, the cursor sprite and status text, none of
    which this renders. Compare the map region only, or compare structure.

Remaining candidates for the mismatch, none yet separated: whether $7E2440 is
the right palette source in a given state (it doubles as a menu tilemap
buffer), whether $01bd/$01bf are in cell units, and whether one map cell is
really one 8x8 tile -- the scroll bounds imply a ~25x22 cell view while 256px
is 32 tiles, and those do not reconcile.

Usage:
    python tools/render_map.py <wram.bin> <ppu.bin> <rom.sfc> <out.png>
                               [--scroll-x N --scroll-y N --w N --h N]
"""
import sys, struct, zlib

MAP_OFF   = 0x010200          # $7F0200 within a 128K WRAM dump
PAL_OFF   = 0x002440          # $7E2440
TILE_ADDR = 0x0156A9          # 02:d6a9, background tile table (USA)
TILU_ADDR = TILE_ADDR - 0x77C # 0x014F2D, overlay tile table
MAP_W, MAP_H = 120, 100
VRAM_BYTES = 0x8000 * 2

def u16(b, o): return b[o] | (b[o + 1] << 8)

def load_palette(wram):
    """256 BGR555 entries -> RGB888."""
    pal = []
    for i in range(256):
        v = u16(wram, PAL_OFF + i * 2)
        r = (v & 31) * 255 // 31
        g = ((v >> 5) & 31) * 255 // 31
        b = ((v >> 10) & 31) * 255 // 31
        pal.append((r, g, b))
    return pal

def tile_pixels(vram, tile_index):
    """Decode one 4bpp SNES tile to 64 palette indices (0 = transparent)."""
    base = (tile_index & 0x3FF) * 32
    out = [0] * 64
    if base + 32 > len(vram): return out
    for y in range(8):
        p0 = vram[base + y * 2]
        p1 = vram[base + y * 2 + 1]
        p2 = vram[base + 16 + y * 2]
        p3 = vram[base + 16 + y * 2 + 1]
        for x in range(8):
            bit = 7 - x
            out[y * 8 + x] = (((p0 >> bit) & 1)
                              | (((p1 >> bit) & 1) << 1)
                              | (((p2 >> bit) & 1) << 2)
                              | (((p3 >> bit) & 1) << 3))
    return out

def render(wram, vram, rom, sx, sy, cols, rows):
    pal = load_palette(wram)
    cache = {}
    W, H = cols * 8, rows * 8
    img = bytearray(W * H * 3)

    def blit(tc, px, py):
        if tc not in cache: cache[tc] = tile_pixels(vram, tc)
        pix = cache[tc]
        pbase = ((tc >> 10) & 7) * 16
        for ty in range(8):
            oy = py + ty
            if oy < 0 or oy >= H: continue
            for tx in range(8):
                ox = px + tx
                if ox < 0 or ox >= W: continue
                ci = pix[ty * 8 + tx]
                if ci == 0: continue          # transparent
                r, g, b = pal[(pbase + ci) & 0xFF]
                o = (oy * W + ox) * 3
                img[o] = r; img[o + 1] = g; img[o + 2] = b

    for ry in range(rows):
        for rx in range(cols):
            mx, my = sx + rx, sy + ry
            if not (0 <= mx < MAP_W and 0 <= my < MAP_H): continue
            v = u16(wram, MAP_OFF + (my * MAP_W + mx) * 2) & 0x03FF
            blit(u16(rom, TILE_ADDR + v * 2), rx * 8, ry * 8)

    # overlays last, offset -1,-1 so they overlap up and left
    for ry in range(rows):
        for rx in range(cols):
            mx, my = sx + rx, sy + ry
            if not (0 <= mx < MAP_W and 0 <= my < MAP_H): continue
            v = u16(wram, MAP_OFF + (my * MAP_W + mx) * 2) & 0x03FF
            tu = u16(rom, TILU_ADDR + v * 2)
            if (tu & 0x3FF) == 0x300: continue
            blit(tu, rx * 8 - 1, ry * 8 - 1)
    return W, H, bytes(img)

def png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def ck(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    open(path, 'wb').write(
        b'\x89PNG\r\n\x1a\n'
        + ck(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
        + ck(b'IDAT', zlib.compress(raw, 9)) + ck(b'IEND', b''))

def main():
    wram = open(sys.argv[1], 'rb').read()
    ppu  = open(sys.argv[2], 'rb').read()
    rom  = open(sys.argv[3], 'rb').read()
    out  = sys.argv[4]
    a = sys.argv[5:]
    def opt(name, dflt):
        return int(a[a.index(name) + 1]) if name in a else dflt
    sx = opt('--scroll-x', wram[0x01bd])
    sy = opt('--scroll-y', wram[0x01bf])
    cols = opt('--w', 32)
    rows = opt('--h', 28)
    vram = ppu[:VRAM_BYTES]
    w, h, img = render(wram, vram, rom, sx, sy, cols, rows)
    png(out, w, h, img)
    print("rendered %dx%d from map (%d,%d) -> %s" % (w, h, sx, sy, out))

main()
