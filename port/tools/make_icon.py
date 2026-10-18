"""Build the port's icon from the game's own HUD Shine sprite (game_6.szs /timg/shine_get_icon.bti).

python port/tools/make_icon.py <path to the disc's files/data folder>
Writes port/res/shine.ico (16-256 px, PNG-compressed entries) and port/res/shine_1024.png (Discord).
The sprite (32x38) is scaled by whole-pixel steps (nearest neighbour) so it stays crisp, then
centred on a transparent square canvas.
"""
import os, struct, sys, zlib
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "level_export", "tools"))
import rarc
from bmd2obj import decode_texture, u16, u32

def png_bytes(w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")

def crop_alpha(w, h, px):
    xs = [x for y in range(h) for x in range(w) if px[(y * w + x) * 4 + 3] > 8]
    ys = [y for y in range(h) for x in range(w) if px[(y * w + x) * 4 + 3] > 8]
    x0, x1, y0, y1 = min(xs), max(xs) + 1, min(ys), max(ys) + 1
    out = bytearray()
    for y in range(y0, y1):
        out += px[(y * w + x0) * 4:(y * w + x1) * 4]
    return x1 - x0, y1 - y0, out

def place(sw, sh, src, size, margin):
    """Nearest-neighbour: whole-pixel scale when it fits, else area-average downscale."""
    room = size - 2 * margin
    k = room / max(sw, sh)
    dw, dh = max(1, round(sw * k)), max(1, round(sh * k))
    if k >= 1:
        k = int(k); dw, dh = sw * k, sh * k  # whole pixels only: stays sharp
    canvas = bytearray(size * size * 4)
    ox, oy = (size - dw) // 2, (size - dh) // 2
    for y in range(dh):
        for x in range(dw):
            if k >= 1:
                s = ((y // k) * sw + (x // k)) * 4
                p = src[s:s + 4]
            else:  # box filter over the source footprint, premultiplied
                fx0, fx1 = x * sw / dw, (x + 1) * sw / dw
                fy0, fy1 = y * sh / dh, (y + 1) * sh / dh
                acc = [0.0] * 4; tot = 0.0
                for sy in range(int(fy0), min(sh, int(fy1 + 0.999))):
                    for sx in range(int(fx0), min(sw, int(fx1 + 0.999))):
                        wgt = (min(fx1, sx + 1) - max(fx0, sx)) * (min(fy1, sy + 1) - max(fy0, sy))
                        if wgt <= 0: continue
                        s = (sy * sw + sx) * 4; a = src[s + 3] / 255.0
                        for c in range(3): acc[c] += src[s + c] * a * wgt
                        acc[3] += a * wgt; tot += wgt
                a = acc[3] / tot if tot else 0
                p = bytes([int(acc[c] / acc[3]) if acc[3] else 0 for c in range(3)] + [int(a * 255 + 0.5)])
            d = ((oy + y) * size + ox + x) * 4
            canvas[d:d + 4] = p
    return canvas

def main():
    data = sys.argv[1]
    root = rarc.read(open(os.path.join(data, "game_6.szs"), "rb").read())
    b = root.get("/timg/shine_get_icon.bti")
    fmt, w, h = b[0], u16(b, 2), u16(b, 4)
    px = bytearray(decode_texture(b, u32(b, 0x1C), fmt, w, h, None))
    sw, sh, spr = crop_alpha(w, h, px)
    res = os.path.join(HERE, "..", "res")
    os.makedirs(res, exist_ok=True)
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    imgs = [png_bytes(s, s, place(sw, sh, spr, s, 0 if s <= 48 else s // 64)) for s in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    off = 6 + 16 * len(sizes); dirs = b""
    for s, im in zip(sizes, imgs):
        dirs += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(im), off); off += len(im)
    open(os.path.join(res, "shine.ico"), "wb").write(head + dirs + b"".join(imgs))
    open(os.path.join(res, "shine_1024.png"), "wb").write(png_bytes(1024, 1024, place(sw, sh, spr, 1024, 64)))
    print("sprite %dx%d -> port/res/shine.ico, port/res/shine_1024.png" % (sw, sh))

if __name__ == "__main__":
    main()
