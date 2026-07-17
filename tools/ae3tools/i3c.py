#!/usr/bin/env python3
"""Parse I3D_I3C -- the collision format of Ape Escape 3 / SG2. Exports OBJ.

Read out of SCUS_975.01, not guessed. FUN_003a7b88 memcmps 'I3D_I3C\\0' (@0x007201d8);
FUN_003a7d08 is the fixup routine and gives the entire pointer graph, and FUN_003a7c50
is its exact inverse (unfixup), which independently confirms every field below.

I3C is a THIRD header -- it shares neither BIN's nor I3M's. Note +0x10 is a *byte* flag
here, where I3M keeps its fixup state in a u16 at +0x16 and BIN uses a u32 at +0x0c.

    +0x00  'I3D_I3C\\0'
    +0x08  u32  version == 0x00030000
    +0x0c  u32  FILE SIZE                 (== len(file) on 50/50)
    +0x10  u8   fixup flag (1 = offsets on disk)   <- byte, per `*(char*)(iVar2+0x10)`
    +0x14  u16  COUNT_A  -> A records at +0x18, stride 0x0c
    +0x16  u16  COUNT_C  -> u32 array at +0x1c (collision material names)
    +0x18  ptr  A records
    +0x1c  ptr  name-pointer array

A record (12 bytes)  -- FUN_003a7d08 relocates +0x00 and +0x08:
    +0x00  ptr  NAME        ('ROOTNODE' on every file seen)
    +0x04  u16  COUNT_B  -> B records, stride 0x30
    +0x06  u16  ?
    +0x08  ptr  B records

B record (48 bytes)  -- FUN_003a7d08 relocates exactly +0x24, +0x28, +0x2c:
    +0x00  f32[4]  AABB CENTRE       (w = 1.0)
    +0x10  f32[4]  AABB HALF-EXTENT  (w = 0.0)
    +0x20  u32     ?  (0 on every file seen)
    +0x24  ptr     ?  (NULL on 50/50 -- unused in this corpus, purpose unknown)
    +0x28  ptr     BVH root       (fed to FUN_003a7bb8)
    +0x2c  ptr     VERTEX ARRAY   f32[4], stride 0x10

BVH node (12 bytes) -- FUN_003a7bb8 / FUN_003a7c50:
    +0x00  u16   COUNT; bit15 SET => leaf: do not recurse (`*param_1 & 0x8000`)
    +0x02  s8[3] quantised box CENTRE       -- relative to the PARENT node's box
    +0x05  u8[3] quantised box HALF-EXTENT  -- as a fraction of the parent's half-extent
    +0x08  ptr   children (stride 0x0c) when internal; TRIANGLE when leaf

Leaf triangle (8 bytes):
    u16 v0, u16 v1, u16 v2, u16 TRI_ID

QUANTISED BOUNDS (see bvh_box / check_bounds)
--------------------------------------------
The node's 6 bytes are a centre/half-extent pair -- the SAME shape as the B record's
f32 centre + f32 half-extent, just quantised to s8/u8 -- expressed in its PARENT's box:

    box.centre = parent.centre + (s8_centre / 127) * parent.half
    box.half   =                 (u8_half   / 127) * parent.half

and the root node's parent frame is the B record's own f32 AABB. Nested frames are why
an earlier attempt to read these as an absolute s8 min/max pair failed: it is neither
min/max nor root-relative.

This was first derived from the data, and is now PROVEN against the collision query --
the recursive BVH descent at FUN_003bf9a0 (two callers: itself, and the public entry at
0x003bfd80). It reads the bounds instruction-for-instruction as documented here:
`lb` on +2/+3/+4 (signed centre) and `lbu` on +5/+6/+7 (unsigned half-extent); the
divisor is an explicit 1/127 constant in .rodata (*(float*)0x721760 = 0x3C010204 =
0.007874015..., 1/x = 127.0000005); and the freshly-decoded box is passed down as the
next level's parent frame, which is the nesting. Code and data agree exactly.

At each node the query dequantises the box and calls a caller-supplied predicate through
*(s3+0x0c), culling the subtree when it returns 0 -- so it is a GENERIC VISITOR, not a
hard-coded ray or sweep. Which predicate the player uses (and its epsilon) is still
unread; nothing in this conversion depends on it, since Godot rebuilds its own broadphase
from the triangle soup.

(Two claims that stood here previously were wrong and are retracted: that the query was
unlocated, and that the leaf test compiles to a signed `lh`/`bltz` rather than a mask. It
is `lhu s4,0(s1)` then `andi t7,s4,0x8000` at 0x003bfb04 -- exactly the mask below.)

The data evidence stands independently and still agrees:
  * containment holds for 47434 / 47434 nodes in all 50 files -- every node's decoded box
    contains every triangle beneath it;
  * the fit is TIGHT: 97.3% of axis checks sit within 1-2 quantisation units of the
    contents, i.e. the encoder rounded outward by a unit or two, exactly as a
    conservative quantiser must. (The 1.3% of looser ones are all axes whose parent
    half-extent is ~0.0001, where one unit is microscopic; their worst ABSOLUTE slack is
    0.11% of the part's size.)
  * the divisor is uniquely 127: /128 breaks containment on 47346 nodes and /126 on
    45458, because the scale moves the centre as well as the extent.

VERIFIED (all 50 files):
  * +0x0c == real file size, version == 0x00030000, fixup flag == 1
  * nverts == (ptr_0x28 - ptr_0x2c)/16 == max_vertex_index + 1     <- self-consistent
  * the TRI_ID field is exactly the permutation 0..ntris-1
  * every BVH node's decoded box contains its subtree's triangles (47434/47434)
  * col_cube_1x1x1.i3d -> AABB centre (0,0,0) half-extent (0.5,0.5,0.5) = a 1x1x1 cube,
    8 vertices at every combination of +-0.5, and 12 triangles that lie on exactly 6
    distinct axis-aligned planes, 2 per face. Ground truth from the file's own name.

LEGAL: reads Sony's data for personal study / clean-room analysis. Do not redistribute
extracted assets.
"""
import argparse
import os
import struct
import sys

