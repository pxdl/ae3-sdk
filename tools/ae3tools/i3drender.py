#!/usr/bin/env python3
"""Verification renderer for the I3D pipeline: solid faces, real textures, posed skin.

This exists to make skin/animation bugs VISIBLE. Two earlier shortcuts could not:

  * a POINT CLOUD (one dot per vertex, `pc.tris` never touched) can only answer "did the
    right vertices move" -- a torn or scattered surface is invisible.
  * a FLAT CENTROID sample (one texel at each triangle's UV centroid, filled flat) looks
    like texturing but proves nothing: these atlases are 64x64 grids of flat colour
    patches with HARD seams, so any triangle whose centroid lands across a seam takes the
    wrong patch -- which reads as "colours all over the place". It also cannot show
    texture detail, so it can never validate the UVs.

So: rasterise the real triangles, interpolate UV per pixel, z-buffer.

UV `v` is NOT flipped -- TIM2 rows and `v` both run top-down. Proven by the colour-coded
variants: with `v` as-is `ape_nrmc_pants_b` is CYAN and `ape_nrmy_pants_b` is YELLOW,
matching the c/y naming (and ape_nrmb = blue shorts); flipped, both come out brown/peach
and nearly identical to each other. `--flip` is kept only to re-run that A/B.

Usage:
  i3drender.py <model.i3d> <out.png> [--yaw DEG] [--flip]
  i3drender.py <model.i3d> <out.png> --anim <anim.i3d> [--t SECONDS] [--yaw DEG]

Run tools/tm2.py first; untextured materials fall back to flat blue.
"""
import glob
import math
import os
import sys

from PIL import Image

from .i3dmesh import Mat4, load
from .i3manim import Anim, bone_names, quat_to_m3

SIZE = 460


def local_of(model):
    """Bind-pose local matrices, with the degenerate-parent fallback i3dgltf uses."""
    local, rooted = [], []
    for i, p in enumerate(model.bone_parent):
        try:
            local.append(model.bone_world[i] if p < 0
                         else model.bone_world[p].inverse() * model.bone_world[i])
            rooted.append(p < 0)
        except ValueError:                  # singular parent: re-root, world intact
            local.append(model.bone_world[i])
            rooted.append(True)
    return local, rooted


def world_from_local(model, local, rooted):
    w = [None] * len(local)
    for i, p in enumerate(model.bone_parent):
        w[i] = local[i] if rooted[i] else w[p] * local[i]
    return w


def posed_world(model, anim, names, t):
    """Pose at time t: the track's quaternion REPLACES the bone's bind local rotation.

    LOCAL, not world: many tracks sample exactly identity (jnt_armcuffL3 = (0,0,0,32767),
    jnt_pelvis, jnt_tail1), which under a local reading means "follow the parent rigidly"
    -- natural for a cuff on a swinging arm -- but under a world reading would pin the
    bone to the world axes and tear the arm off. comp2 is NOT the translation (refuted:
    mean 0.40 units from the nearest real bone), so the skeleton supplies it.

    REPLACE, not compose: on npc_aki -- the model that can tell them apart -- the first
    key reproduces the bind local rotation to |q(0) - bind| = 0.0000 on all 5 bones whose
    bind rotation is genuinely non-identity, jnt_taleR1's 180-degree flip included. A
    delta reading predicts identity there and is off by 2.0.
    (This file used to claim every bind local rotation IS identity. That is true of
    ape_nrmb_body_b, the only model it was ever run on, and false in general -- which is
    exactly why the claim survived, and why the check moved to npc_aki.)

    Only the ROTATION is replaced. The bind local scale is carried through: 83 bones in
    the corpus are non-uniformly scaled and the worst is 45x, so overwriting the whole
    3x3 with a pure rotation -- as this did -- silently resets them to 1. Invisible on
    ape_nrmb_body_b, whose bones are all unit scale.
    """
    local, rooted = local_of(model)
    out = []
    for i, L in enumerate(local):
        nm = names[i] if i < len(names) else None
        tr = anim.tracks.get(nm) if nm else None
        if tr is None:
            out.append(L)
            continue
        m3 = quat_to_m3(tr.sample(t))
        # each column of the bind local 3x3 is an axis times its scale (no shear anywhere
        # in the corpus: worst off-axis column dot 9.4e-08), so |column| IS that scale
        sc = [math.sqrt(sum(L.m[r][c] ** 2 for r in range(3))) for c in range(3)]
        out.append(Mat4([[m3[r][0] * sc[0], m3[r][1] * sc[1], m3[r][2] * sc[2], L.m[r][3]]
                         for r in range(3)]
                        + [[0.0, 0.0, 0.0, 1.0]]))
    return world_from_local(model, out, rooted)


_TEX = {}


def tex_for(model_path, matname):
    """The material's texture, FROM THE MODEL'S OWN STAGE.

    A material name IS the .tm2 basename (365/365 non-empty match), but basenames COLLIDE
    across stages (ape_nrm01_b lives under both toyhouse_c and arabian_a), so a bare
    recursive glob silently picks whichever it finds first.
    """
    if not matname:
        return None
    key = (model_path, matname)
    if key in _TEX:
        return _TEX[key]
    cands = sorted(glob.glob(f"extracted/png/**/{matname}.png", recursive=True))
    parts = os.path.normpath(model_path).split(os.sep)
    stage = parts[parts.index("stage") + 1] if "stage" in parts else None
    if stage:
        same = [c for c in cands if os.sep + stage + os.sep in c]
        if same:
            cands = same
    _TEX[key] = Image.open(cands[0]).convert("RGB") if cands else None
    return _TEX[key]


