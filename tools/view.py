#!/usr/bin/env python3
"""Reference renderer: draw a Ballance level (.NMO) to a PNG, textured, z-buffered.

    tools/.venv/bin/python tools/view.py LEVEL.NMO out.png [--textures DIR] [--size 1024x768]
                                         [--eye x,y,z --at x,y,z] [--fov 60]

Checks the geometry decoders in ck.py (meshes, 3D entities, materials, textures). Not the game's
renderer: no lights, fog or blending beyond alpha-tested cut-outs; faces get a fixed directional shade.
Default camera looks at the whole level from above at 45 degrees.
"""
import argparse, os, sys
import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import ck

SKIP_GROUPS = {'DepthTestCubes'}   # 'Shadow' = objects that receive the ball shadow (floors), keep them
SKIP_NAMES = {'SkyLayer'}   # the cloud plane under the level
FRAME_GROUP = 'Phys_Floors'  # default camera frames this group


def load_textures(f, tex_dir):
    cache = {}
    for o in f.objs:
        if o.cid != 31:
            continue
        files = ck.texture(o)['files']
        img = None
        if files:
            path = os.path.join(tex_dir, files[0])
            if not os.path.exists(path):   # case-insensitive fallback
                low = {n.lower(): n for n in os.listdir(tex_dir)}
                path = os.path.join(tex_dir, low.get(files[0].lower(), files[0]))
            if os.path.exists(path):
                img = np.asarray(Image.open(path).convert('RGBA'), dtype=np.float32) / 255
        cache[o.index] = img
    return cache


def look_at(eye, at, up=(0, 1, 0)):
    """D3D left-handed view matrix (row vectors)."""
    eye, at, up = map(np.asarray, (eye, at, up))
    z = at - eye; z = z / np.linalg.norm(z)
    x = np.cross(up, z); x = x / np.linalg.norm(x)
    y = np.cross(z, x)
    m = np.identity(4)
    m[:3, 0], m[:3, 1], m[:3, 2] = x, y, z
    m[3, :3] = -eye @ np.stack([x, y, z], axis=1)
    return m


def collect(f):
    skip = set()
    for o in f.objs:
        if o.cid == 23 and o.name in SKIP_GROUPS:
            skip.update(ck.group(o))
    tris = []   # per entity: world positions, uvs, faces, materials
    for o in f.objs:
        if o.cid != 41 or o.index in skip or o.name in SKIP_NAMES:
            continue
        e = ck.entity3d(o)
        if e['mesh'] is None or e['matrix'] is None:
            continue
        m = ck.mesh(f.ref(e['mesh']))
        if not m['pos'] or not m['faces']:
            continue
        p = np.asarray(m['pos'], dtype=np.float64)
        world = np.c_[p, np.ones(len(p))] @ np.asarray(e['matrix'])
        tris.append((world[:, :3], np.asarray(m['uv'] or [(0, 0)] * len(p)), m['faces'], m['materials'], o.index))
    return tris


