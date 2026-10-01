#!/usr/bin/env python3
"""Write each used Building Block's decompiled functions (declaration, creation = I/O and parameter
layout, execute, callback) to re/bb/<dll>/<name>.c, from re/bb_map.txt and re/<dll>.dll.c."""
import os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
import bbindex

def main():
    rows = [l.split(None, 5) for l in open('re/bb_map.txt')]
    cache = {}
    for guid, uses, dll, fn, cb, name in rows:
        name = name.strip()
        if dll not in cache:
            text = open(f're/{dll}.dll.c', encoding='latin1').read()
            cache[dll] = (bbindex.funcs(text), bbindex.index(f're/{dll}.dll.c')[0])
        fs, idx = cache[dll]
        by_addr = {a: (n, b) for n, (a, b) in fs.items()}
        g = tuple(int(x, 16) for x in guid.split(':'))
        entry = next(r for r in idx if r[1] == g)
        os.makedirs(f're/bb/{dll}', exist_ok=True)
        safe = re.sub(r'[^A-Za-z0-9_.-]+', '_', name)
        with open(f're/bb/{dll}/{safe}.c', 'w') as out:
            out.write(f'// {name}  guid {guid}  uses {uses}  dll {dll}\n')
            by_name = {n: (a, b) for n, (a, b) in fs.items()}
            for role, addr in (('declaration', entry[2]), ('creation', entry[3]), ('execute', entry[4]), ('callback', entry[5])):
                if addr and addr in by_addr:
                    n, body = by_addr[addr]
                    # follow jump thunks (thunk_FUN_x -> FUN_x)
                    for _ in range(3):
                        if not n.startswith('thunk_'):
                            break
                        target = n[len('thunk_'):]
                        if target not in by_name:
                            break
                        addr, body = by_name[target]
                        n = target
                    out.write(f'\n// ---- {role}: {n} @ {addr}\n{body.strip()}\n')

main()
