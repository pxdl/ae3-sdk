#!/usr/bin/env python3
"""Decode I3D_BIN geometry (Ape Escape 3 / SG2) and export OBJ+MTL.

PROVENANCE / CREDIT
-------------------
The payload layout here is a PORT of Murugo's `mdl2obj.py` for Rule of Rose (PS2),
which is the same I3D engine family:

    https://github.com/Murugo/Misc-Game-Research/blob/main/PS2/Rule%20of%20Rose/mdl2obj.py
    MIT License, Copyright (c) Murugo.  Ported here under the MIT terms.

Ported from that MIT original -- deliberately NOT from Durik256's Noesis adaptation
(fmt_i3dg.py), which is a derivative of the same script but ships with no license file.
The only thing that adaptation adds over the original is locating the payload at
file+0x10 (the original reads a Rule of Rose RTPK archive instead); we already derived
that independently from SCUS_975.01 -- see i3d.py and docs/formats/I3D.md.

Changes made in this port, and why:
  * No numpy (not installed here) -- Mat4 below is a plain 4x4.
  * Node tree comes from i3d.py, which reads CHILD_COUNT as the full 24 bits per the
    decompiled fixup routine (FUN_003a85e0: `uVar1 & 0xffffff`). The reference reads a
    u16 at +0x4; that is the low half of the same field and only agrees while counts
    stay under 65536.
  * VIF `vumem` is per-call, not a module global. In the reference a stale buffer can
    leak between meshes when a piece unpacks fewer quadwords than the previous one.
  * Verified against Ape Escape 3 rather than Rule of Rose; the node types are
    unchanged (see the census in docs/formats/I3D.md).

WHAT IS INDEPENDENTLY CONFIRMED vs INHERITED
--------------------------------------------
Confirmed by us from SCUS_975.01 / the corpus: the container (file+0x10 base, 16-byte
node, type = bits 24..30, count = bits 0..23), and the type roles 0x25 Material,
0x2d CombinedMesh, 0x59 MeshInstance, 0x2a Bone -- all cross-checked against the
tail string table ('Material0000', 'CombinedMesh0000', 'polySurfaceShape977').
INHERITED from the reference and NOT yet re-derived from the ELF: the byte-level
payload field offsets (transform table at root.data+0x14, the 0x59/0x4c/0x4d/0x56
field layout, the 4-byte index record). They reproduce correct geometry, which is
strong evidence, but they are not read out of the decompilation yet.

LEGAL: reads Sony's data for personal study / clean-room analysis of mechanics.
Do NOT redistribute extracted assets. Output goes outside assets/.
"""
import argparse
import os
import struct
import sys

from .i3d import BASE, I3d, dest_paths

# Node types. Roles marked (*) are confirmed by us from the string table / ELF;
# the rest are inherited from the Rule of Rose reference.
T_MATERIAL = 0x25      # (*)
T_BONE = 0x2A          # (*)
T_COMBINED_MESH = 0x2D  # (*)
T_VERTEX_WEIGHTS = 0x31
T_BIND_POSE = 0x46
T_INDEX_LIST = 0x47
T_MESH = 0x4B
T_MESH_BONEREF = 0x4C
T_SUBMESH = 0x4D
T_SUBMESH_PIECE = 0x56
T_MESH_INSTANCE = 0x59  # (*)


