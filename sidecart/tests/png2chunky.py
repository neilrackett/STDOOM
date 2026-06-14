#!/usr/bin/env python3
"""
png2chunky.py - build-time PNG -> C header generator for UPTEST.TOS.

Converts the real Doom screenshots (doom_0/1/2.png, 320x200 8-bit indexed)
into doom_frames.h: for each frame, the raw 64000 palette-index bytes plus the
768-byte (256xRGB) palette, ready to embed in the standalone upload benchmark.

Pure Python 3 standard library only (zlib, struct) - no Pillow / ImageMagick,
so it runs on a stock host with no extra install. Runs on the host (macOS),
NOT through the m68k cross toolchain.

Usage (from repo root, invoked by the Makefile):
    python3 sidecart/tests/png2chunky.py
"""

import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FRAMES = ["doom_0.png", "doom_1.png", "doom_2.png"]
OUT = os.path.join(HERE, "doom_frames.h")

EXPECT_W = 320
EXPECT_H = 200


def read_chunks(data):
    """Yield (type, payload) for each PNG chunk after the 8-byte signature."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos = 8
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        yield ctype, payload
        pos += 12 + length  # length + type + data + CRC


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def unfilter(raw, width, height, bpp):
    """Reverse PNG scanline filters. bpp = bytes per pixel (1 for 8-bit idx)."""
    stride = width * bpp
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        ftype = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if ftype == 0:        # None
            pass
        elif ftype == 1:      # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:      # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:      # Average
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:      # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + paeth(a, prev[i], c)) & 0xFF
        else:
            raise ValueError("bad filter type %d on row %d" % (ftype, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return out


def decode_png(path):
    """Return (indices[64000], palette[768]) for a 320x200 8-bit indexed PNG."""
    with open(path, "rb") as f:
        data = f.read()

    width = height = bitdepth = colortype = None
    idat = bytearray()
    plte = None
    for ctype, payload in read_chunks(data):
        if ctype == b"IHDR":
            width, height, bitdepth, colortype, _, _, interlace = struct.unpack(
                ">IIBBBBB", payload)
            if interlace != 0:
                raise ValueError("%s: interlaced PNG not supported" % path)
        elif ctype == b"PLTE":
            plte = payload
        elif ctype == b"IDAT":
            idat += payload
        elif ctype == b"IEND":
            break

    if (width, height) != (EXPECT_W, EXPECT_H):
        raise ValueError("%s: expected %dx%d, got %dx%d"
                         % (path, EXPECT_W, EXPECT_H, width, height))
    if bitdepth != 8 or colortype != 3:
        raise ValueError("%s: expected 8-bit indexed (colortype 3), got "
                         "bitdepth=%d colortype=%d" % (path, bitdepth, colortype))
    if plte is None:
        raise ValueError("%s: no PLTE chunk" % path)

    raw = zlib.decompress(bytes(idat))
    indices = unfilter(raw, width, height, 1)
    if len(indices) != width * height:
        raise ValueError("%s: decoded %d bytes, expected %d"
                         % (path, len(indices), width * height))

    # Normalise palette to a full 768 bytes (pad missing entries with 0).
    palette = bytearray(768)
    palette[:len(plte)] = plte[:768]
    return bytes(indices), bytes(palette)


def emit_array(fh, name, blob):
    fh.write("static const unsigned char %s[%d] = {\n" % (name, len(blob)))
    for i in range(0, len(blob), 16):
        chunk = blob[i:i + 16]
        fh.write("  " + ",".join("%d" % b for b in chunk) + ",\n")
    fh.write("};\n\n")


def main():
    frames = []
    for name in FRAMES:
        path = os.path.join(HERE, name)
        idx, pal = decode_png(path)
        frames.append((idx, pal))
        sys.stderr.write("png2chunky: %s -> %d idx bytes, %d palette bytes\n"
                         % (name, len(idx), len(pal)))

    with open(OUT, "w") as fh:
        fh.write("/* Generated by png2chunky.py - DO NOT EDIT.\n"
                 " * Source: %s\n */\n" % ", ".join(FRAMES))
        fh.write("#ifndef DOOM_FRAMES_H\n#define DOOM_FRAMES_H\n\n")
        fh.write("#define DOOM_FRAME_COUNT %d\n" % len(frames))
        fh.write("#define DOOM_FRAME_W %d\n" % EXPECT_W)
        fh.write("#define DOOM_FRAME_H %d\n" % EXPECT_H)
        fh.write("#define DOOM_FRAME_BYTES %d\n\n" % (EXPECT_W * EXPECT_H))
        for i, (idx, pal) in enumerate(frames):
            emit_array(fh, "doom_frame_%d" % i, idx)
            emit_array(fh, "doom_palette_%d" % i, pal)
        fh.write("static const unsigned char *const doom_frames[%d] = {\n"
                 % len(frames))
        fh.write("  " + ", ".join("doom_frame_%d" % i
                                  for i in range(len(frames))) + "\n};\n\n")
        fh.write("static const unsigned char *const doom_palettes[%d] = {\n"
                 % len(frames))
        fh.write("  " + ", ".join("doom_palette_%d" % i
                                  for i in range(len(frames))) + "\n};\n\n")
        fh.write("#endif /* DOOM_FRAMES_H */\n")

    sys.stderr.write("png2chunky: wrote %s\n" % OUT)


if __name__ == "__main__":
    main()
