#!/usr/bin/env python3
"""Export I3D_BIN models as binary glTF (.glb) -- geometry + skeleton + skin.

OBJ (tools/i3dmesh.py) throws away the bones and the weights, so it can only ever be a
rest-pose snapshot. This writes the whole rig, which is what Godot needs to animate.

HOW THE PIECES MAP TO glTF
--------------------------
Two kinds of piece, and they need different treatment because they are stored in
different spaces (see i3dmesh.Piece):

  * RIGID piece (no 0x31 node): vertices are in its bone's LOCAL space, so it is bound
    with weight 1.0 to that one bone under a skin whose inverse-bind matrices are all
    IDENTITY -- which is exactly right, because an identity IBM leaves the vertex in
    bone-local space for the joint matrix to place. Parenting the mesh to the bone node
    would render identically, but Godot only promotes a glTF node to a Skeleton3D bone
    when a skin names it as a joint: a 58-bone model then imports as 25 bones plus a
    pile of loose Node3Ds. Binding every bone keeps the rig in ONE Skeleton3D, which is
    what the I3M tracks will need to target.

  * SKINNED piece (has 0x31 nodes): vertices are already in MODEL space, so it becomes a
    skinned mesh node. glTF computes  sum_t w_t * global(joints[t]) * IBM[t] * v, and the
    file gives us exactly those terms: joints[t] = the mesh instance's bone list, and
    IBM[t] = the 0x46 bind table. Per the glTF spec a skinned mesh node's own transform
    is ignored, so it sits at the scene root.

BONES: the file stores WORLD matrices per bone (root payload +0x14, re-derived from the
0x52 fixup handler at 0x003a6f60, which relocates +0x10/+0x14/+0x18). glTF wants
parent-relative ones, so each bone's local matrix = inverse(world(parent)) * world(self).
Recomposing the hierarchy reproduces the original world matrices -- checked by --verify.

Bone nodes are emitted as TRS, not as a `matrix`: glTF forbids animating a node that has
a matrix, and the I3M rotation channels target exactly these nodes. The decomposition is
EXACT rather than a best fit -- across all 2151 bones in the corpus the worst off-axis
column dot is 9.4e-08 (no shear) and no local matrix has a negative determinant (no
reflection), so T*R*S reproduces the matrix to float noise. --verify checks this.

Scale is emitted, NOT assumed to be 1: 83 bones are non-uniformly scaled and the worst is
45x (a_ara_a_doukutu's doa_1 = 46/39/6.5). Nor is the bind local ROTATION always identity
-- npc_aki's jnt_taleR1 and car's jnt_frontR1 are 180-degree flips. Both claims were made
in earlier docs from ape_nrmb_body_b alone, which happens to be pure translation.

Bone NAMES: no bone NODE carries one (every 0x2a shares one `extra` pointing at an empty
string), but the model does have a sorted name table hanging off the ROOT payload --
root+0x1c is a u16 permutation mapping name order to BONE INDEX, with the sorted name
blob immediately after it. See tools/i3manim.bone_names(). Joints are emitted with their
real names (jnt_armL1, jnt_head, ...), so the I3M tracks -- which are named jnt_* -- bind
BY NAME, exactly as the runtime does (FUN_003a78f8 strcmps the track name). Models whose
table cannot be read fall back to bone_NN.

ANIMATIONS (--anims): each paired I3D_I3M becomes one glTF animation, so Godot's importer
builds the AnimationPlayer itself and no I3M parser is needed on the engine side. One
`rotation` channel per resolvable track; sampler input = key times, output = the comp3
quaternions, interpolation = LINEAR (which the glTF spec defines as slerp for rotations,
matching i3manim.Track.sample).

The track quaternion REPLACES the bone's local rotation -- it is not a delta on the bind
pose. Proven on npc_aki, the only model that can tell the two apart (ape_nrmb_body_b's
bind local rotations are all identity, under which replace and compose are identical): on
all 5 bones whose bind rotation is genuinely non-identity, |q(0) - bind| = 0.0000
exactly, including jnt_taleR1's 180-degree flip. A delta reading predicts q(0) = identity
there and is off by 2.0. So a rotation channel simply overrides the node's TRS rotation,
which is what glTF does natively; the bind translation and scale survive untouched.

Tracks are bound to bones BY NAME, exactly as the runtime does (FUN_003a78f8 strcmps the
track name). Tracks that name no bone are skipped and reported -- they are real and
expected: control rigs (npc_aki's eyeCtrl/mouthCtrl), Maya display layers (ply_*), and
non-skeletal targets (camera1, locator1, cd_uv_anim).

WHICH anim file belongs to which model is NOT known: the game's association lives in the
level scripts / .plc / .asq, which are not reversed. --anims pairs by name resolution as
a TESTING convenience, gated by pair_anims() on TWO independent ratios -- see there for
why one is not enough (it silently binds 18 enemy animations onto the ape rig). Pass
explicit files to bypass the guess entirely; the pairing is a heuristic, the track->bone
binding under it is not.

Materials reference the converted textures. A material name IS the .tm2 basename: they
match on 365/365 non-empty materials across the corpus (the only misses are empty names).
Run tools/tm2.py first; --textures points at its output. Images are referenced by
relative URI, so the .glb stays free of Sony's pixels -- see LEGAL below.

LEGAL: reads Sony's data for personal study / clean-room analysis of mechanics.
Do NOT redistribute extracted assets. Output goes outside assets/.
"""
import argparse
import glob
import json
import math
import os
import struct
import sys

