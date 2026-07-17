#!/usr/bin/env python3
"""Parse the I3D_ asset family (models, animations, I3C) from Ape Escape 3 / SG2.

Layout read out of SCUS_975.01 -- NOT guessed. The tags are not lui/ori immediates
(tools/magicfind.py found zero); they live in .rodata as strings and are memcmp'd:

    'I3D_BIN\\0' @0x00720138  <- compared by FUN_003a7390   (model)
    'I3D_I3M\\0' @0x007201d0  <- compared by FUN_003a7620   (animation)
    'I3D_I3C\\0' @0x007201d8  <- compared by FUN_003a7b88   (?)

FUN_003a73e8 is the loader: it checks the tag, requires *(u32*)(f+8) == 0x00100001,
then treats f+0x0c as a FIXUP STATE flag (0 = offsets, 1 = pointers) and calls
FUN_003a85e0(f+0x10, f+0x10) to relocate. Both args are f+0x10, so **every offset in
the file is relative to f+0x10** and f+0x10 is the root node.

FUN_003a85e0 (fixup) / FUN_003a8490 (unfixup) give the node exactly -- it is a generic
16-byte tagged tree node, identical for all three tags:

    +0x00  u32  DATA      offset -> payload   (0 = none)
    +0x04  u32  packed    bits 0..23  = CHILD_COUNT
                          bits 24..30 = TYPE      (uVar1 >> 0x18 & 0x7f)
                          bit  31     = if SET, skip the type handler
                                        (`if (iVar7 != 0 && -1 < (int)uVar1)`)
    +0x08  u32  CHILDREN  offset -> child array, stride 0x10  (0 = none)
    +0x0c  u32  EXTRA     offset -> ?          (0 = none)

Children are walked recursively: `FUN_003a85e0(iVar4, param_2); iVar4 += 0x10`.

FUN_003a8408 validates the type: it returns 0 (reject) for a blacklist of 13 obsolete
types and 1 otherwise; the handler tables are 90 entries, so types are 0..0x59.
Two parallel tables of 90 function pointers, back to back:
    0x0069a1a8  unfixup handlers   (PTR_LAB_0069a1a8[type])
    0x0069a310  fixup   handlers   (PTR_LAB_0069a310[type])
Root node type for I3D_BIN must be 0x52 -- FUN_003a7390 checks byte f+0x17 & 0x7f.

Read-only. Never writes to the source assets.

LEGAL: parses Sony's data for personal study / clean-room analysis. Do not redistribute
extracted assets.
"""
import argparse
import os
import struct
import sys

TAGS = {b"I3D_BIN\0": "BIN", b"I3D_I3M\0": "I3M", b"I3D_I3C\0": "I3C"}


def dest_paths(files, outdir, ext):
    """Map input files to output paths, MIRRORING their directory structure.

    Basenames are not unique in this game's tree: `sw_yuka.i3d` exists under both
    toyhouse_c/bg and arabian_a/bg, and they are different models. Flattening to
    os.path.basename() silently overwrites 9 of the 154 I3D_BIN models and 3 of the 50
    I3D_I3C files -- the loss is invisible because every file still "converts OK".
    """
    absf = [os.path.abspath(f) for f in files]
    dirs = [os.path.dirname(a) for a in absf]
    root = os.path.commonpath(dirs) if len(set(dirs)) > 1 else dirs[0]
    return [os.path.join(outdir, os.path.splitext(os.path.relpath(a, root))[0] + ext)
            for a in absf]

# Each tag has its OWN version and its own header -- they do NOT share a layout.
#   BIN  0x00100001  FUN_003a73e8:  *(u32*)(f+8) == 0x100001
#   I3M  0x00020001  FUN_003a7668:  *(int*)(f+8) == 0x20001
#   I3C  0x00030000  (observed on 44/44 files; the I3C loader is not yet read)
VERSIONS = {"BIN": 0x00100001, "I3M": 0x00020001, "I3C": 0x00030000}
ROOT_TYPE_BIN = 0x52
BASE = 0x10          # BIN: root node offset == relocation base
NODE = 0x10          # BIN: sizeof(node) / child stride