def render(f, tex_dir, size, eye=None, at=None, fov=60):
    w, h = size
    textures = load_textures(f, tex_dir)
    mats = {o.index: ck.material(o) for o in f.objs if o.cid == 30}
    tris = collect(f)
    frame = set()
    for o in f.objs:
        if o.cid == 23 and o.name == FRAME_GROUP:
            frame.update(ck.group(o))
    allp = np.concatenate([t[0] for t in tris if not frame or t[4] in frame])
    lo, hi = allp.min(0), allp.max(0)
    if at is None:
        at = (lo + hi) / 2
    if eye is None:
        r = np.linalg.norm(hi - lo) * 0.75
        eye = at + np.array([-0.5, 1.0, -0.5]) / np.sqrt(1.5) * r
    view = look_at(eye, at)
    f_ = 1 / np.tan(np.radians(fov) / 2)
    color = np.zeros((h, w, 3), np.float32) + np.array([0.18, 0.2, 0.3], np.float32)
    depth = np.full((h, w), np.inf, np.float32)
    light = np.array([0.4, 0.8, -0.3]); light /= np.linalg.norm(light)
    ys, xs = np.mgrid[0:h, 0:w]
    for pos, uv, faces, mlist, _ in tris:
        v = np.c_[pos, np.ones(len(pos))] @ view
        z = v[:, 2]
        sx = (v[:, 0] * f_ / np.maximum(z, 1e-6) * (h / 2)) + w / 2
        sy = (-v[:, 1] * f_ / np.maximum(z, 1e-6) * (h / 2)) + h / 2
        for i0, i1, i2, mi in faces:
            if min(z[i0], z[i1], z[i2]) < 0.5:
                continue
            x0, x1, x2 = sx[i0], sx[i1], sx[i2]
            y0, y1, y2 = sy[i0], sy[i1], sy[i2]
            minx, maxx = int(max(0, np.floor(min(x0, x1, x2)))), int(min(w - 1, np.ceil(max(x0, x1, x2))))
            miny, maxy = int(max(0, np.floor(min(y0, y1, y2)))), int(min(h - 1, np.ceil(max(y0, y1, y2))))
            if minx > maxx or miny > maxy:
                continue
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(area) < 1e-9:
                continue
            px, py = xs[miny:maxy + 1, minx:maxx + 1] + 0.5, ys[miny:maxy + 1, minx:maxx + 1] + 0.5
            b0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) / area
            b1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) / area
            b2 = 1 - b0 - b1
            inside = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
            if not inside.any():
                continue
            iz = b0 / z[i0] + b1 / z[i1] + b2 / z[i2]          # perspective-correct
            zz = 1 / iz
            dview = depth[miny:maxy + 1, minx:maxx + 1]
            ok = inside & (zz < dview)
            if not ok.any():
                continue
            mat = mats.get(mlist[mi]) if mi < len(mlist) else None
            n = np.cross(pos[i1] - pos[i0], pos[i2] - pos[i0])
            nl = np.linalg.norm(n)
            shade = 0.45 + 0.55 * abs(np.dot(n / nl, light)) if nl else 1.0
            tex = textures.get(mat['texture']) if mat else None
            if tex is not None:
                u = (b0 * uv[i0][0] / z[i0] + b1 * uv[i1][0] / z[i1] + b2 * uv[i2][0] / z[i2]) * zz
                vv = (b0 * uv[i0][1] / z[i0] + b1 * uv[i1][1] / z[i1] + b2 * uv[i2][1] / z[i2]) * zz
                th, tw = tex.shape[:2]
                tx = (np.floor(u * tw).astype(np.int64)) % tw
                ty = (np.floor(vv * th).astype(np.int64)) % th
                texel = tex[ty, tx]
                ok &= texel[..., 3] > 0.5 if mat['flags'] & 0x10 else ok   # alpha test, flag bit to be confirmed
                rgb = texel[..., :3] * np.asarray(mat['diffuse'][:3], np.float32)
            else:
                rgb = np.broadcast_to(np.asarray(mat['diffuse'][:3] if mat else (1, 0, 1), np.float32), zz.shape + (3,))
            cview = color[miny:maxy + 1, minx:maxx + 1]
            cview[ok] = rgb[ok] * shade
            dview[ok] = zz[ok]
    return Image.fromarray((np.clip(color, 0, 1) * 255).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('level'); ap.add_argument('out')
    ap.add_argument('--textures'); ap.add_argument('--size', default='1024x768')
    ap.add_argument('--eye'); ap.add_argument('--at'); ap.add_argument('--fov', type=float, default=60)
    a = ap.parse_args()
    tex_dir = a.textures or os.path.join(os.path.dirname(a.level), '..', '..', 'Textures')
    vec = lambda s: np.array([float(x) for x in s.split(',')]) if s else None
    img = render(ck.File(a.level), tex_dir, tuple(int(x) for x in a.size.split('x')), vec(a.eye), vec(a.at), a.fov)
    img.save(a.out)


if __name__ == '__main__':
    main()
