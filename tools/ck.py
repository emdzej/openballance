#!/usr/bin/env python3
"""Virtools (CK2 2.1, file version 8) reader and behavior-graph dumper.

    tools/ck.py summary FILE             object/class histogram
    tools/ck.py graph FILE [NAME...]     dump behavior graphs (top-level scripts, or the named ones)
    tools/ck.py obj FILE INDEX           raw chunk words of one object

File layout: 64-byte header, header1 (object table, plugin deps), data (manager chunks, then one
chunk per object). Chunk = u32 version (dataVer|classId<<8|chunkVer<<16|options<<24), u32 dword
count, payload, then optional ID / sub-chunk / manager lists (options 1/4/2). Inside the payload,
fields are grouped under identifiers forming a linked list: [id, next index, fields...].
With option 8 (FILE) object references are indices into the file's object table.

Behavior layout from CKBehavior::Save (CK2.dll FUN_24004e1e).
"""
import struct, sys, zlib, collections

CLASS = {1: 'Object', 2: 'ParameterIn', 3: 'ParameterOut', 4: 'ParameterOperation', 6: 'BehaviorLink',
         8: 'Behavior', 9: 'BehaviorIO', 10: 'Scene', 11: 'SceneObject', 19: 'BeObject', 21: 'Level',
         23: 'Group', 24: 'Sound', 25: 'WaveSound', 27: '2dEntity', 28: 'Sprite', 29: 'SpriteText',
         30: 'Material', 31: 'Texture', 32: 'Mesh', 33: '3dEntity', 34: 'Camera', 35: 'TargetCamera',
         38: 'Light', 39: 'TargetLight', 41: '3dObject', 43: 'Curve', 45: 'ParameterLocal', 46: 'Parameter',
         52: 'DataArray'}

# Behavior flag and save-mask bits (CKBehavior::Save)
BF_PROTO, BF_PRIORITY, BF_CLASS, BF_TARGET = 0x8000, 0x4, 0x10, 0x40000
ARRAYS = [(0x100, 'sub'), (0x80000, 'links'), (0x4000, 'ops'), (0x200, 'pin'), (0x400, 'pout'),
          (0x20000, 'local'), (0x800, 'in'), (0x1000, 'out')]


class Chunk:
    def __init__(self, raw):
        v, dw = struct.unpack_from('<2I', raw, 0)
        self.data_version, self.class_id = v & 0xff, (v >> 8) & 0xff
        self.chunk_version, self.options = (v >> 16) & 0xff, v >> 24
        self.w = list(struct.unpack_from(f'<{dw}I', raw, 8))
        q = 8 + dw * 4
        self.ids = self.subchunks = self.managers = ()
        for bit, attr in ((1, 'ids'), (4, 'subchunks'), (2, 'managers')):
            if self.options & bit:
                n, = struct.unpack_from('<I', raw, q)
                setattr(self, attr, struct.unpack_from(f'<{n}I', raw, q + 4))
                q += 4 + 4 * n
        assert q == len(raw), 'chunk size mismatch'

    def idents(self):
        """{identifier: payload words}"""
        out, i, w = {}, 0, self.w
        while i + 1 < len(w):
            nxt = w[i + 1]
            out[w[i]] = w[i + 2: nxt if nxt else len(w)]
            if not nxt:
                break
            i = nxt
        return out


class Obj:
    def __init__(self, index, oid, cid, name, chunk):
        self.index, self.oid, self.cid, self.name, self.chunk = index, oid, cid, name, chunk

    @property
    def cls(self):
        return CLASS.get(self.cid, f'cid{self.cid}')


class File:
    def __init__(self, path):
        b = open(path, 'rb').read()
        assert b[:8] == b'Nemo Fi\0', 'not a Virtools file'
        (self.crc, self.ck_version, self.file_version, _, self.write_mode, h1pack) = struct.unpack_from('<6I', b, 8)
        dpack, dunpack, nman, nobj, _, _, _, h1unpack = struct.unpack_from('<8I', b, 32)
        h1 = b[64:64 + h1pack]
        if h1pack != h1unpack:
            h1 = zlib.decompress(h1)
        d = b[64 + h1pack: 64 + h1pack + dpack]
        if dpack != dunpack:
            d = zlib.decompress(d)
        table, p = [], 0
        for _ in range(nobj):
            oid, cid, _, nlen = struct.unpack_from('<4I', h1, p); p += 16
            table.append((oid, cid, h1[p:p + nlen].rstrip(b'\0').decode('latin1'))); p += nlen
        self.managers, p = [], 0
        for _ in range(nman):
            g0, g1, sz = struct.unpack_from('<3I', d, p); p += 12
            self.managers.append(((g0, g1), Chunk(d[p:p + sz]) if sz else None)); p += sz
        self.objs = []
        for i, (oid, cid, name) in enumerate(table):
            sz, = struct.unpack_from('<I', d, p); p += 4
            self.objs.append(Obj(i, oid, cid, name, Chunk(d[p:p + sz]) if sz else None)); p += sz
        assert p == len(d), 'data section not fully consumed'

    def ref(self, x):
        return self.objs[x] if x < len(self.objs) else None


