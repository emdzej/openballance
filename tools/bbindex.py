#!/usr/bin/env python3
"""Index the Building Blocks of a decompiled BB DLL (re/<dll>.c): name, GUID, creation function and,
once the creation functions are decompiled, the execute and callback functions.

    tools/bbindex.py re/Logics.dll.c            table
    tools/bbindex.py re/Logics.dll.c --labels   addresses Ghidra still has to turn into functions
"""
import re, sys

def funcs(text):
    parts = re.split(r'^// ==== (\S+) @ ([0-9a-f]+)$', text, flags=re.M)
    return {parts[i]: (parts[i + 1], parts[i + 2]) for i in range(1, len(parts) - 2, 3)}

def index(path):
    text = open(path, encoding='latin1').read()
    fs = funcs(text)
    by_addr = {addr: body for name, (addr, body) in fs.items()}
    out = []
    for name, (addr, body) in fs.items():
        m = re.search(r'CreateCKObjectDeclaration\((s_\w+|&DAT_\w+|"[^"]*")\)', body)
        g = re.search(r'SetGuid\(this,(0x[0-9a-f]+|-?\d+),(0x[0-9a-f]+|-?\d+)\)', body)
        c = re.search(r'SetCreationFunction\s*\(this,(?:\(\w+ \*\))?(?:&LAB_|thunk_FUN_|FUN_)([0-9a-f]+)\)', body)
        if not (m and g):
            continue
        guid = tuple(int(x, 0) & 0xffffffff for x in g.groups())
        create = c.group(1) if c else None
        fn = cb = None
        cbody = by_addr.get(create)
        if cbody:
            f = re.search(r'SetFunction\([^,]+,(?:\(\w+ \*\))?(?:&LAB_|thunk_FUN_|FUN_)([0-9a-f]+)\)', cbody)
            k = re.search(r'SetBehaviorCallbackFct\s*\([^,]+,(?:\(\w+ \*\))?(?:&LAB_|thunk_FUN_|FUN_)([0-9a-f]+)', cbody)
            fn = f.group(1) if f else None
            cb = k.group(1) if k else None
        label = re.sub(r'_[0-9a-f]{8}$', '', m.group(1)[2:]).replace('_', ' ')
        out.append((label, guid, addr, create, fn, cb))
    return out, set(by_addr)

if __name__ == '__main__':
    rows, known = index(sys.argv[1])
    if '--labels' in sys.argv:
        for r in rows:
            for a in r[3:]:
                if a and a not in known:
                    print(a)
    else:
        for label, g, decl, create, fn, cb in sorted(rows):
            print(f'{g[0]:08x}:{g[1]:08x}  {label:32s} decl={decl} create={create} fn={fn} cb={cb}')