def gather(model, world, model_path):
    """Per triangle: 3 world positions, 3 UVs, the texture."""
    out = []
    for g in model.groups:
        tex = tex_for(model_path, g.material)
        for pc in g.pieces:
            if pc.skin is None:
                w = world[pc.bone]
                P = [w.xform(v) for v in pc.vtx]
            else:
                mats = [world[pc.joints[k]] * pc.ibm[k] for k in range(len(pc.joints))]
                P = []
                for v, infl in zip(pc.vtx, pc.skin):
                    acc = [0.0, 0.0, 0.0, 0.0]
                    for j, wt in infl:
                        x = mats[j].xform(v)
                        acc = [a + wt * b for a, b in zip(acc, x)]
                    P.append(acc)
            for tri in pc.tris:              # tri = [(vertex_index, uv_index), ...]
                out.append(([P[k[0]] for k in tri],
                            [pc.vt[k[1]][:2] for k in tri] if pc.vt else [(0, 0)] * 3,
                            tex))
    return out


def draw(tris, size, yaw_deg, flip_v):
    c, s = math.cos(math.radians(yaw_deg)), math.sin(math.radians(yaw_deg))
    pts = [([(v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c) for v in pos], uv, tex)
           for pos, uv, tex in tris]
    allv = [v for p, _, _ in pts for v in p]
    xs = [v[0] for v in allv]
    ys = [v[1] for v in allv]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    span = max(max(xs) - min(xs), max(ys) - min(ys)) * 1.08
    sc = (size * 0.92) / span
    img = Image.new("RGB", (size, size), (20, 20, 26))
    px = img.load()
    zbuf = [[-1e30] * size for _ in range(size)]

    def proj(v):
        return (size / 2 + (v[0] - cx) * sc, size / 2 - (v[1] - cy) * sc, v[2])

    L = (0.4, 0.45, 0.8)
    for pos, uv, tex in pts:
        a, b, cc = (proj(v) for v in pos)
        w1 = [pos[1][i] - pos[0][i] for i in range(3)]
        w2 = [pos[2][i] - pos[0][i] for i in range(3)]
        n = [w1[1] * w2[2] - w1[2] * w2[1], w1[2] * w2[0] - w1[0] * w2[2],
             w1[0] * w2[1] - w1[1] * w2[0]]
        ln = math.sqrt(sum(x * x for x in n)) or 1.0
        n = [x / ln for x in n]
        area = (b[0] - a[0]) * (cc[1] - a[1]) - (cc[0] - a[0]) * (b[1] - a[1])
        if area >= 0:                       # degenerate or backfacing
            continue
        lam = max(0.18, abs(sum(n[k] * L[k] for k in range(3))))
        x0 = max(0, int(min(a[0], b[0], cc[0])))
        x1 = min(size - 1, int(max(a[0], b[0], cc[0])) + 1)
        y0 = max(0, int(min(a[1], b[1], cc[1])))
        y1 = min(size - 1, int(max(a[1], b[1], cc[1])) + 1)
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                fx, fy = x + 0.5, y + 0.5
                w0 = ((b[0] - a[0]) * (fy - a[1]) - (fx - a[0]) * (b[1] - a[1])) / area
                wa = ((cc[0] - b[0]) * (fy - b[1]) - (fx - b[0]) * (cc[1] - b[1])) / area
                wb = ((a[0] - cc[0]) * (fy - cc[1]) - (fx - cc[0]) * (a[1] - cc[1])) / area
                if w0 < 0 or wa < 0 or wb < 0:
                    continue
                z = wa * a[2] + wb * b[2] + w0 * cc[2]
                if z <= zbuf[y][x]:
                    continue
                zbuf[y][x] = z
                if tex is None:
                    col = (150, 190, 235)
                else:
                    u = wa * uv[0][0] + wb * uv[1][0] + w0 * uv[2][0]
                    v = wa * uv[0][1] + wb * uv[1][1] + w0 * uv[2][1]
                    tw, th = tex.size
                    col = tex.getpixel((int(u * tw) % tw,
                                        int((1.0 - v if flip_v else v) * th) % th))
                px[x, y] = tuple(min(255, int(q * (0.35 + 0.65 * lam))) for q in col)
    return img


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    mp, out = sys.argv[1], sys.argv[2]
    flip = "--flip" in sys.argv
    yawd = float(sys.argv[sys.argv.index("--yaw") + 1]) if "--yaw" in sys.argv else 0.0
    model = load(mp)
    if "--anim" in sys.argv:
        ap = sys.argv[sys.argv.index("--anim") + 1]
        t = float(sys.argv[sys.argv.index("--t") + 1]) if "--t" in sys.argv else 0.0
        names, _ = bone_names(mp)
        anim = Anim(ap)
        hit = sum(1 for n in names if n in anim.tracks)
        print(f"bones={len(names)} tracks={len(anim.tracks)} bound BY NAME={hit} t={t}")
        world = posed_world(model, anim, names, t)
    else:
        local, rooted = local_of(model)
        world = world_from_local(model, local, rooted)
    tris = gather(model, world, mp)
    draw(tris, SIZE, yawd, flip).save(out)
    print(f"{len(tris)} tris -> {out}  (flip_v={flip}, yaw={yawd})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
