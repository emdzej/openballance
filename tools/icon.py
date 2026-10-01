#!/usr/bin/env python3
"""Scale the site's favicon (docs/public/favicon.png, the Ballance ball) to an app icon.

Smooth scaling (area average when shrinking, bilinear when enlarging, on premultiplied alpha), so the
rendered ball stays round and soft. Standard library only, so it runs on CI runners without Pillow.
Reads 8-bit non-interlaced RGBA/RGB PNGs, writes RGBA.

    tools/icon.py <size> <out.png> [in.png]
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit(f'{path}: not a PNG')
    pos, idat, hdr = 8, b'', None
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            hdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            idat += body
    w, h, depth, ctype, _, _, interlace = hdr
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise SystemExit(f'{path}: need an 8-bit non-interlaced RGB(A) PNG')
    bpp = 4 if ctype == 6 else 3
    raw, stride, rows, prev = zlib.decompress(idat), w * bpp, [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line if bpp == 4 else bytearray(
            sum(([line[x * 3], line[x * 3 + 1], line[x * 3 + 2], 255] for x in range(w)), [])))
        prev = line
    return w, h, rows


def write_png(path, w, h, rows):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def scale(w, h, rows, size):
    """Premultiplied RGBA float planes -> size x size bytes rows."""
    px = [[(r[x * 4] * r[x * 4 + 3] / 255.0, r[x * 4 + 1] * r[x * 4 + 3] / 255.0,
            r[x * 4 + 2] * r[x * 4 + 3] / 255.0, float(r[x * 4 + 3])) for x in range(w)] for r in rows]

    def sample(sx0, sx1, sy0, sy1):
        if sx1 - sx0 <= 1 and sy1 - sy0 <= 1:   # enlarging: bilinear at the centre
            cx, cy = (sx0 + sx1) / 2 - 0.5, (sy0 + sy1) / 2 - 0.5
            x0, y0 = max(0, min(w - 1, int(cx // 1))), max(0, min(h - 1, int(cy // 1)))
            x1, y1 = min(w - 1, x0 + 1), min(h - 1, y0 + 1)
            fx, fy = max(0.0, min(1.0, cx - x0)), max(0.0, min(1.0, cy - y0))
            a, b, c, d = px[y0][x0], px[y0][x1], px[y1][x0], px[y1][x1]
            return [(a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy for k in range(4)]
        acc, n = [0.0] * 4, 0
        for y in range(int(sy0), max(int(sy0) + 1, int(sy1))):
            for x in range(int(sx0), max(int(sx0) + 1, int(sx1))):
                p = px[min(y, h - 1)][min(x, w - 1)]
                for k in range(4):
                    acc[k] += p[k]
                n += 1
        return [v / n for v in acc]

    out = []
    for y in range(size):
        line = bytearray()
        for x in range(size):
            r, g, b, a = sample(x * w / size, (x + 1) * w / size, y * h / size, (y + 1) * h / size)
            u = 255.0 / a if a > 0 else 0
            line += bytes((min(255, round(r * u)), min(255, round(g * u)), min(255, round(b * u)), min(255, round(a))))
        out.append(line)
    return out


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    size, out = int(sys.argv[1]), sys.argv[2]
    src = sys.argv[3] if len(sys.argv) > 3 else 'docs/public/favicon.png'
    w, h, rows = read_png(src)
    write_png(out, size, size, scale(w, h, rows, size))


if __name__ == '__main__':
    main()