from .i3d import dest_paths

VERSION_I3C = 0x00030000
NODE = 0x0C      # BVH node stride
BREC = 0x30      # B record stride
AREC = 0x0C      # A record stride
QUANT = 127.0    # BVH bound quantisation; uniquely 127 -- see the module docstring


def _cstr(b, o):
    z = b.find(b"\0", o)
    return b[o:z if z >= 0 else o].decode("ascii", "replace")


class Part:
    """One B record: an AABB, a vertex array and a BVH over triangles."""

    __slots__ = ("centre", "half_extent", "verts", "tris", "tree_off", "verts_off")

    def __init__(self, centre, half_extent, verts, tris, tree_off, verts_off):
        self.centre, self.half_extent = centre, half_extent
        self.verts, self.tris = verts, tris
        self.tree_off, self.verts_off = tree_off, verts_off


class I3c:
    def __init__(self, blob: bytes, path: str = "?"):
        self.b = blob
        self.path = path
        if blob[:8] != b"I3D_I3C\0":
            raise ValueError(f"{path}: not an I3D_I3C file")
        self.version, self.filesize = struct.unpack_from("<2I", blob, 8)
        if self.version != VERSION_I3C:
            raise ValueError(f"{path}: version {self.version:#010x} != {VERSION_I3C:#010x}")
        if self.filesize != len(blob):
            raise ValueError(f"{path}: +0x0c={self.filesize} != actual {len(blob)}")
        self.fixup = blob[0x10]
        if self.fixup != 1:
            raise ValueError(f"{path}: expected on-disk offsets (flag={self.fixup})")
        self.count_a, self.count_c = struct.unpack_from("<2H", blob, 0x14)
        self.a_off, self.names_off = struct.unpack_from("<2I", blob, 0x18)

        self.materials = [
            _cstr(blob, struct.unpack_from("<I", blob, self.names_off + i * 4)[0])
            for i in range(self.count_c)
        ]

        self.groups = []   # [(name, [Part, ...])]
        for i in range(self.count_a):
            n_ptr, count_b, _u6, b_off = struct.unpack_from("<I2HI", blob, self.a_off + i * AREC)
            parts = []
            for j in range(count_b):
                r = b_off + j * BREC
                centre = struct.unpack_from("<4f", blob, r)
                half = struct.unpack_from("<4f", blob, r + 0x10)
                _p24, tree, vptr = struct.unpack_from("<3I", blob, r + 0x24)
                # The vertex block runs from its own pointer up to the BVH root; there is
                # no explicit count anywhere in the record. Cross-checked against the
                # triangles' max index on 50/50 files -- see _check().
                nverts = (tree - vptr) // 0x10
                verts = [struct.unpack_from("<4f", blob, vptr + k * 0x10) for k in range(nverts)]
                parts.append(Part(centre, half, verts, self._walk(tree), tree, vptr))
            self.groups.append((_cstr(blob, n_ptr) if n_ptr else "", parts))

    def _walk(self, off, depth=0):
        """Enumerate BVH leaves -> triangles. Mirrors FUN_003a7bb8's recursion."""
        if depth > 64:
            raise ValueError(f"{self.path}: BVH deeper than 64 at {off:#x}")
        count, = struct.unpack_from("<H", self.b, off)
        ptr, = struct.unpack_from("<I", self.b, off + 8)
        if count & 0x8000:                       # leaf
            return [struct.unpack_from("<4H", self.b, ptr)]
        out = []
        for i in range(count & 0x7FFF):
            out += self._walk(ptr + i * NODE, depth + 1)
        return out

    def bvh_box(self, off, frame):
        """Decode a node's AABB. `frame` is the PARENT's (centre, half); for the root it
        is the B record's own AABB. Returns this node's (centre, half)."""
        q = struct.unpack_from("<3b", self.b, off + 2)      # s8 centre
        h = struct.unpack_from("<3B", self.b, off + 5)      # u8 half-extent
        fc, fh = frame
        centre = [fc[k] + (q[k] / QUANT) * fh[k] for k in range(3)]
        half = [(h[k] / QUANT) * fh[k] for k in range(3)]
        return centre, half

    def bvh_nodes(self, part):
        """Yield (offset, centre, half, is_leaf) for every BVH node of `part`."""
        def rec(off, frame, depth=0):
            if depth > 64:
                raise ValueError(f"{self.path}: BVH deeper than 64 at {off:#x}")
            centre, half = self.bvh_box(off, frame)
            count, = struct.unpack_from("<H", self.b, off)
            ptr, = struct.unpack_from("<I", self.b, off + 8)
            yield off, centre, half, bool(count & 0x8000)
            if not count & 0x8000:
                for i in range(count & 0x7FFF):
                    yield from rec(ptr + i * NODE, (centre, half), depth + 1)
        yield from rec(part.tree_off, (part.centre[:3], part.half_extent[:3]))

    def check(self):
        """The invariants that prove the decode. Returns a list of failures."""
        bad = []
        for gname, parts in self.groups:
            for p in parts:
                mx = max((max(t[:3]) for t in p.tris), default=-1)
                if len(p.verts) != mx + 1:
                    bad.append(f"nverts {len(p.verts)} != maxidx+1 {mx+1}")
                if sorted(t[3] for t in p.tris) != list(range(len(p.tris))):
                    bad.append("TRI_ID is not the permutation 0..N-1")
                bad += self.check_bounds(p)
        return bad

    def check_bounds(self, part):
        """Every BVH node's decoded box must contain every triangle beneath it. This is
        what pins the quantisation down -- a wrong frame or divisor fails immediately."""
        content = {}

        def gather(off, depth=0):
            count, = struct.unpack_from("<H", self.b, off)
            ptr, = struct.unpack_from("<I", self.b, off + 8)
            if count & 0x8000:
                t = struct.unpack_from("<4H", self.b, ptr)
                pts = [part.verts[i] for i in t[:3]]
                box = ([min(v[k] for v in pts) for k in range(3)],
                       [max(v[k] for v in pts) for k in range(3)])
            else:
                lo, hi = [1e30] * 3, [-1e30] * 3
                for i in range(count & 0x7FFF):
                    a, b = gather(ptr + i * NODE, depth + 1)
                    lo = [min(lo[k], a[k]) for k in range(3)]
                    hi = [max(hi[k], b[k]) for k in range(3)]
                box = (lo, hi)
            content[off] = box
            return box

        gather(part.tree_off)
        bad = []
        for off, centre, half, _leaf in self.bvh_nodes(part):
            lo, hi = content[off]
            for k in range(3):
                if centre[k] - half[k] > lo[k] + 1e-3 or centre[k] + half[k] < hi[k] - 1e-3:
                    bad.append(f"BVH box at {off:#x} does not contain its triangles")
                    return bad
        return bad


