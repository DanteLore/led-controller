#!/usr/bin/env python3
"""Converts an 8-bit RGB or RGBA PNG to raw RGB565, big-endian as the ST7789 takes it.

Usage: png_to_rgb565.py in.png out.rgb565
Standard library only: PNG is zlib-compressed rows, each with a filter byte.
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos, idat = 8, b''
    while pos < len(data):
        length, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height, depth, colour, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and colour in (2, 6) and not interlace, 'needs 8-bit RGB/RGBA, not interlaced'
        elif kind == b'IDAT':
            idat += body
        pos += 12 + length
    bpp = 3 if colour == 2 else 4
    raw, stride, rows, prev = zlib.decompress(idat), width * bpp, [], bytearray(width * bpp)
    for y in range(height):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 0xFF
            elif f == 2: line[i] = (line[i] + b) & 0xFF
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 0xFF
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 0xFF
        rows.append(line)
        prev = line
    return width, height, bpp, rows


def main():
    width, height, bpp, rows = read_png(sys.argv[1])
    out = bytearray()
    for line in rows:
        for x in range(width):
            r, g, b = line[x * bpp:x * bpp + 3]
            out += struct.pack('>H', ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    open(sys.argv[2], 'wb').write(out)
    print(f'{sys.argv[2]}: {width} x {height}, {len(out)} bytes')


main()