# ---- per-class decoders -----------------------------------------------------------------------

def behavior(f, o):
    w = o.chunk.idents()[0x20]
    b = {'flags': w[0]}
    k = 1
    if w[0] & BF_PROTO:
        b['proto'] = (w[k], w[k + 1]); b['proto_version'] = w[k + 2]; k += 3
    if w[0] & BF_PRIORITY:
        b['priority'] = struct.unpack('<i', struct.pack('<I', w[k]))[0]; k += 1
    if w[0] & BF_CLASS:
        b['compat_class'] = w[k]; k += 1
    if w[0] & BF_TARGET:
        b['target'] = w[k]; k += 1
    mask = w[k]; k += 1
    for bit, key in ARRAYS:
        b[key] = []
        if mask & bit:
            n = w[k]; b[key] = list(w[k + 1:k + 1 + n]); k += 1 + n
    assert k == len(w), f'behavior {o.index} {o.name}: {len(w) - k} words left'
    return b


def io_flags(o):
    return o.chunk.idents().get(8, [0])[0]


def link(o):
    w = o.chunk.idents()[0x20]
    return {'delay': w[0], 'src': w[1], 'dst': w[2]}


# Parameter type GUIDs the loader treats specially (CKParameter load, CK2.dll FUN_2400867c).
# Legacy GUIDs are remapped to current ones before the type lookup.
GUID_REMAP = {(0x213661cc, 0x7d1a2d54): (0x03881e12, 0x5ba34e2b), (0x13b97e4c, 0x982e8b4f): (0x3ea34ee9, 0x09fa5366),
              (0x71653557, 0x2d1b2e97): (0x30ec20ab, 0x6df6517d), (0x4a4d4867, 0x3c28773f): (0x54b4422b, 0x730f0f4f)}
PG_MESSAGE_OLD, PG_ATTRIBUTE_OLD, PG_PARAMTYPE = (0x213661cc, 0x7d1a2d54), (0x13b97e4c, 0x982e8b4f), (0x34517df5, 0x045e4965)


def param_value(o):
    """ParameterOut / ParameterLocal: (type guid, value). Value modes: 0 = type-specific sub-chunk,
    1 = buffer (u32 byte size + data), 2 = object reference, 3 = none, other = skip one dword, value in the next."""
    w = o.chunk.idents().get(0x40)
    if w is None:
        return None, None
    stored = (w[0], w[1])
    guid = GUID_REMAP.get(stored, stored)
    mode = w[2]
    if mode == 0:
        return guid, ('subchunk',)
    if mode == 1:
        if stored == PG_PARAMTYPE:
            return guid, ('paramtype', (w[3], w[4]))
        size = w[3]
        raw = struct.pack(f'<{len(w) - 4}I', *w[4:])[:size]
        if stored in (PG_MESSAGE_OLD, PG_ATTRIBUTE_OLD):   # stored by name
            return guid, ('name', raw.split(b'\0')[0].decode('latin1'))
        return guid, raw
    if mode == 2:
        return guid, ('obj', w[3])
    if mode == 3:
        return guid, ('none',)
    return guid, ('int', w[4])


def param_in(o):
    ids = o.chunk.idents()
    for ident, kind in ((0x1000, 'direct'), (0x2000, 'shared'), (0x800, 'direct'), (0x4000, 'shared')):
        if ident in ids:
            w = ids[ident]
            return (w[0], w[1]), kind, (w[2] if len(w) > 2 else None)
    return None, '?', None


def param_op(o):
    w = o.chunk.idents()[0x400]
    return {'op': (w[0], w[1]), 'args': list(w[3:3 + w[2]])}


# ---- geometry ---------------------------------------------------------------------------------

def _floats(ws):
    return struct.unpack(f'<{len(ws)}f', struct.pack(f'<{len(ws)}I', *ws))


def argb(c):
    """Packed ARGB dword -> (r, g, b, a) floats in 0..1."""
    return ((c >> 16 & 0xff) / 255, (c >> 8 & 0xff) / 255, (c & 0xff) / 255, (c >> 24) / 255)


