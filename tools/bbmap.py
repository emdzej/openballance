#!/usr/bin/env python3
"""re/bb_map.txt: every prototype the game uses (re/bb_inventory.txt) -> DLL, execute and callback
function (from the BB DLL dumps, tools/bbindex.py)."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
import bbindex

DLLS = ['Logics', 'TT_Toolbox_RT', 'TT_ParticleSystems_RT', 'Narratives', 'Visuals', 'Interface', 'Controllers',
        'Sounds', '3DTransfo', 'Materials', 'TT_DatabaseManager_RT', 'TT_InterfaceManager_RT', 'BuildingBlocksAddons1',
        'Lights', 'WorldEnvironments', 'Cameras', 'Collisions', 'physics_RT', 'TT_Gravity_RT']
where = {}
for d in DLLS:
    rows, _ = bbindex.index(f're/{d}.dll.c')
    for label, g, decl, create, fn, cb in rows:
        where.setdefault(g, (d, fn, cb))
miss = 0
with open('re/bb_map.txt', 'w') as out:
    for line in open('re/bb_inventory.txt'):
        if line.startswith('#'):
            continue
        parts = line.split()
        g = tuple(int(x, 16) for x in parts[0].split(':'))
        name = ' '.join(parts[2:]).split('(')[0]
        d = where.get(g)
        miss += not d or not d[1]
        out.write(f"{parts[0]} {int(parts[1]):5d} {d[0] if d else '?':24s} fn={d[1] if d else '-'} cb={d[2] if d else '-'} {name}\n")
print('prototypes without execute function:', miss)
