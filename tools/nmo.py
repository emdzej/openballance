#!/usr/bin/env python3
"""Minimal Virtools (CK2) .nmo/.cmo header reader: object table + plugin deps."""
import struct, sys, zlib, collections

def read(path):
    b = open(path, 'rb').read()
    assert b[:8] == b'Nemo Fi\0', 'not a Virtools file'
    crc, ckver, filever, filever2, writemode, h1pack = struct.unpack_from('<6I', b, 8)
    dpack, dunpack, nman, nobj, maxid, prodver, prodbuild, h1unpack = struct.unpack_from('<8I', b, 32)
    off = 64
    h1 = b[off:off + h1pack]
    if h1pack != h1unpack: h1 = zlib.decompress(h1)
    data = b[off + h1pack: off + h1pack + dpack]
    if dpack != dunpack: data = zlib.decompress(data)
    objs = []; p = 0
    for _ in range(nobj):
        oid, cid, fidx, nlen = struct.unpack_from('<4I', h1, p); p += 16
        name = h1[p:p + nlen].rstrip(b'\0').decode('latin1'); p += nlen
        objs.append((oid, cid, name))
    ncat, = struct.unpack_from('<I', h1, p); p += 4
    deps = []
    for _ in range(ncat):
        cat, cnt = struct.unpack_from('<2I', h1, p); p += 8
        for _ in range(cnt):
            deps.append((cat, struct.unpack_from('<2I', h1, p))); p += 8
    return dict(ckver=ckver, filever=filever, writemode=writemode, nman=nman, objs=objs, deps=deps,
                data_size=len(data), prodbuild=prodbuild)

if __name__ == '__main__':
    f = read(sys.argv[1])
    print(f"ck=0x{f['ckver']:x} filever={f['filever']} mode=0x{f['writemode']:x} objs={len(f['objs'])} data={f['data_size']}")
    h = collections.Counter(c for _, c, _ in f['objs'])
    print('classes:', sorted(h.items()))
    if len(sys.argv) > 2:
        want = int(sys.argv[2])
        print(collections.Counter(n for _, c, n in f['objs'] if c == want).most_common(int(sys.argv[3]) if len(sys.argv)>3 else 60))