def entity3d(o):
    """CK3dEntity (CK2_3D.dll FUN_1000a7b9, current format under 0x100000): world matrix and meshes."""
    ids = o.chunk.idents()
    e = {'meshes': [], 'mesh': None, 'matrix': None}
    w = ids.get(0x100000)
    if w:
        e['flags'], e['moveable'] = w[0], w[1]
        rows = _floats(w[2:14])          # X, Y, Z axes and position (row vectors, D3D convention)
        e['matrix'] = [rows[0:3] + (0.0,), rows[3:6] + (0.0,), rows[6:9] + (0.0,), rows[9:12] + (1.0,)]
    w = ids.get(0x4000)
    if w:
        e['mesh'] = w[0]
        e['meshes'] = list(w[2:2 + w[1]])
    return e


def mesh(o):
    """CKMesh, data version >= 9 (CK2_3D.dll FUN_1002816a, vertices FUN_10027e1e)."""
    ids = o.chunk.idents()
    m = {'flags': ids.get(0x2000, [0])[0], 'materials': [], 'pos': [], 'normal': [], 'uv': [],
         'color': [], 'specular': [], 'faces': []}
    w = ids.get(0x100000)
    if w:
        n = w[0]
        m['materials'] = [w[1 + 2 * i] for i in range(n)]      # (material, unused int) pairs
    w = ids.get(0x20000)
    if w and w[0]:
        n, save = w[0], w[1]
        k = 3                                                 # w[2] = buffer size in dwords
        if not save & 0x10:
            p = _floats(w[k:k + 3 * n]); k += 3 * n
            m['pos'] = [p[i:i + 3] for i in range(0, 3 * n, 3)]
        if not save & 1:
            m['color'] = list(w[k:k + n]); k += n
        else:
            m['color'] = [w[k]] * n; k += 1
        if not save & 2:
            m['specular'] = list(w[k:k + n]); k += n
        else:
            m['specular'] = [w[k]] * n; k += 1
        if not save & 4:
            p = _floats(w[k:k + 3 * n]); k += 3 * n
            m['normal'] = [p[i:i + 3] for i in range(0, 3 * n, 3)]
        if not save & 8:
            p = _floats(w[k:k + 2 * n]); k += 2 * n
            m['uv'] = [p[i:i + 2] for i in range(0, 2 * n, 2)]
        else:
            m['uv'] = [_floats(w[k:k + 2])] * n; k += 2
    w = ids.get(0x10000)
    if w:
        n = w[0]
        for i in range(n):
            a, b = w[1 + 2 * i], w[2 + 2 * i]                 # (i0 | i1 << 16), (i2 | material << 16)
            m['faces'].append((a & 0xffff, a >> 16, b & 0xffff, b >> 16))
    return m


# Material packed fields (CK2_3D.dll FUN_100655e1, data version >= 5)
BLEND = {1: 'zero', 2: 'one', 3: 'src-color', 4: 'one-minus-src-color', 5: 'src-alpha', 6: 'one-minus-src-alpha',
         7: 'dst-alpha', 8: 'one-minus-dst-alpha', 9: 'dst-color', 10: 'one-minus-dst-color', 11: 'src-alpha-saturated'}


def material(o):
    w = o.chunk.idents()[0x1000]
    modes, flags = w[7], w[8]
    return {'diffuse': argb(w[0]), 'ambient': argb(w[1]), 'specular': argb(w[2]), 'emissive': argb(w[3]),
            'power': _floats([w[4]])[0], 'texture': w[5], 'blend_color': w[6],
            'texture_blend': modes & 0xf, 'min_filter': modes >> 4 & 0xf, 'mag_filter': modes >> 8 & 0xf,
            'src_blend': modes >> 12 & 0xf, 'dst_blend': modes >> 16 & 0xf, 'fill': modes >> 20 & 0xf,
            'shade': modes >> 24 & 0xf, 'address': modes >> 28, 'flags': flags & 0xff,
            'alpha_func': flags >> 8 & 0xf, 'z_func': flags >> 16 & 0xf, 'alpha_ref': flags >> 24}


def texture(o):
    """CKTexture: slot file names under 0x10000 (count, then length-prefixed strings)."""
    ids = o.chunk.idents()
    w = ids.get(0x10000)
    files = []
    if w:
        raw = struct.pack(f'<{len(w)}I', *w)
        n, = struct.unpack_from('<I', raw, 0); p = 4
        for _ in range(n):
            ln, = struct.unpack_from('<I', raw, p); p += 4
            files.append(raw[p:p + ln].rstrip(b'\0').decode('latin1')); p += (ln + 3) & ~3
    return {'files': files}


def group(o):
    """CKGroup members (CKBeObject-derived, members under 0xfffff? observed: count + object indices)."""
    w = o.chunk.idents().get(0xfffff, [])
    return list(w[1:1 + w[0]]) if w else []