from .i3d import dest_paths
from .i3dmesh import Mat4, load
from .i3manim import Anim, bone_names

FLOAT, USHORT, UINT = 5126, 5123, 5125
ARRAY_BUFFER, ELEMENT_ARRAY_BUFFER = 34962, 34963
MAX_INFLUENCES = 4      # JOINTS_0/WEIGHTS_0 are VEC4; the corpus never exceeds 4.


def _colmajor(m: Mat4):
    """glTF stores matrices column-major; Mat4 is row-major."""
    return [m.m[r][c] for c in range(4) for r in range(4)]


def _trs(m: Mat4):
    """Mat4 -> (translation, rotation quaternion xyzw, scale). Exact for this corpus.

    Valid only because the data has no shear and no reflections (checked over all 2151
    bones: worst off-axis column dot 9.4e-08, zero negative determinants), so each column
    is an axis scaled by its own length. verify_bones() re-multiplies T*R*S and compares
    against the stored world matrices, so a violation would surface as a failure there
    rather than as a silently skewed rig.
    """
    t = [m.m[r][3] for r in range(3)]
    cols = [[m.m[r][c] for r in range(3)] for c in range(3)]
    sc = [math.sqrt(sum(x * x for x in c)) for c in cols]
    # R[r][c]: the normalised basis vectors back as columns
    R = [[cols[c][r] / (sc[c] or 1.0) for c in range(3)] for r in range(3)]
    return t, _quat_of(R), sc


def _quat_of(R):
    """Rotation matrix -> quaternion (x, y, z, w). Shepperd: pivot on the largest
    diagonal term so the divisor is never near zero (a naive w-based formula loses all
    precision at 180 degrees -- which real bones hit: npc_aki's jnt_taleR1)."""
    tr = R[0][0] + R[1][1] + R[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        return [(R[2][1] - R[1][2]) / s, (R[0][2] - R[2][0]) / s,
                (R[1][0] - R[0][1]) / s, 0.25 * s]
    i = max(range(3), key=lambda k: R[k][k])
    j, k = (i + 1) % 3, (i + 2) % 3
    s = math.sqrt(1.0 + R[i][i] - R[j][j] - R[k][k]) * 2
    q = [0.0, 0.0, 0.0, 0.0]
    q[i] = 0.25 * s
    q[j] = (R[j][i] + R[i][j]) / s
    q[k] = (R[k][i] + R[i][k]) / s
    q[3] = (R[k][j] - R[j][k]) / s
    return q


class Glb:
    """Accumulates the binary chunk and the accessor/bufferView bookkeeping."""

    def __init__(self):
        self.bin = bytearray()
        self.views = []
        self.accessors = []

    def _view(self, data: bytes, target=None):
        while len(self.bin) % 4:            # bufferView offsets must be 4-aligned
            self.bin.append(0)
        off = len(self.bin)
        self.bin += data
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target:
            v["target"] = target
        self.views.append(v)
        return len(self.views) - 1

    def add(self, values, comp, typ, target=None, minmax=False):
        n = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}[typ]
        fmt = {FLOAT: "f", USHORT: "H", UINT: "I"}[comp]
        flat = [x for v in values for x in v] if n > 1 else list(values)
        data = struct.pack(f"<{len(flat)}{fmt}", *flat)
        acc = {"bufferView": self._view(data, target), "componentType": comp,
               "count": len(values), "type": typ}
        if minmax:
            # SCALAR values are bare numbers, not 1-tuples. Animation sampler INPUT
            # accessors are the only SCALAR case here, and the spec REQUIRES min/max on
            # them -- without it Godot cannot work out the animation's length.
            if n == 1:
                acc["min"], acc["max"] = [min(values)], [max(values)]
            else:
                acc["min"] = [min(v[i] for v in values) for i in range(n)]
                acc["max"] = [max(v[i] for v in values) for i in range(n)]
        self.accessors.append(acc)
        return len(self.accessors) - 1