COL_MAGIC = b"AE3COL\0\0"
COL_VERSION = 1


def godot_normal(a, b, c):
    """The face normal Godot's collision gives triangle (a, b, c) -- SIGN MATTERS.

    Godot's convention is the NEGATION of the OpenGL / glTF one. Under the familiar
    n = (b-a) x (c-a), I3C's triangles look inward-facing: all 12 faces of the closed
    col_cube_1x1x1 point at its own centre, and coll_arabian_a reads 159 up against 7177
    down, i.e. a level built almost entirely of ceiling. Both readings are absurd, and
    both are artefacts of the sign, not of the data.

    Godot proves the point empirically: load col_cube_1x1x1 in FILE ORDER and drop a ray
    on it, and it hits y = +0.5 -- the top face, front-on. Reverse the winding and the
    same ray hits y = -0.5, the bottom. So Godot already treats file order as outward,
    and I3C needs NO winding flip. (This was nearly "fixed" the wrong way: reversing the
    winding to satisfy the OpenGL sign broke the cube, and the cube caught it. The
    evidence for the flip -- inward cube, ceiling-shaped level -- was real; the
    convention it was measured against was wrong.)

    Flipping the sign here makes both readings snap into place: the cube faces out, and
    coll_arabian_a becomes 7177 floors against 159 ceilings, which is what a level is.
    """
    u = [b[k] - a[k] for k in range(3)]
    v = [c[k] - a[k] for k in range(3)]
    n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
    return [-x for x in n]


