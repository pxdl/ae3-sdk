#!/usr/bin/env python3
"""Extract from DATA.BIN. Full pipeline: VFI -> deflate (.sz) -> PCK -> assets.

  DATA.BIN            VFI container, 3994 files, real directory tree
    └─ *.sz           [u32 decompressed_size][RAW DEFLATE][Adler-32 BE]  (zlib wbits=-15
                      inflates it; the trailer is ignored -- see DATA_BIN.md §2)
         └─ *.pck     PCK container -- every member declares a TYPE and ATTRIBUTES
              ├─ I3D_   3D model / animation / collision
              └─ TIM2   texture    (standard PS2 format, documented)

Each pck member carries an attribute string (see unpack_pck). Alongside the members we
write a PCK.tsv holding it verbatim -- member names are NOT unique and the attributes are
often the only thing telling two same-named blobs apart, so the sidecar, not the filename,
is the authority on what a file is.

Provenance: layout read from SCUS_975.01 (FUN_0010070c / FUN_00100958 / FUN_001008e0),
cross-checked against aluigi's ape_escape_vfi.bms, then verified empirically
(400/400 .sz decompress to their exact declared size). See docs/formats/DATA_BIN.md.

LEGAL: extracted data is Sony's. Personal study only. Do NOT redistribute any
extracted asset.

Usage:
  ae3 extract --data DATA.BIN --manifest                # list everything, extract nothing
  ae3 extract --data DATA.BIN --glob 'debug/us/stage/toyhouse_c/*' --out DIR
  tools/vfiextract.py --glob '*.irx' --out DIR --raw    # skip deflate/pck expansion
  tools/vfiextract.py --all --out DIR                   # everything (~2 GB)
"""
import argparse
import fnmatch
import os
import struct
import sys
import zlib

from .vfiparse import SECTOR, Vfi

# NOTE: the I3D_ family needs the FULL 8-byte tag, not the 4-byte prefix. Sniffing only
# b"I3D_" conflates three different formats with three different headers, and silently
# wrote animations out as models (ape_nrmb_run_b.i3d is an I3D_I3M). See docs/formats/I3D.md.
MAGICS = {
    b"MThd": "midi", b"\x7fELF": "irx", b"SShd": "sndbank", b"EXST": "stream",
    b"VFI\0": "vfi", b"TIM2": "texture", b"PCK\0": "pck",
    b"I3D_BIN\0": "model", b"I3D_I3M\0": "anim", b"I3D_I3C\0": "collision",
    b"I3D_": "i3d_unknown",   # keep last: only reached if the sub-tag is new
}


def sniff(b: bytes) -> str:
    for m, what in MAGICS.items():
        if b.startswith(m):
            return what
    return "?"


def safe_member(name: str) -> str:
    """A PCK member name reduced to a single, contained filename.

    Member names are data, not paths, and 11 of them in DATA.BIN start with '/'
    (asia_a's '/f_asi_ita04', all 8 of space_e's '/z_*', onsen_e's two) -- Sony's
    packer left the separator in. os.path.join(d, "/x") DISCARDS d and yields "/x",
    so the naive join walked out of --out and tried to write to the filesystem root.
    Take the basename and strip separators: '..' and nested paths cannot escape either.
    """
    name = name.replace("\\", "/").strip("/")
    name = os.path.basename(name)
    return name or "_unnamed"


def _cstr(data: bytes, off: int) -> str:
    z = data.find(b"\0", off)
    return data[off:z if z >= 0 else off].decode("ascii", "replace")


def unpack_pck(data: bytes):
    """PCK\\0 | u32 INFO_OFF | u32 FILES | ... ; entry = [name_off, ATTR_OFF, off, size].

    The second u32 was long labelled "flags" here and in docs/formats/DATA_BIN.md, with
    "0x12a = model, 0x2ec = texture" -- that was two lucky samples. It is NOT a bitfield:
    across all 36508 members it takes 1029 distinct values for one single kind and has no
    bit constant within a kind. It is a SECOND OFFSET into the same string pool as the
    name, and it points at a whitespace-separated ATTRIBUTE string whose first word is the
    asset's declared type:

        tm2                                        a texture
        i3r static bg prio=1000 start=0 end=10000  a model, with draw attributes
        cl2 pictname=meka_zenmai_o                 an extra palette for that texture
        i3c geom=ape_nrm_head_b parent=jnt_armL1   collision, bound to a bone

    Proof it is an offset and not a value: in ci_saveload/ui.pck the textures all carry
    0x10 and blob[0x10:] is literally "tm2"; the others carry 0x3f and blob[0x3f:] is
    "uis". The value is constant across a run of members and changes exactly where the
    kind changes.

    This matters twice over. The declared type is authoritative where magic-sniffing is
    blind (6574 members have no recognisable magic and were all being written as ".bin"),
    and `parent=`/`geom=` are the only place the character hitbox rig is written down.

    Returns (name, attrs, off, size); attrs is the raw string, `type_of`/`attrs_of` parse it.
    """
    if data[:4] != b"PCK\0":
        return None
    info_off, files = struct.unpack_from("<2I", data, 4)
    out = []
    for i in range(files):
        o = info_off + i * 16
        if o + 16 > len(data):
            break
        name_off, attr_off, off, size = struct.unpack_from("<4I", data, o)
        out.append((_cstr(data, name_off), _cstr(data, attr_off), off, size))
    return out


