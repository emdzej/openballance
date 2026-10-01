#!/usr/bin/env python3
"""Read bytes/strings at virtual addresses of a PE file: tools/peread.py FILE VA [VA...]"""
import struct, sys

def sections(b):
    pe, = struct.unpack_from('<I', b, 0x3c)
    ns, = struct.unpack_from('<H', b, pe + 6)
    so, = struct.unpack_from('<H', b, pe + 20)
    base, = struct.unpack_from('<I', b, pe + 52)
    out = []
    for i in range(ns):
        o = pe + 24 + so + i * 40
        vs, va, rs, ro = struct.unpack_from('<4I', b, o + 8)
        out.append((base + va, max(vs, rs), ro))
    return out

def at(b, va, n=64):
    for sva, size, ro in sections(b):
        if sva <= va < sva + size:
            return b[ro + va - sva: ro + va - sva + n]
    return None

if __name__ == '__main__':
    b = open(sys.argv[1], 'rb').read()
    for a in sys.argv[2:]:
        raw = at(b, int(a, 16))
        print(a, repr(raw.split(b'\0')[0]) if raw else None)