# FUN_003a8408: these types are rejected by the loader.
BLACKLIST = {0x04, 0x16, 0x24, 0x2C, 0x2E, 0x2F, 0x36, 0x3F, 0x40, 0x41, 0x48, 0x4E, 0x51}
MAX_TYPE = 0x59


class Node:
    __slots__ = ("off", "data", "packed", "children_off", "extra", "children", "parent")

    def __init__(self, off, data, packed, children_off, extra):
        self.off = off
        self.data = data
        self.packed = packed
        self.children_off = children_off
        self.extra = extra
        self.children = []
        self.parent = None

    @property
    def type(self) -> int:
        return (self.packed >> 24) & 0x7F

    @property
    def count(self) -> int:
        return self.packed & 0xFFFFFF

    @property
    def skip_handler(self) -> bool:
        """bit31 set => the loader does NOT run the type's fixup handler."""
        return bool(self.packed & 0x80000000)

    @property
    def valid_type(self) -> bool:
        return self.type <= MAX_TYPE and self.type not in BLACKLIST

    def __repr__(self):
        return (f"<Node @0x{self.off:x} type=0x{self.type:02x} n={self.count} "
                f"data=0x{self.data:x}>")

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


class I3d:
    """An I3D_BIN model: the 16-byte tagged node tree. See I3m for animations."""

    def __init__(self, blob: bytes, path: str = "?"):
        self.b = blob
        self.path = path
        tag = blob[:8]
        if tag not in TAGS:
            raise ValueError(f"{path}: not an I3D file (tag {tag!r})")
        self.kind = TAGS[tag]
        if self.kind != "BIN":
            raise ValueError(f"{path}: {self.kind} is not a node tree -- use I3m")
        self.version, self.fixup_state = struct.unpack_from("<2I", blob, 8)
        if self.version != VERSIONS["BIN"]:
            raise ValueError(f"{path}: version 0x{self.version:08x} != 0x00100001")
        # BIN only: +0x0c is a fixup STATE (0 = offsets on disk, 1 = pointers).
        # For I3M/I3C that same word is the FILE SIZE -- different header.
        if self.fixup_state != 0:
            raise ValueError(f"{path}: already fixed up (state={self.fixup_state})")
        self.root = self._node(BASE)
        if self.root.type != ROOT_TYPE_BIN:
            raise ValueError(f"{path}: BIN root type 0x{self.root.type:02x} != 0x52")

    def _node(self, off: int, depth: int = 0, seen=None) -> Node:
        if seen is None:
            seen = set()
        if off in seen or depth > 64:
            raise ValueError(f"cycle/too deep at 0x{off:x}")
        seen.add(off)
        data, packed, children_off, extra = struct.unpack_from("<4I", self.b, off)
        n = Node(off, data, packed, children_off, extra)
        if not n.valid_type:
            raise ValueError(f"{self.path}: bad type 0x{n.type:02x} at 0x{off:x}")
        if n.children_off and n.count:
            base = n.children_off + BASE
            for i in range(n.count):
                c = self._node(base + i * NODE, depth + 1, seen)
                c.parent = n
                n.children.append(c)
        return n

    def payload(self, n: Node, size: int) -> bytes:
        """Bytes of a node's DATA block (offsets are relative to BASE)."""
        if not n.data:
            return b""
        o = n.data + BASE
        return self.b[o:o + size]

    def name(self, n: Node) -> str:
        """EXTRA is a pointer to a NUL-terminated name in the tail string table.

        Verified on ape_slm_body_c.i3d: the tail from EXTRA=0x1b99c reads
        b'\\0CombinedMesh0000\\0CombinedMeshInstance0000\\0sortOff__body1\\0...'
        and the sibling EXTRA values 0x1b9c7 / 0x1b9d6 land exactly on
        'sortOff__body1' (+43) and 'sortOff__body2' (+58).
        """
        if not n.extra:
            return ""
        o = n.extra + BASE
        z = self.b.find(b"\0", o)
        if z < 0:
            return ""
        return self.b[o:z].decode("ascii", "replace")

    def type_census(self) -> dict:
        c = {}
        for n in self.root.walk():
            c[n.type] = c.get(n.type, 0) + 1
        return c