def _weld(piece):
    """A glTF vertex is one (position, uv) pair; the file indexes those separately."""
    remap, pos, uvs, idx = {}, [], [], []
    src = []
    for tri in piece.tris:
        for vi, ti in tri:
            key = (vi, ti)
            if key not in remap:
                remap[key] = len(pos)
                v = piece.vtx[vi]
                pos.append([v[0], v[1], v[2]])
                uv = piece.vt[ti] if ti < len(piece.vt) else [0.0, 0.0]
                # V passes through UNFLIPPED. glTF puts TEXCOORD (0,0) at the image's
                # top-left, and these UVs already index the TIM2 rows top-down. This used
                # to emit 1.0-v, which was an untestable guess while no images were
                # referenced. Proven by the colour-coded variants: rendered with v as-is,
                # ape_nrmc_pants_b is CYAN and ape_nrmy_pants_b is YELLOW, matching the
                # c/y naming; flipped, both come out brown/peach and nearly identical.
                uvs.append([uv[0], uv[1]])
                src.append(vi)
            idx.append(remap[key])
    return pos, uvs, idx, src


def _texture_uri(model_path, matname, texroot, dest):
    """Relative URI from the .glb to the material's PNG, or None.

    A material name IS the .tm2 basename (365/365 non-empty materials match). But those
    basenames COLLIDE across stages -- ape_nrm01_b exists under both toyhouse_c and
    arabian_a -- so prefer the copy from this model's own stage. A bare recursive glob
    silently picks whichever it finds first; that class of bug is what silently reduced
    154 exports to 145 files earlier in this project.
    """
    if not matname:
        return None
    cands = sorted(glob.glob(os.path.join(texroot, "**", matname + ".png"), recursive=True))
    if not cands:
        return None
    parts = os.path.normpath(model_path).split(os.sep)
    stage = parts[parts.index("stage") + 1] if "stage" in parts else None
    if stage:
        same = [c for c in cands if os.sep + stage + os.sep in c]
        if same:
            cands = same
    return os.path.relpath(cands[0], os.path.dirname(os.path.abspath(dest)))


def _keys(quats):
    """Prepare comp3 quaternions for a glTF rotation sampler: normalise, then hemisphere.

    NORMALISE because the spec requires unit quaternions and the raw keys are not: they
    are 4 x s16/32768, so quantisation leaves the norm about 1.3e-4 short (a real key
    reads 0.416718, 0.908966 -> norm 0.999873). Scaling to unit changes no rotation --
    q and lambda*q are the same rotation for lambda > 0 -- it just stops every consumer
    from having to guess. Godot normalises on import anyway; emitting it makes the .glb
    conformant instead of relying on that.

    HEMISPHERE because q and -q are the same rotation but not the same interpolation: the
    spec defines LINEAR rotation as slerp along the shortest arc, and Godot does flip
    internally, but pre-flipping makes the data say so outright rather than leaving the
    result contingent on the importer. i3manim.slerp flips at sample time, so the Python
    reference agrees either way.
    """
    out = []
    for q in quats:
        n = math.sqrt(sum(x * x for x in q)) or 1.0
        q = [x / n for x in q]
        if out and sum(a * b for a, b in zip(out[-1], q)) < 0.0:
            q = [-x for x in q]
        out.append(q)
    return out