def write_col(c: I3c, colpath: str):
    """Write OUR OWN compact binary for the Godot host to load at runtime.

    Not a Sony format -- this is a transport, and it is deliberately not OBJ:
      * OBJ round-trips every float through decimal text; this keeps the exact f32 bits;
      * it needs no Godot import step, so it loads identically in the editor, in an
        exported build and under --headless (where the .import pipeline may not have run);
      * it carries the collision MATERIAL NAME, which OBJ can only smuggle via usemtl.

    Layout (little-endian). Vertices are raw game units, w dropped (w == 0.0 on all
    15000 vertices in the corpus, checked -- there is no per-vertex payload):

        +0x00  'AE3COL\\0\\0'
        +0x08  u32  version == 1
        +0x0c  u32  nverts
        +0x10  u32  ntris
        +0x14  u32  mat_len          (bytes of ASCII material name, no NUL, may be 0)
        +0x18  u8[mat_len]           material, then pad to a 4-byte boundary
               f32[3] * nverts       positions
               u32[3] * ntris        triangle indices, in FILE ORDER (see godot_normal:
                                     I3C's winding already matches Godot -- do not flip)

    All 50 files in the corpus are exactly one group / one part, so the groups+parts
    nesting flattens with nothing lost; the assert below fails loudly if that ever
    stops being true rather than silently dropping geometry.
    """
    parts = [p for _g, ps in c.groups for p in ps]
    if len(parts) != 1:
        raise ValueError(f"{c.path}: expected 1 part, got {len(parts)} -- .col assumes a "
                         f"single mesh per file; extend the format rather than dropping parts")
    if len(c.materials) != 1:
        raise ValueError(f"{c.path}: expected 1 material, got {c.materials} -- the surface "
                         f"type is per-object in this corpus; .col carries exactly one")
    p = parts[0]
    mat = (c.materials[0] or "").encode("ascii")
    os.makedirs(os.path.dirname(os.path.abspath(colpath)), exist_ok=True)
    with open(colpath, "wb") as f:
        f.write(COL_MAGIC)
        f.write(struct.pack("<4I", COL_VERSION, len(p.verts), len(p.tris), len(mat)))
        f.write(mat)
        f.write(b"\0" * (-len(mat) % 4))
        for v in p.verts:
            f.write(struct.pack("<3f", v[0], v[1], v[2]))
        for t in p.tris:
            f.write(struct.pack("<3I", t[0], t[1], t[2]))
    return len(p.verts), len(p.tris)