# ---- dumping ----------------------------------------------------------------------------------

def fmt_value(v):
    if isinstance(v, bytes):
        if len(v) == 4:
            i, fl = struct.unpack('<i', v)[0], struct.unpack('<f', v)[0]
            return f'{i}' if abs(i) < 1 << 20 else f'{fl:g}'
        s = v.split(b'\0')[0]
        if s and all(32 <= c < 127 for c in s) and len(s) >= len(v) - 2:
            return repr(s.decode())
        return v.hex() if len(v) <= 16 else f'<{len(v)} bytes>'
    if isinstance(v, tuple) and v:
        if v[0] == 'obj':
            return f'-> #{v[1]}' if v[1] != 0xffffffff else '-> none'
        if v[0] == 'int':
            return f'#{v[1]}'
        if v[0] == 'name':
            return repr(v[1])
        if v[0] == 'paramtype':
            return f'type {v[1][0]:08x}:{v[1][1]:08x}'
        return f'<{v[0]}>'
    return str(v)


def dump_graph(f, o, depth=0, out=print):
    ind = '  ' * depth
    b = behavior(f, o)
    head = f"{ind}{'BB' if 'proto' in b else 'GRAPH'} #{o.index} {o.name!r}"
    if 'proto' in b:
        head += f" proto={b['proto'][0]:08x}:{b['proto'][1]:08x}"
    if b.get('priority'):
        head += f" prio={b['priority']}"
    if 'target' in b and b['target'] != 0xffffffff:
        head += f" target=#{b['target']}"
    out(head)
    name = lambda x: (f.ref(x).name or f'#{x}') if f.ref(x) else f'#{x}'
    if b['in'] or b['out']:
        out(f"{ind}  io: in[{', '.join(name(x) for x in b['in'])}] out[{', '.join(name(x) for x in b['out'])}]")
    for x in b['pin']:
        g, kind, src = param_in(f.ref(x))
        s = f.ref(src) if src is not None else None
        out(f"{ind}  pin  {name(x)!r} <- {kind} {('#%d %s %r' % (src, s.cls, s.name)) if s else src}")
    for x in b['pout']:
        _, v = param_value(f.ref(x))
        out(f"{ind}  pout {name(x)!r} = {fmt_value(v)}")
    for x in b['local']:
        _, v = param_value(f.ref(x))
        out(f"{ind}  local #{x} {name(x)!r} = {fmt_value(v)}")
    for x in b['ops']:
        p = param_op(f.ref(x))
        out(f"{ind}  op #{x} {name(x)!r} {p['op'][0]:08x}:{p['op'][1]:08x} args={['#%d' % a for a in p['args']]}")
    owner = {}
    for s in b['sub']:
        sb = behavior(f, f.ref(s))
        for io in sb['in'] + sb['out']:
            owner[io] = s
    for io in b['in'] + b['out']:
        owner[io] = o.index
    for x in b['links']:
        l = link(f.ref(x))
        end = lambda io: f"{name(owner[io]) if io in owner else '?'}.{name(io)}"
        d = l['delay']   # two 16-bit halves (activation delay / initial delay, order not yet confirmed)
        dtxt = '' if not d else f" delay={d & 0xffff}" if d >> 16 == d & 0xffff else f" delay={d & 0xffff}/{d >> 16}"
        out(f"{ind}  link {end(l['src'])} -> {end(l['dst'])}{dtxt}")
    for s in b['sub']:
        dump_graph(f, f.ref(s), depth + 1, out)


def top_level(f):
    sub = set()
    for o in f.objs:
        if o.cid == 8:
            sub.update(behavior(f, o)['sub'])
    return [o for o in f.objs if o.cid == 8 and o.index not in sub]


def main(argv):
    cmd, path = argv[1], argv[2]
    f = File(path)
    if cmd == 'summary':
        print(f'ck=0x{f.ck_version:08x} file_version={f.file_version} objects={len(f.objs)}')
        for cid, n in sorted(collections.Counter(o.cid for o in f.objs).items()):
            print(f'  {CLASS.get(cid, cid):20s} {n}')
    elif cmd == 'graph':
        names = set(argv[3:])
        roots = [o for o in f.objs if o.cid == 8 and o.name in names] if names else top_level(f)
        for o in roots:
            dump_graph(f, o)
            print()
    elif cmd == 'obj':
        o = f.objs[int(argv[3])]
        print(o.index, o.cls, repr(o.name), {hex(k): [f'{x:x}' for x in v] for k, v in o.chunk.idents().items()})


if __name__ == '__main__':
    main(sys.argv)