class I3mTrack:
    __slots__ = ("index", "name", "u4", "nkeys", "keys_off")

    def __init__(self, index, name, u4, nkeys, keys_off):
        self.index, self.name, self.u4 = index, name, u4
        self.nkeys, self.keys_off = nkeys, keys_off

    def __repr__(self):
        return f"<Track {self.index} {self.name!r} keys={self.nkeys}>"


class I3m:
    """An I3D_I3M animation. Header read out of SCUS_975.01, not guessed.

    FUN_003a7680 (fixup) / FUN_003a7760 (unfixup) relocate exactly four pointers --
    +0x1c, +0x20, +0x24, +0x28 -- against param_1 = the FILE BASE (offset 0, unlike
    BIN's +0x10), and flip the u16 at +0x16 (1 = offsets on disk, 0 = pointers).
    They also walk TRACK_COUNT records of 12 bytes from +0x1c, relocating each
    record's +0x00 and +0x08.

        +0x00  'I3D_I3M\\0'
        +0x08  u32  version == 0x00020001        (FUN_003a7668)
        +0x0c  u32  FILE SIZE                    (matches len(file) on 104/104)
        +0x10  u16  ?                            (seen: 7, 13, 29, 31)
        +0x12  u16  COMPS -- u16 slots per key   (seen: 4 and 5)
        +0x14  u16  TRACK_COUNT                  (FUN_003a7860 = get_track_count)
        +0x16  u16  fixup state (1 = offsets)
        +0x18  f32  DURATION in seconds
        +0x1c  ptr  TRACKS -> TRACK_COUNT x 12
        +0x20  ptr  pool B  (element type NOT yet established -- see docs/formats/I3D.md)
        +0x24  ptr  pool A of f32 (FUN_003a7a40 indexes it *4)
        +0x28  ptr  ?

    Track (12 bytes):
        +0x00  ptr  NAME  -- proven by FUN_003a78f8 (find_track_by_name), which
                            strcmps the caller's string against *(u32*)(track+0)
        +0x04  u16  ?     (equals the header's COMPS on every track seen)
        +0x06  u16  KEY_COUNT                    (FUN_003a79e0)
        +0x08  ptr  KEYS -> KEY_COUNT x COMPS x u16

    Key component 0 is a f32 index into pool A, per FUN_003a7a40:
        idx = *(u16*)(track.keys + COMPS*key*2);  return *(f32*)(poolA + idx*4)
    Empirically comp0 decodes to exact key TIMES (0, 1/30, 2/30 ... DURATION).
    Components 1..COMPS-1 are NOT yet decoded -- no code has been found that reads
    them, so their meaning is deliberately left open rather than guessed.
    """

    def __init__(self, blob: bytes, path: str = "?"):
        self.b = blob
        self.path = path
        if blob[:8] != b"I3D_I3M\0":
            raise ValueError(f"{path}: not an I3D_I3M file")
        self.version, self.filesize = struct.unpack_from("<2I", blob, 8)
        if self.version != VERSIONS["I3M"]:
            raise ValueError(f"{path}: I3M version 0x{self.version:08x} != 0x00020001")
        self.h10, self.comps, self.ntracks, self.fixup_state = \
            struct.unpack_from("<4H", blob, 0x10)
        self.duration, = struct.unpack_from("<f", blob, 0x18)
        self.tracks_off, self.pool_b, self.pool_a, self.p28 = \
            struct.unpack_from("<4I", blob, 0x1c)
        if self.fixup_state != 1:
            raise ValueError(f"{path}: expected on-disk offsets (state={self.fixup_state})")
        self.tracks = []
        for t in range(self.ntracks):
            o = self.tracks_off + t * 12
            nptr, u4, nkeys, keys = struct.unpack_from("<I2HI", blob, o)
            self.tracks.append(I3mTrack(t, self._cstr(nptr), u4, nkeys, keys))

    def _cstr(self, o: int) -> str:
        z = self.b.find(b"\0", o)
        return self.b[o:z if z >= 0 else o].decode("ascii", "replace")

    def pool_a_floats(self) -> list:
        n = (len(self.b) - self.pool_a) // 4
        return list(struct.unpack_from(f"<{n}f", self.b, self.pool_a))

    def key_indices(self, t: I3mTrack) -> list:
        """KEY_COUNT rows of COMPS u16 slots."""
        out = []
        for k in range(t.nkeys):
            out.append(struct.unpack_from(f"<{self.comps}H", self.b,
                                          t.keys_off + k * self.comps * 2))
        return out

    def key_times(self, t: I3mTrack) -> list:
        """Component 0 -> pool A, exactly as FUN_003a7a40 does."""
        pool = self.pool_a_floats()
        return [pool[row[0]] for row in self.key_indices(t)]


