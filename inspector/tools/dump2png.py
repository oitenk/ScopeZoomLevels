"""Turns SZL Inspector image dumps (raw .bin files) into PNGs, at half size.

Usage: python -I dump2png.py <dump folder> [out folder]
File names carry the size, DXGI format and row pitch: ..._3840x2160_f28_pitch15360.bin
Values are stretched so small IDs and masks are visible; the stretch is printed per image.
"""
import os
import re
import struct
import sys
import zlib


def png(path, w, h, rows, channels):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    color = {1: 0, 3: 2}[channels]
    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def half_floats(b):
    return struct.unpack("<%de" % (len(b) // 2), b)


def floats(b):
    return struct.unpack("<%df" % (len(b) // 4), b)


def convert(path, out_dir):
    m = re.search(r"_(\d+)x(\d+)_f(\d+)_pitch(\d+)\.bin$", path)
    w, h, fmt, pitch = map(int, m.groups())
    data = open(path, "rb").read()
    base = os.path.join(out_dir, os.path.basename(path)[:-4])
    step = 2
    ow, oh = w // step, h // step
    bpp = {28: 4, 29: 4, 27: 4, 24: 4, 61: 1, 62: 1, 49: 2, 54: 2, 10: 8, 41: 4, 39: 4, 19: 8, 20: 8, 2: 16, 34: 4, 37: 4}
    # Depth/stencil planes come out as their own copyable formats.
    size = len(data) // h if h else 0
    bp = pitch and bpp.get(fmt)
    if bp is None:
        bp = max(1, min(16, (pitch // w) if w else 1))
    lines = []
    for y in range(0, oh * step, step):
        row = data[y * pitch : y * pitch + w * bp]
        lines.append(row)

    def stretch(vals):
        lo, hi = min(vals), max(vals)
        return lo, hi

    summary = ""
    if bp == 4 and fmt in (27, 28, 29, 24):
        rgb, alpha = [], []
        for row in lines:
            px = row[0 : ow * step * 4]
            rgb.append(bytes(b for x in range(0, len(px), 4 * step) for b in px[x : x + 3]))
            alpha.append(bytes(px[x + 3] for x in range(0, len(px), 4 * step)))
        png(base + "_rgb.png", ow, oh, rgb, 3)
        png(base + "_alpha.png", ow, oh, alpha, 1)
        summary = "rgb + alpha"
    elif bp == 1:
        vals = [row[x] for row in lines for x in range(0, ow * step, step)]
        lo, hi = stretch(vals)
        scale = 255 / (hi - lo) if hi > lo else 1
        out = [bytes(int((row[x] - lo) * scale) for x in range(0, ow * step, step)) for row in lines]
        png(base + ".png", ow, oh, out, 1)
        distinct = sorted(set(vals))
        summary = f"8-bit values {lo}..{hi}, distinct {distinct[:16]}{'...' if len(distinct) > 16 else ''}"
    elif bp == 2:
        vals = [half_floats(row[x * 2 : x * 2 + 2])[0] for row in lines for x in range(0, ow * step, step)]
        lo, hi = stretch(vals)
        scale = 255 / (hi - lo) if hi > lo else 1
        it = iter(vals)
        out = [bytes(int((next(it) - lo) * scale) for _ in range(ow)) for _ in lines]
        png(base + ".png", ow, oh, out, 1)
        summary = f"16-bit float {lo:.3f}..{hi:.3f}"
    elif bp == 8 and fmt == 10:
        out = []
        for row in lines:
            vals = half_floats(row[0 : ow * step * 8])
            px = bytearray()
            for x in range(0, len(vals), 4 * step):
                for c in vals[x : x + 3]:
                    v = c / (1 + c) if c > 0 else 0  # simple tone map
                    px.append(int(min(1, v) ** (1 / 2.2) * 255))
            out.append(bytes(px))
        png(base + ".png", ow, oh, out, 3)
        summary = "HDR colour (tone mapped for viewing)"
    elif bp == 4:
        vals = [floats(row[x * 4 : x * 4 + 4])[0] for row in lines for x in range(0, ow * step, step)]
        finite = [v for v in vals if v == v and abs(v) < 1e30]
        lo, hi = (min(finite), max(finite)) if finite else (0, 1)
        scale = 255 / (hi - lo) if hi > lo else 1
        it = iter(vals)
        out = [bytes(int(max(0, min(255, (next(it) - lo) * scale))) for _ in range(ow)) for _ in lines]
        png(base + ".png", ow, oh, out, 1)
        summary = f"32-bit float {lo:.5f}..{hi:.5f}"
    else:
        summary = f"skipped ({bp} bytes per pixel)"
    print(os.path.basename(path), "->", summary)


if __name__ == "__main__":
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src
    os.makedirs(dst, exist_ok=True)
    for name in sorted(os.listdir(src)):
        if name.endswith(".bin"):
            convert(os.path.join(src, name), dst)