def animations(g, anim_paths, name_to_node, log=None):
    """Build glTF animations from I3M files. Returns (animations, skipped_track_names)."""
    out, skipped = [], []
    for ap in anim_paths:
        a = Anim(ap)
        channels, samplers = [], []
        for tname, tr in a.tracks.items():
            node = name_to_node.get(tname)
            if node is None:
                skipped.append((os.path.basename(ap), tname))
                continue
            si = g.add(tr.times, FLOAT, "SCALAR", minmax=True)   # min/max REQUIRED
            oi = g.add(_keys(tr.quats), FLOAT, "VEC4")
            samplers.append({"input": si, "output": oi, "interpolation": "LINEAR"})
            channels.append({"sampler": len(samplers) - 1,
                             "target": {"node": node, "path": "rotation"}})
        if not channels:
            continue
        out.append({"name": os.path.splitext(os.path.basename(ap))[0],
                    "channels": channels, "samplers": samplers})
        if log:
            log(f"      anim {os.path.basename(ap):34s} {len(channels):3d} channels "
                f"{a.duration:6.3f}s")
    return out, skipped


def pair_anims(model_path, bone_name_set, threshold=0.75, coverage=0.5, log=None):
    """Co-located I3M files whose tracks resolve against this model's bones.

    A guess, and gated as one -- see the module docstring. TWO gates are needed, because
    they catch opposite failures and neither alone is enough:

      RESOLVE  = anim tracks that name a bone here / all its tracks. Rejects a foreign rig
                 with EXTRA tracks: pl_g_nrm_vcl_w_car (the girl player -- jnt_skirt_*,
                 usa_*) hits ape_spcy_body_c on 33 of 102 tracks = 0.33.
      COVERAGE = bones here that the anim drives / all bones here. Rejects a foreign rig
                 whose names are a SUBSET: every zak_* enemy anim resolves 0.71-0.83
                 against the 58-bone ape rig -- passing the resolve gate outright -- but
                 drives only 15 of its bones = 0.26. Without this gate ape_nrmb_body_b
                 imports 26 animations, 18 of them another creature's.

    Cross-checked against an INDEPENDENT signal: the filename family prefix (ape_*, zak_*,
    npc_*). Resolve alone agrees with it on 48.8% of 529 pairs; adding coverage gives
    100% on 230, and stays at 100% anywhere in coverage 0.3-0.75, so the result is not
    perched on the threshold. Family prefix is the right granularity -- all 15 ape
    variants share one 57-bone skeleton, so ape_dnc_mon1_b legitimately drives
    ape_nrmb_body_b (resolve 1.00, coverage 0.98) despite sharing no filename tokens.
    """
    if not bone_name_set:
        return []
    hits = []
    for ap in sorted(glob.glob(os.path.join(os.path.dirname(model_path), "*.i3d"))):
        try:
            if open(ap, "rb").read(8) != b"I3D_I3M\0":
                continue
            tn = set(Anim(ap).tracks)
        except Exception:
            continue
        if not tn:
            continue
        hit = len(tn & bone_name_set)
        res, cov = hit / len(tn), hit / len(bone_name_set)
        if res >= threshold and cov >= coverage:
            hits.append(ap)
        elif hit and log:
            log(f"      skip {os.path.basename(ap):34s} resolve={res:.2f} "
                f"coverage={cov:.2f} (need {threshold:.2f}/{coverage:.2f})")
    return hits