def dump_i3m(a: I3m) -> None:
    print(f"=== {a.path} ===")
    print(f"  I3M v0x{a.version:08x}  filesize {a.filesize} (actual {len(a.b)})  "
          f"{'MATCH' if a.filesize == len(a.b) else 'MISMATCH'}")
    print(f"  +0x10={a.h10}  COMPS={a.comps}  TRACKS={a.ntracks}  DURATION={a.duration}s")
    print(f"  tracks=0x{a.tracks_off:x} poolB=0x{a.pool_b:x} poolA=0x{a.pool_a:x} "
          f"p28=0x{a.p28:x}")
    end = a.tracks_off + a.ntracks * 12
    print(f"  CHECK track table ends 0x{end:x}; first name at 0x{a.tracks[0].keys_off and 0:x}"
          f" -> table_end == first name offset: "
          f"{end == a.b.find(a.tracks[0].name.encode(), end)}")
    for t in a.tracks[:6]:
        times = a.key_times(t)
        print(f"    [{t.index:3d}] {t.name:16s} +4={t.u4} keys={t.nkeys:3d}  "
              f"t0={times[0]:.4f} tN={times[-1]:.4f}")
    if a.ntracks > 6:
        print(f"    ... {a.ntracks-6} more")


def dump(i: I3d, max_depth: int, limit: int) -> None:
    print(f"=== {i.path} ===")
    print(f"  kind {i.kind}  version 0x{i.version:08x}  fixup_state {i.fixup_state}  "
          f"size {len(i.b)}")
    shown = [0]

    def rec(n, d):
        if d > max_depth or shown[0] >= limit:
            return
        shown[0] += 1
        flag = " SKIP" if n.skip_handler else ""
        print(f"  {'  '*d}[0x{n.off:05x}] type=0x{n.type:02x} n={n.count:<4} "
              f"data=0x{n.data:06x} extra=0x{n.extra:06x}{flag}")
        for c in n.children:
            rec(c, d + 1)
    rec(i.root, 0)
    print(f"\n  --- type census ({len(list(i.root.walk()))} nodes) ---")
    for t, c in sorted(i.type_census().items()):
        print(f"    type 0x{t:02x}  x{c}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--depth", type=int, default=3)
    ap.add_argument("--limit", type=int, default=80)
    ap.add_argument("--census", action="store_true", help="only the aggregate type census")
    args = ap.parse_args()

    agg, bad = {}, 0
    for p in args.files:
        blob = open(p, "rb").read()
        if blob[:8] == b"I3D_I3M\0":
            try:
                dump_i3m(I3m(blob, p))
            except Exception as ex:
                print(f"  FAIL {p}: {ex}")
                bad += 1
            continue
        try:
            i = I3d(blob, p)
        except Exception as ex:
            print(f"  FAIL {p}: {ex}")
            bad += 1
            continue
        if args.census:
            for t, c in i.type_census().items():
                agg[(i.kind, t)] = agg.get((i.kind, t), 0) + c
        else:
            dump(i, args.depth, args.limit)
    if args.census:
        print(f"{'kind':>5} {'type':>6} {'count':>8}")
        for (k, t), c in sorted(agg.items()):
            print(f"{k:>5}   0x{t:02x} {c:>8}")
    if bad:
        print(f"\n{bad} file(s) failed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