# Declared type -> the extension we write. Only the formats the project already has tools
# and docs for are renamed, and only to the name their MAGIC justifies: every "i3r" member
# is an I3D_BIN and there is no "i3d" type token in the archive, so i3r->i3d is a pure
# 1:1 rename that keeps tools/i3dgltf.py's *.i3d glob working. i3c_s and i3c are both
# I3D_I3C. The exact token is never lost -- PCK.tsv records it verbatim.
TYPE_EXT = {"i3r": "i3d", "i3c_s": "i3c"}


def type_of(attrs: str) -> str:
    """First word of the attribute string = the declared type. Mapped through TYPE_EXT."""
    toks = attrs.split()
    t = toks[0] if toks else "bin"
    return TYPE_EXT.get(t, t)


def attrs_of(attrs: str) -> dict:
    """The key=value tail. Bare words (static, bg, lighting, semitrans) map to True."""
    out = {}
    for t in attrs.split()[1:]:
        k, sep, v = t.partition("=")
        out[k] = v if sep else True
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True, metavar="DATA_BIN",
                    help="path to DATA.BIN (on the game disc)")
    ap.add_argument("--out", default="extracted/databin")
    ap.add_argument("--glob", help="match against the full path, e.g. '*/toyhouse_c/*'")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--manifest", action="store_true")
    ap.add_argument("--raw", action="store_true", help="do not expand .sz / .pck")
    args = ap.parse_args()

    f = open(args.data, "rb")
    v = Vfi(f)
    total = f.seek(0, 2)
    ents = v.entries()
    paths = [(v.full_path(e), e) for e in ents]

    if args.manifest:
        os.makedirs(args.out, exist_ok=True)
        mpath = os.path.join(args.out, "MANIFEST.tsv")
        with open(mpath, "w") as mf:
            mf.write("path\tsector\tbyte_offset\tsize\tkind\n")
            for p, e in paths:
                off = e["sector"] * SECTOR
                f.seek(off)
                mf.write(f"{p}\t{e['sector']}\t{off}\t{e['size']}\t{sniff(f.read(8))}\n")
        print(f"wrote {mpath}  ({len(paths)} files)")
        return 0

    if args.glob:
        sel = [(p, e) for p, e in paths if fnmatch.fnmatch(p, args.glob)]
    elif args.all:
        sel = paths
    else:
        ap.error("pick one of --manifest / --glob / --all")
    print(f"{len(sel)} file(s) selected")

    n_raw = n_sz = n_pck = 0
    for p, e in sel:
        off = e["sector"] * SECTOR
        if off + e["size"] > total:
            print(f"  SKIP out-of-range {p}")
            continue
        f.seek(off)
        blob = f.read(e["size"])
        dest = os.path.join(args.out, p)
        os.makedirs(os.path.dirname(dest), exist_ok=True)

        if not args.raw and p.endswith(".sz"):
            xsize = struct.unpack_from("<I", blob, 0)[0]
            try:
                blob = zlib.decompress(blob[4:], -15)
            except zlib.error as ex:
                print(f"  DEFLATE FAILED {p}: {ex}")
                continue
            if len(blob) != xsize:
                print(f"  SIZE MISMATCH {p}: {len(blob)} != {xsize}")
            dest = dest[:-3]  # drop .sz
            n_sz += 1

        members = None if args.raw else unpack_pck(blob)
        if members:
            d = dest[:-4] if dest.endswith(".pck") else dest + ".d"
            os.makedirs(d, exist_ok=True)
            # Member names are NOT unique and are not meant to be: 8 different collision
            # blobs all called col_ape_mg1_b are told apart only by parent=<bone>, and 579
            # pairs collide on name AND type with genuinely different bytes. Whatever we
            # name files, the pck's own table is the authority -- so write it out as a
            # sidecar and let the index break ties. Nothing is dropped.
            rows = []
            used = {}
            for i, (name, attrs, mo, ms) in enumerate(members):
                ext = type_of(attrs)
                stem = safe_member(name)
                fname = f"{stem}.{ext}"
                if fname in used:
                    fname = f"{stem}.{i:03d}.{ext}"
                used[fname] = True
                with open(os.path.join(d, fname), "wb") as g:
                    g.write(blob[mo:mo + ms])
                rows.append((i, fname, name, ext, ms, sniff(blob[mo:mo + 8]), attrs))
            with open(os.path.join(d, "PCK.tsv"), "w") as g:
                g.write("index\tfile\tname\ttype\tsize\tmagic\tattrs\n")
                for r in rows:
                    g.write("\t".join(str(x) for x in r) + "\n")
            n_pck += 1
            print(f"  {p} -> {len(members)} members")
        else:
            with open(dest, "wb") as g:
                g.write(blob)
            n_raw += 1

    print(f"\nwrote {n_raw} plain file(s), inflated {n_sz} .sz, expanded {n_pck} .pck -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