class Mat4:
    """Row-major 4x4. Replaces numpy.matrix in the reference."""

    __slots__ = ("m",)

    def __init__(self, rows):
        self.m = [list(r) for r in rows]

    @staticmethod
    def identity():
        return Mat4([[1 if i == j else 0 for j in range(4)] for i in range(4)])

    def transpose(self):
        return Mat4([[self.m[j][i] for j in range(4)] for i in range(4)])

    def __mul__(self, o):
        a, b = self.m, o.m
        return Mat4([[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
                     for i in range(4)])

    def xform(self, v):
        """Transform a column vec4 (the reference does `transform * v.reshape(-1,1)`)."""
        return [sum(self.m[i][k] * v[k] for k in range(4)) for i in range(4)]

    def scaled(self, s):
        return Mat4([[x * s for x in r] for r in self.m])

    def __add__(self, o):
        return Mat4([[self.m[i][j] + o.m[i][j] for j in range(4)] for i in range(4)])

    def inverse(self):
        """Gauss-Jordan with partial pivoting. Needed for bone LOCAL transforms:
        the file stores WORLD matrices only, and glTF wants parent-relative ones."""
        a = [row[:] + [1.0 if i == j else 0.0 for j in range(4)]
             for i, row in enumerate(self.m)]
        for c in range(4):
            piv = max(range(c, 4), key=lambda r: abs(a[r][c]))
            if abs(a[piv][c]) < 1e-12:
                raise ValueError("singular matrix")
            a[c], a[piv] = a[piv], a[c]
            d = a[c][c]
            a[c] = [x / d for x in a[c]]
            for r in range(4):
                if r != c and a[r][c] != 0.0:
                    f = a[r][c]
                    a[r] = [x - f * y for x, y in zip(a[r], a[c])]
        return Mat4([row[4:] for row in a])


def _u8(b, o):
    return b[o]


def _u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _nf32(b, o, n):
    return list(struct.unpack_from("<" + "f" * n, b, o))


def _cstr(b, o):
    z = b.find(b"\0", o)
    return b[o:z if z >= 0 else o].decode("ascii", "replace")


def parse_vif(buf, offs):
    """Interpret a VIF DMA packet and return the resulting VU1 memory (list of vec4).

    PS2 geometry is uploaded to VU1 as VIF command packets rather than stored as a flat
    array, so the vertex/UV blocks have to be *executed*, not just read. Ported from the
    MIT reference; the wrapper header is 0x10 bytes and +0x4 holds the packet size in
    quadwords.
    """
    vumem = [[0.0, 0.0, 0.0, 0.0] for _ in range(0x1000)]
    endoffs = offs + (buf[offs + 0x4] << 4) + 0x10
    offs += 0x10
    vif_r = [0, 0, 0, 0]
    vif_c = [1, 1, 1, 1]
    cl = wl = 1
    mask = [0] * 16

    def maybe_mask(val, index, cycle, use_mask):
        if not use_mask or mask[index] == 0b00:
            return val
        if mask[index + min(cycle, 3) * 4] == 0b01:
            return vif_r[index]
        if mask[index + min(cycle, 3) * 4] == 0b10:
            return vif_c[min(3, cycle)]
        return 0

    while offs < endoffs:
        imm, qwd, cmd = struct.unpack_from("<HBB", buf, offs)
        cmd &= 0x7F
        offs += 4
        if cmd == 0x00:              # NOP
            continue
        if cmd == 0x01:              # STCYCLE
            cl, wl = imm & 0xFF, (imm >> 8) & 0xFF
        elif cmd == 0x30:            # STROW
            vif_r = _nf32(buf, offs, 4)
            offs += 0x10
        elif cmd == 0x31:            # STCOL
            vif_c = _nf32(buf, offs, 4)
            offs += 0x10
        elif cmd == 0x20:            # STMASK
            m = _u32(buf, offs)
            mask = [((m >> (i << 1)) & 0x3) for i in range(16)]
            offs += 4
        elif cmd >> 5 == 0b11:       # UNPACK
            addr = imm & 0x3FF
            vnvl = cmd & 0xF
            use_mask = (cmd & 0x10) > 0
            width = {0b0000: 4, 0b0100: 8, 0b1000: 12, 0b1100: 16}.get(vnvl)
            if width is None:
                raise ValueError(f"unsupported UNPACK vnvl {vnvl:#x} at {offs:#x}")
            ncomp = width // 4
            j = 0
            for i in range(qwd):
                val = [0.0] * ncomp
                if cl >= wl or (i % wl) < cl:
                    val = _nf32(buf, width * j + offs, ncomp)
                    j += 1
                val = (val + [0.0, 0.0, 0.0, 0.0])[:4]
                ao = cl * (i // wl) + (i % wl) if cl >= wl else 0
                vumem[addr + ao] = [maybe_mask(val[k], k, i, use_mask) for k in range(4)]
            offs += j * width
        else:
            raise ValueError(f"unrecognized vifcmd {cmd:#x} at {offs:#x}")
    return vumem


def _by_type(node, t):
    """Pre-order subtree search including `node` itself -- matches the reference's
    getChildrenByType() ordering, which matters because indices refer into these lists."""
    return [n for n in node.walk() if n.type == t]


def _read_buffer(buf, node_off, count):
    """A vertex/UV block: either a VIF packet (+0x8 == 1) or a flat vec4 array."""
    if _u8(buf, node_off + 0x8) == 1:
        return parse_vif(buf, node_off)[:count]
    return [_nf32(buf, node_off + i * 0x10 + 0x10, 4) for i in range(count)]


class Piece:
    """One VIF vertex block plus the triangles over it.

    `vtx` is RAW -- exactly as unpacked, NOT baked to model space. Which space that is
    depends on whether the piece is skinned:
      * rigid  (skin is None): vertices are in `bone`'s LOCAL space; `xf` maps them to
        model space.
      * skinned(skin is not None): vertices are already in MODEL space, and `skin_mats`
        are the per-joint skinning matrices (identity at bind -- see load()).
    Baking is left to the writer so the glTF exporter can keep vertices unbaked.
    """

    __slots__ = ("vtx", "vt", "tris", "xf", "bone", "skin", "skin_mats", "joints", "ibm")

    def __init__(self):
        self.vtx, self.vt, self.tris = [], [], []
        self.xf = None          # rigid: bone-local -> model
        self.bone = -1          # rigid: owning GLOBAL bone index
        self.skin = None        # skinned: per-vertex [(local_joint, weight), ...]
        self.skin_mats = None   # skinned: Mat4 per local joint = world(joints[t]) * ibm[t]
        self.joints = None      # skinned: GLOBAL bone index per local joint
        self.ibm = None         # skinned: INVERSE BIND matrix per local joint (type 0x46)

    def model_space(self):
        """Bake to model space: LBS for skinned pieces, a single matrix for rigid."""
        if self.skin is None:
            return [self.xf.xform(v) for v in self.vtx]
        out = []
        for v, infl in zip(self.vtx, self.skin):
            acc = [0.0, 0.0, 0.0, 0.0]
            for j, w in infl:
                t = self.skin_mats[j].xform(v)
                acc = [a + w * b for a, b in zip(acc, t)]
            out.append(acc)
        return out


class Group:
    __slots__ = ("name", "material", "pieces")

    def __init__(self, name, material):
        self.name, self.material, self.pieces = name, material, []


class Model:
    __slots__ = ("path", "groups", "materials", "bone_parent", "bone_world")

    def __init__(self, path, groups, materials, bone_parent, bone_world):
        self.path, self.groups, self.materials = path, groups, materials
        self.bone_parent, self.bone_world = bone_parent, bone_world


def load(path):
    """Return a Model. Geometry is left RAW; see Piece for which space it is in."""
    blob = open(path, "rb").read()
    model = I3d(blob, path)
    buf = blob[BASE:]          # every offset in the file is relative to file+0x10
    root = model.root

    materials = []
    for mnode in _by_type(root, T_MATERIAL):
        moff = mnode.children[0].children[0].data
        materials.append(_cstr(buf, _u32(buf, moff + 0x18) + moff))

    def transform(table_off, idx):
        o = table_off + idx * 0x40
        return Mat4([_nf32(buf, o + i * 0x10, 4) for i in range(4)]).transpose()

    bones = _by_type(root, T_BONE)
    xform_table = _u32(buf, root.data + 0x14) + root.data
    combined = _by_type(root, T_COMBINED_MESH)

    groups = []
    for bone_index, bone in enumerate(bones):
        inst_nodes = _by_type(bone, T_MESH_INSTANCE)
        if not inst_nodes:
            continue
        base_xform = transform(xform_table, bone_index)

        for inst in inst_nodes:
            bl_off = _u32(buf, inst.data) + inst.data
            cm_index = _u16(buf, inst.data + 0x4)
            bl_count = _u16(buf, inst.data + 0x6)
            bone_list = list(struct.unpack_from("<" + "H" * bl_count, buf, bl_off))
            cm = combined[cm_index]

            bind_off = 0
            if cm.children[0].data > 0:      # type 0x46
                bind_off = _u32(buf, cm.children[0].data) + cm.children[0].data

            for mesh in _by_type(cm, T_MESH) + _by_type(cm, T_MESH_BONEREF):
                # Recomputed per mesh, and never written to inside the piece loop below.
                # (The reference assigns its `xf` inside that loop, so a skinned piece
                # leaks its matrix onto the next *rigid* piece of the same submesh.)
                xf, rigid_bone = base_xform, bone_index
                if mesh.type == T_MESH_BONEREF and (_u8(buf, mesh.data + 0x5) & 0x8):
                    rigid_bone = bone_list[_u16(buf, mesh.data + 0x8)]
                    xf = transform(xform_table, rigid_bone)

                for sm in _by_type(mesh, T_SUBMESH):
                    mat_index = _u8(buf, sm.data + 0xC)
                    g = Group(f"b{bone_index}_cm{cm_index}_"
                              f"m{mesh.off:x}_s{sm.off:x}",
                              materials[mat_index] if mat_index < len(materials) else "")
                    groups.append(g)

                    for spn in _by_type(sm, T_SUBMESH_PIECE):
                        p = Piece()
                        g.pieces.append(p)
                        p.xf, p.bone = xf, rigid_bone

                        vnode = spn.children[4].children[0]
                        vcount = _u8(buf, vnode.data + 0x6)
                        p.vtx = _read_buffer(buf, vnode.data, vcount)

                        # Skin. Each 0x31 node is ONE joint's influence over a subset of
                        # this piece's vertices -- so a piece has as many 0x31 nodes as
                        # joints touching it, and a vertex is named by several of them.
                        # (The reference takes vw[0] only and applies that single matrix
                        # to the whole piece, which is why it warps multi-joint models.)
                        vw = _by_type(spn, T_VERTEX_WEIGHTS)
                        if vw and bind_off > 0:
                            skin = [[] for _ in range(vcount)]
                            for w in vw:
                                recs = w.data + _u32(buf, w.data)
                                joint = _u16(buf, w.data + 0x4)
                                if joint >= bl_count:
                                    raise ValueError(f"{path}: joint {joint} >= bone "
                                                     f"list {bl_count}")
                                for i in range(_u16(buf, w.data + 0x6)):
                                    off, wt = struct.unpack_from("<If", buf, recs + i * 8)
                                    # Lists are tail-padded with zero-weight entries that
                                    # all point at offset 0; 1152/9981 records in the
                                    # corpus. Keeping them would double-count vertex 0.
                                    if wt == 0.0:
                                        continue
                                    vi = off // 0x10   # byte offset into the vec4 block
                                    if vi >= vcount:
                                        raise ValueError(f"{path}: weight vertex {vi} "
                                                         f">= vcount {vcount}")
                                    skin[vi].append((joint, wt))
                            p.skin = skin
                            p.joints = bone_list
                            # The 0x46 table is the INVERSE BIND matrix: composing it
                            # with the joint's world transform gives exactly identity on
                            # 326/336 joints in the corpus (i.e. wherever the file's
                            # skeleton is stored at its bind pose). The 10 that don't are
                            # LOD/prop files posed away from bind, and there the product
                            # is a plain rigid transform -- which is the correct rest
                            # pose, not an error.
                            p.ibm = [transform(bind_off, t) for t in range(bl_count)]
                            p.skin_mats = [transform(xform_table, bone_list[t]) * p.ibm[t]
                                           for t in range(bl_count)]

                        ind = []
                        for il in _by_type(spn, T_INDEX_LIST):
                            n = _u8(buf, il.data + 0x5)
                            # UVs are one-per-index-entry: ucount == n on 10788/10788
                            # index lists in the corpus. A few untextured pieces have an
                            # empty second child and so no UV buffer at all; pad to keep
                            # `p.vt` aligned with `ind`, since both accumulate across all
                            # the 0x47 lists of this piece. (The reference crashes here --
                            # Rule of Rose has no such piece.)
                            if il.children[1].children:
                                uvn = il.children[1].children[0]
                                p.vt.extend(_read_buffer(buf, uvn.data,
                                                         _u8(buf, uvn.data + 0x6)))
                            else:
                                p.vt.extend([[0.0, 0.0, 0.0, 0.0]] * n)
                            o = il.data + 0x10
                            for i in range(n):
                                ind.append(struct.unpack_from("<4B", buf, o + i * 4))

                        # Triangle strip. byte0 = vertex index, byte1 = 0x80 marks a
                        # vertex that only primes the strip, byte3 flips the winding.
                        for i in range(len(ind)):
                            _, ctrl, _, rev = ind[i]
                            if ctrl == 0x80:
                                continue
                            tri = ((i - 2, i - 1, i) if rev else (i, i - 1, i - 2))
                            p.tris.append([(ind[k][0], k) for k in tri])

    # Bone hierarchy. The 0x2a fixup handler (0x003a6d68) is a bare `jr ra` -- the
    # payload holds no pointers -- and the payload's first u16 is the PARENT index
    # (0xffff = root). Payloads overlap: bones with the same parent share one slot in
    # a pooled u16 array. Verified on 154/154 files: a valid single-rooted tree with
    # no cycles, and every parent index lower than its child's.
    bone_parent = []
    for bn in bones:
        v = _u16(buf, bn.data)
        bone_parent.append(-1 if v == 0xFFFF else v)
    bone_world = [transform(xform_table, i) for i in range(len(bones))]
    return Model(path, groups, materials, bone_parent, bone_world)


def write_obj(model, objpath):
    """Rest-pose OBJ. Bones and weights are lost -- use write_glb for those."""
    groups, materials = model.groups, model.materials
    mtlpath = os.path.splitext(objpath)[0] + ".mtl"
    os.makedirs(os.path.dirname(os.path.abspath(objpath)), exist_ok=True)
    nv = nt = 0
    with open(objpath, "w") as f:
        f.write(f"mtllib {os.path.basename(mtlpath)}\n\n")
        vlast = vtlast = 1
        for g in groups:
            f.write(f"g {g.name}\n")
            if g.material:
                f.write(f"usemtl {g.material}\n")
            for p in g.pieces:
                for v in p.model_space():
                    f.write(f"v {v[0]} {v[1]} {v[2]}\n")
                for vt in p.vt:
                    f.write(f"vt {vt[0]} {1 - vt[1]}\n")
                for tri in p.tris:
                    f.write("f " + " ".join(f"{vlast+a}/{vtlast+b}" for a, b in tri) + "\n")
                nv += len(p.vtx)
                nt += len(p.tris)
                vlast += len(p.vtx)
                vtlast += len(p.vt)
            f.write("\n")
    with open(mtlpath, "w") as f:
        for m in materials:
            f.write(f"newmtl {m}\nKa 0.5 0.5 0.5\n")
            f.write("Kd 1.0 1.0 1.0\n" if not m else f"map_Kd {m}.png\n")
            f.write("Ks 0.0 0.0 0.0\nNs 500\nillum 2\n\n")
    return nv, nt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out", default="extracted/obj")
    ap.add_argument("--stats", action="store_true", help="parse only, write nothing")
    args = ap.parse_args()

    nok = nfail = 0
    dests = dest_paths(args.files, args.out, ".obj")
    for p, dest in zip(args.files, dests):
        try:
            model = load(p)
            groups, materials = model.groups, model.materials
            nv = sum(len(pc.vtx) for g in groups for pc in g.pieces)
            nt = sum(len(pc.tris) for g in groups for pc in g.pieces)
            if not args.stats:
                nv, nt = write_obj(model, dest)
            skinned = sum(1 for g in groups for pc in g.pieces if pc.skin is not None)
            print(f"  OK   {os.path.basename(p):44s} groups={len(groups):4d} "
                  f"verts={nv:6d} tris={nt:6d} mats={len(materials)} "
                  f"bones={len(model.bone_parent):3d} skinned={skinned:3d}")
            nok += 1
        except Exception as ex:
            print(f"  FAIL {os.path.basename(p):44s} {type(ex).__name__}: {ex}")
            nfail += 1
    print(f"\n{nok} ok, {nfail} failed")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
