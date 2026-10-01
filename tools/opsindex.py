#!/usr/bin/env python3
"""Extract parameter operation registrations from a decompiled init function:
RegisterOperationType(op, name) and RegisterOperationFunction(&op, &res, &p1, &p2, fn), where each GUID is a
pair of stack variables assigned just before. Usage: tools/opsindex.py re/ParameterOperations.dll.c"""
import re, sys

def extract(path):
    names, funcs, env, genv, fenv = {}, [], {}, {}, {}
    text = open(path, encoding='latin1').read()
    for stmt in text.split(';'):
        stmt = ' '.join(stmt.split())
        m = re.search(r'(\w+) = (0x[0-9a-f]+|-?\d+)$', stmt)
        if m:
            env[m.group(1)] = int(m.group(2), 0) & 0xffffffff
            continue
        m = re.search(r'RegisterOperationType\(this,(0x[0-9a-f]+|-?\d+),(0x[0-9a-f]+|-?\d+),(s_\w+|&DAT_\w+)\)', stmt)
        if m:
            g = (int(m.group(1), 0) & 0xffffffff, int(m.group(2), 0) & 0xffffffff)
            names[g] = re.sub(r'_[0-9a-f]{8}$', '', m.group(3).lstrip('&').replace('s_', '', 1)).replace('_', ' ')
            continue
        m = re.search(r'(\w+) = \(CKGUID \*\)FUN_\w+\(&\w+,(0x[0-9a-f]+|-?\d+),(0x[0-9a-f]+|-?\d+)\)$', stmt)
        if m:
            genv[m.group(1)] = (int(m.group(2), 0) & 0xffffffff, int(m.group(3), 0) & 0xffffffff)
            continue
        m = re.search(r'(\w+) = \(?[\w *]*\)?(thunk_FUN_[0-9a-f]+|FUN_[0-9a-f]+|LAB_[0-9a-f]+)$', stmt)
        if m and 'RegisterOperation' not in stmt:
            fenv[m.group(1)] = m.group(2)
            continue
        m = re.search(r'RegisterOperationFunction\(this,(\w+),(\w+),(\w+),(\w+),(\w+)\)$', stmt)
        if m and all(v in genv for v in m.groups()[:4]):
            funcs.append(tuple(genv[v] for v in m.groups()[:4]) + (fenv.get(m.group(5), m.group(5)),))
            continue
        if 'RegisterOperationFunction' in stmt and '(this' in stmt:
            refs = re.findall(r'&(\w+)', stmt)
            f = re.search(r'(thunk_FUN_[0-9a-f]+|FUN_[0-9a-f]+|LAB_[0-9a-f]+)\)?\s*$', stmt)
            def guid(v):
                k = int(re.search(r'(\d+)$', v).group(1))
                pre = v[:-len(str(k))]
                return (env.get(v, 0), env.get(f'{pre}{k + 1}', 0))
            if len(refs) >= 4:
                funcs.append(tuple(guid(v) for v in refs[:4]) + (f.group(1) if f else '?',))
    return names, funcs

if __name__ == '__main__':
    names, funcs = extract(sys.argv[1])
    g = lambda x: f'{x[0]:08x}:{x[1]:08x}'
    for op, res, p1, p2, fn in funcs:
        print(f'{g(op)} res={g(res)} p1={g(p1)} p2={g(p2)} {fn} {names.get(op, "")}')
