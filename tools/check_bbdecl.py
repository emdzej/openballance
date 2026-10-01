#!/usr/bin/env python3
"""Check every BB_DECL(var, a, b, "Name", fn) in src/bb against re/bb_map.txt (GUID <-> name)."""
import glob, re, sys
m = {}
for line in open('re/bb_map.txt'):
    parts = line.split(None, 5)
    m[parts[0]] = parts[5].strip()
bad = 0
for path in glob.glob('src/bb/*.c'):
    for a, b, name in re.findall(r'BB_DECL(?:_CB)?\(\w+,\s*([0-9a-f]{8}),\s*([0-9a-f]{8}),\s*"([^"]+)"', open(path).read()):
        g = f'{a}:{b}'
        if m.get(g) != name:
            print(f'{path}: {name} {g} -> map says {m.get(g)!r}')
            bad += 1
print('ok' if not bad else f'{bad} mismatches')
sys.exit(1 if bad else 0)