def build(model, name, texroot=None, dest=None, anims=None, anim_threshold=0.75,
          anim_coverage=0.5, log=None):
    g = Glb()
    nodes, meshes, skins, materials = [], [], [], []
    images, textures = [], []
    # Real joint names, so the I3M tracks (named jnt_*) bind BY NAME like the runtime.
    try:
        bnames, _ = bone_names(model.path)
    except Exception:
        bnames = []

    matidx = {}
    for m in model.materials:
        matidx[m] = len(materials)
        mat = {"name": m or "unnamed",
               "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 1.0}}
        uri = _texture_uri(model.path, m, texroot, dest) if texroot and dest else None
        if uri:
            images.append({"uri": uri.replace(os.sep, "/"), "name": m})
            textures.append({"source": len(images) - 1, "sampler": 0})
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": len(textures) - 1}
            # the atlas has hard-edged colour patches; NEAREST keeps the seams crisp
            # and stops neighbouring patches bleeding into each other
            mat["alphaMode"] = "MASK"
        materials.append(mat)

    # --- bones: world -> parent-relative ---------------------------------------
    # A handful of bones are genuinely DEGENERATE in the data: npc_aki's bones 27 and 28
    # (2 of 2151 in the corpus, the only ones) have a zero X basis, so their world matrix
    # is singular. A singular parent destroys information -- no local matrix can satisfy
    # world(child) = world(parent) * local -- so a child of one cannot be parented in
    # glTF without corrupting its world transform. The stored world matrices are the
    # authoritative thing (the engine indexes them directly rather than composing the
    # hierarchy), so such a child is re-rooted with its world matrix intact. Neither
    # degenerate bone is referenced by any geometry.
    bone_node, reparented = [], []
    for i, w in enumerate(model.bone_world):
        p = model.bone_parent[i]
        try:
            local = w if p < 0 else model.bone_world[p].inverse() * w
        except ValueError:
            local, reparented = w, reparented + [i]
        # TRS, not "matrix": glTF forbids animating a node that carries a matrix, and the
        # I3M rotation channels target these very nodes. Exact here -- no shear, no
        # reflections (see _trs).
        t, q, s = _trs(local)
        nodes.append({"name": bnames[i] if i < len(bnames) and bnames[i]
                                   else f"bone_{i:02d}",
                      "translation": t, "rotation": q, "scale": s})
        bone_node.append(len(nodes) - 1)
    for i, p in enumerate(model.bone_parent):
        if p >= 0 and i not in reparented:
            nodes[bone_node[p]].setdefault("children", []).append(bone_node[i])

    # --- pieces ----------------------------------------------------------------
    # Bucket by what places the piece: a skin (skinned) or a bone node (rigid). Each
    # bucket becomes one mesh; primitives inside it carry the material.
    buckets, order = {}, []
    for grp in model.groups:
        for p in grp.pieces:
            if p.skin is not None:
                key = ("skin", tuple(p.joints),
                       tuple(tuple(map(tuple, M.m)) for M in p.ibm))
            else:
                key = ("rigid", p.bone)
            if key not in buckets:
                buckets[key] = []
                order.append(key)
            buckets[key].append((grp, p))

    skin_of = {}
    for key in order:
        if key[0] != "skin":
            continue
        joints = key[1]
        ibm = [Mat4(rows) for rows in key[2]]
        acc = g.add([_colmajor(M) for M in ibm], FLOAT, "MAT4")
        skins.append({"inverseBindMatrices": acc,
                      "joints": [bone_node[j] for j in joints]})
        skin_of[key] = len(skins) - 1

    # One shared skin for every rigid piece: joints = ALL bones (so they all become
    # Skeleton3D bones), identity IBMs (so bone-local vertices stay put), and each vertex
    # bound 1.0 to its own bone. Sharing the joint nodes with the skins above keeps
    # everything in a single skeleton.
    rigid_skin = None
    if any(k[0] == "rigid" for k in order):
        ident = Mat4.identity()
        acc = g.add([_colmajor(ident)] * len(model.bone_world), FLOAT, "MAT4")
        skins.append({"inverseBindMatrices": acc, "joints": list(bone_node)})
        rigid_skin = len(skins) - 1

    for key in order:
        prims = []
        for grp, p in buckets[key]:
            if not p.tris:
                continue
            pos, uvs, idx, src = _weld(p)
            attrs = {"POSITION": g.add(pos, FLOAT, "VEC3", ARRAY_BUFFER, minmax=True),
                     "TEXCOORD_0": g.add(uvs, FLOAT, "VEC2", ARRAY_BUFFER)}
            j4, w4 = [], []
            if p.skin is not None:
                for vi in src:
                    infl = sorted(p.skin[vi], key=lambda t: -t[1])[:MAX_INFLUENCES]
                    j4.append([t[0] for t in infl] + [0] * (4 - len(infl)))
                    w4.append([t[1] for t in infl] + [0.0] * (4 - len(infl)))
            else:
                j4 = [[p.bone, 0, 0, 0]] * len(pos)
                w4 = [[1.0, 0.0, 0.0, 0.0]] * len(pos)
            attrs["JOINTS_0"] = g.add(j4, USHORT, "VEC4", ARRAY_BUFFER)
            attrs["WEIGHTS_0"] = g.add(w4, FLOAT, "VEC4", ARRAY_BUFFER)
            prim = {"attributes": attrs,
                    "indices": g.add(idx, UINT, "SCALAR", ELEMENT_ARRAY_BUFFER)}
            if grp.material in matidx:
                prim["material"] = matidx[grp.material]
            prims.append(prim)
        if not prims:
            continue
        meshes.append({"name": f"{key[0]}_{len(meshes)}", "primitives": prims})
        mi = len(meshes) - 1
        # A skinned mesh node's own transform is ignored per spec, so both kinds sit at
        # the scene root and are placed entirely by their joints.
        if key[0] == "skin":
            nodes.append({"name": f"skinned_{mi}", "mesh": mi, "skin": skin_of[key]})
        else:
            nodes.append({"name": f"rigid_{mi}_b{key[1]}", "mesh": mi, "skin": rigid_skin})

    # --- animations -------------------------------------------------------------
    # Bind BY NAME, like the runtime. Done after the bone nodes exist so a channel can
    # target one; a track naming no bone is dropped and reported, never silently.
    gltf_anims = []
    if anims is not None:
        name_to_node = {n: bone_node[i] for i, n in enumerate(bnames) if n}
        paths = anims if anims else pair_anims(model.path, set(name_to_node),
                                               anim_threshold, anim_coverage, log)
        gltf_anims, skipped = animations(g, paths, name_to_node, log)
        if skipped and log:
            byanim = {}
            for f, t in skipped:
                byanim.setdefault(f, []).append(t)
            for f, ts in byanim.items():
                log(f"      note {f:34s} {len(ts)} tracks name no bone: "
                    f"{', '.join(sorted(ts)[:4])}{' ...' if len(ts) > 4 else ''}")

    child = {c for n in nodes for c in n.get("children", [])}
    roots = [i for i in range(len(nodes)) if i not in child]
    gltf = {"asset": {"version": "2.0",
                      "generator": "ape-escape-3-recreation/tools/i3dgltf.py"},
            "scene": 0, "scenes": [{"name": name, "nodes": roots}],
            "nodes": nodes, "meshes": meshes,
            "buffers": [{"byteLength": len(g.bin)}],
            "bufferViews": g.views, "accessors": g.accessors}
    if skins:
        gltf["skins"] = skins
    if materials:
        gltf["materials"] = materials
    if gltf_anims:
        gltf["animations"] = gltf_anims
    if images:
        gltf["images"] = images
        gltf["textures"] = textures
        # NEAREST/NEAREST: the atlases are 64x64 with hard-edged colour patches, so
        # bilinear filtering bleeds neighbouring patches across the seams.
        gltf["samplers"] = [{"magFilter": 9728, "minFilter": 9728,
                             "wrapS": 10497, "wrapT": 10497}]
    return gltf, bytes(g.bin)


def write_glb(model, path, texroot=None, anims=None, anim_threshold=0.75,
              anim_coverage=0.5, log=None):
    name = os.path.splitext(os.path.basename(path))[0]
    gltf, binchunk = build(model, name, texroot=texroot, dest=path, anims=anims,
                           anim_threshold=anim_threshold, anim_coverage=anim_coverage,
                           log=log)
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((4 - len(js) % 4) % 4)            # chunks must be 4-aligned
    binchunk += b"\0" * ((4 - len(binchunk) % 4) % 4)
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "wb") as f:
        total = 12 + 8 + len(js) + (8 + len(binchunk) if binchunk else 0)
        f.write(struct.pack("<3I", 0x46546C67, 2, total))
        f.write(struct.pack("<2I", len(js), 0x4E4F534A) + js)
        if binchunk:
            f.write(struct.pack("<2I", len(binchunk), 0x004E4942) + binchunk)
    return gltf