def write_obj(c: I3c, objpath: str):
    os.makedirs(os.path.dirname(os.path.abspath(objpath)), exist_ok=True)
    nv = nt = 0
    with open(objpath, "w") as f:
        mat = c.materials[0] if c.materials and c.materials[0] else "collision"
        f.write(f"# I3D_I3C collision -- {os.path.basename(c.path)}\n")
        f.write(f"# collision material(s): {', '.join(m or '<empty>' for m in c.materials)}\n")
        base = 1
        for gname, parts in c.groups:
            for i, p in enumerate(parts):
                f.write(f"g {gname or 'part'}_{i}\nusemtl {mat}\n")
                for v in p.verts:
                    f.write(f"v {v[0]} {v[1]} {v[2]}\n")
                for t in p.tris:
                    f.write(f"f {base+t[0]} {base+t[1]} {base+t[2]}\n")
                base += len(p.verts)
                nv += len(p.verts)
                nt += len(p.tris)
    return nv, nt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--out", default="extracted/col")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("--format", choices=("obj", "col", "both"), default="both",
                    help="obj = viewable mesh; col = the binary the Godot host loads "
                         "at runtime (src/host/ae3_col.gd); both = default")
    args = ap.parse_args()

    nok = nfail = 0
    objs = dest_paths(args.files, args.out, ".obj")
    cols = dest_paths(args.files, args.out, ".col")
    for path, dest, coldest in zip(args.files, objs, cols):
        try:
            c = I3c(open(path, "rb").read(), path)
            bad = c.check()
            nv = sum(len(p.verts) for _, ps in c.groups for p in ps)
            nt = sum(len(p.tris) for _, ps in c.groups for p in ps)
            if not args.stats:
                if args.format in ("obj", "both"):
                    write_obj(c, dest)
                if args.format in ("col", "both"):
                    write_col(c, coldest)
            status = "OK  " if not bad else "BAD "
            print(f"  {status} {os.path.basename(path):46s} verts={nv:6d} tris={nt:6d} "
                  f"mat={','.join(m or '-' for m in c.materials)}"
                  + (f"  !! {'; '.join(bad)}" if bad else ""))
            nok += not bad
            nfail += bool(bad)
        except Exception as ex:
            print(f"  FAIL {os.path.basename(path):46s} {type(ex).__name__}: {ex}")
            nfail += 1
    print(f"\n{nok} ok, {nfail} failed")
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
