#!/usr/bin/env python3
"""Compact view of an extracted Building Block (re/bb/<dll>/<name>.c): I/O and parameter layout from the
creation function (names resolved from the DLL), then the execute/callback bodies without local
declarations. Usage: tools/bbview.py DLL NAME [...]"""
import os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
import peread

DLLDIR = {'CK2': 'Bin'}

def dll_path(dll):
    for d in ('BuildingBlocks', 'Managers', 'Bin', 'RenderEngines', 'Plugins'):
        p = f'data/{d}/{dll}.dll'
        if os.path.exists(p):
            return p

def resolve(b, tok):
    m = re.match(r'(?:s_\w*?_|&DAT_|DAT_)([0-9a-f]{8})$', tok)
    if not m:
        return tok
    raw = peread.at(b, int(m.group(1), 16), 80)
    return repr(raw.split(b'\0')[0].decode('latin1')) if raw else tok

def view(dll, name):
    path = f're/bb/{dll}/{name}.c'
    text = open(path, encoding='latin1').read()
    b = open(dll_path(dll), 'rb').read()
    parts = re.split(r'^// ---- (\w+): (\S+) @ (\S+)$', text, flags=re.M)
    print(parts[0].strip())
    for i in range(1, len(parts), 4):
        role, fn, addr, body = parts[i:i + 4]
        if role == 'declaration':
            continue
        if role == 'creation':
            kinds = {'**(undefined4 **)this)': 'in', '+ 4))': 'out', '+ 0xc))': 'pin', '+ 0x14))': 'pout'}
            for line in body.split('\n'):
                line = line.strip()
                k = next((v for key, v in kinds.items() if key in line), None)
                if k and '(code' in line:
                    args = re.findall(r'\(([^()]*)\);?$', line)
                    toks = [t.strip() for t in (args[0].split(',') if args else [])]
                    toks = [resolve(b, t) for t in toks]
                    print(f'  {k:5s}', ' '.join(toks))
                elif 'DeclareSetting' in line or 'DeclareLocalParameter' in line or 'SetBehaviorFlags' in line or 'SetFlags' in line:
                    found = re.findall(r'\((.*)\);?', line)
                    toks = found[0].split(',') if found else []
                    print('  ', line.split('(')[0].split('::')[-1], ' '.join(resolve(b, t.strip()) for t in toks[1:]))
            continue
        print(f'  -- {role} {fn} @ {addr}')
        for line in body.split('\n'):
            s = line.rstrip()
            if (re.match(r'^\s+[A-Za-z_][\w *\[\]]*\s+\*?\w+(\s*\[\d+\])?;$', s) and not s.strip().startswith(('return', 'goto', 'break'))) or not s.strip():
                continue
            s = re.sub(r'(s_\w*?_[0-9a-f]{8}|&DAT_[0-9a-f]{8})', lambda m: resolve(b, m.group(1)), s)
            print('  ' + s)

if __name__ == '__main__':
    dll = sys.argv[1]
    for n in sys.argv[2:]:
        view(dll, n)
        print()