def _from_trs(t, q, s):
    """T*R*S back to a Mat4 -- the composition glTF itself will perform."""
    from .i3manim import quat_to_m3
    R = quat_to_m3(q)
    return Mat4([[R[r][c] * s[c] for c in range(3)] + [t[r]] for r in range(3)]
                + [[0.0, 0.0, 0.0, 1.0]])


def verify_bones(model):
    """Recompose each bone's world matrix from what we actually EMIT, and compare against
    the world matrix the file stores. Catches a wrong parent index, a bad inverse, or a
    lossy TRS decomposition -- each of which would still produce a loadable, silently
    skewed rig. Since the decomposition is now part of the emitted data, it is checked
    here too: the local matrix is round-tripped through _trs/_from_trs before use.
    """
    world = {}
    for i, p in enumerate(model.bone_parent):
        rooted = p < 0
        try:
            local = model.bone_world[i] if rooted \
                else model.bone_world[p].inverse() * model.bone_world[i]
        except ValueError:
            local, rooted = model.bone_world[i], True   # degenerate parent: re-rooted
        local = _from_trs(*_trs(local))                 # exactly what the .glb carries
        # Parents always precede children (checked on 154/154), so world[p] is ready.
        world[i] = local if rooted else world[p] * local
    worst = 0.0
    for i, w in enumerate(model.bone_world):
        worst = max(worst, max(abs(w.m[r][c] - world[i].m[r][c])
                               for r in range(4) for c in range(4)))
    return worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out", default="extracted/glb")
    ap.add_argument("--textures", default="extracted/png",
                    help="root of tools/tm2.py output; '' to emit untextured materials")
    ap.add_argument("--anims", nargs="*", default=None, metavar="I3M",
                    help="bake I3M animations in. No values = auto-pair co-located anims "
                         "(a guess -- see module docstring); explicit files bypass it.")
    ap.add_argument("--anim-threshold", type=float, default=0.75,
                    help="RESOLVE gate: min fraction of an anim's tracks that must name a "
                         "bone in the model before auto-pairing accepts it")
    ap.add_argument("--anim-coverage", type=float, default=0.5,
                    help="COVERAGE gate: min fraction of the model's bones the anim must "
                         "drive. Without it, every zak_* enemy anim binds to the ape rig.")
    ap.add_argument("--verify", action="store_true",
                    help="check the emitted TRS bone hierarchy recomposes the stored "
                         "world matrices")
    args = ap.parse_args()

    if args.verify:
        worst = 0.0
        for p in args.files:
            w = verify_bones(load(p))
            worst = max(worst, w)
            if w > 1e-3:
                print(f"  BAD  {os.path.basename(p):44s} worst={w:.6f}")
        print(f"\nbone hierarchy: worst deviation over {len(args.files)} files = {worst:.8f}")
        return 1 if worst > 1e-3 else 0

    nok = nfail = nanim = 0
    dests = dest_paths(args.files, args.out, ".glb")
    for p, dest in zip(args.files, dests):
        try:
            model = load(p)
            gltf = write_glb(model, dest, texroot=args.textures or None,
                             anims=args.anims, anim_threshold=args.anim_threshold,
                             anim_coverage=args.anim_coverage,
                             log=print if args.anims is not None else None)
            nsk = len(gltf.get("skins", []))
            na = len(gltf.get("animations", []))
            nanim += na
            print(f"  OK   {os.path.basename(p):44s} nodes={len(gltf['nodes']):4d} "
                  f"meshes={len(gltf['meshes']):3d} skins={nsk:2d} "
                  f"bones={len(model.bone_parent):3d} imgs={len(gltf.get('images', [])):2d} "
                  f"anims={na:2d} size={os.path.getsize(dest):8d}")
            nok += 1
        except Exception as ex:
            print(f"  FAIL {os.path.basename(p):44s} {type(ex).__name__}: {ex}")
            nfail += 1
    print(f"\n{nok} ok, {nfail} failed"
          + (f", {nanim} animations baked" if args.anims is not None else ""))
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
