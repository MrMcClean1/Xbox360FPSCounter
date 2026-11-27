"""Converts a binary PPM (P6) to PNG, optionally cropped. Standard library only.

Usage: python tools/ppm2png.py in.ppm out.png [x y w h]
"""
import struct
import sys
import zlib


def read_ppm(path):
    data = open(path, 'rb').read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


def main():
    w, h, rgb = read_ppm(sys.argv[1])
    if len(sys.argv) == 7:
        x, y, cw, ch = (int(v) for v in sys.argv[3:7])
        rgb = b''.join(rgb[((y + r) * w + x) * 3:((y + r) * w + x + cw) * 3] for r in range(ch))
        w, h = cw, ch
    write_png(sys.argv[2], w, h, rgb)


if __name__ == '__main__':
    main()
