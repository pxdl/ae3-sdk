#!/usr/bin/env python3
"""Generic parser for the game's "EDB" databases (*.exdb.exdb) -- the self-
describing tables that hold most of AE3's design data (574 files inside
DATA.BIN: BgmDesc, SeDesc, MonkeyProfile, MainCameraParam, boss/weapon
params, ...). Discovered while tracing BGM volume (the game's EE readers
follow FUN_001bb9a8's pattern: FUN_002bb3bc pulls fields BY NAME with a
per-field default).

Format (all little-endian, header is plain text):
    EDB\n
    stn:<SchemaName>:<nfields>:<nrecords>\n
    <t>:<byte offset>:<field name>\n        x nfields; t in {s,f,i} (str/f32/s32)
    sizest:<record size>\n
    b:<header block size>\n                 = file offset of record 0
    'U' (0x55) padding up to offset b, then nrecords x sizest raw records.
Strings are NUL-terminated char[32] (every observed schema spaces string fields
32 apart; the parser sizes each field to the gap to the next one anyway).

Library:  parse(path) -> Exdb(name, fields, records=[{field: value}, ...])
CLI:      exdb.py FILE... [--schema] [--tsv]     (default = aligned table)
"""
import argparse
import struct
import sys
from collections import namedtuple

Exdb = namedtuple("Exdb", "name fields records")   # fields = [(type, off, name)]


def _dedup(names):
    """Duplicate field names exist in shipped schemas (space_f 'priority',
    boss tables 'T_09'/'T_10'); suffix repeats so dict records keep every column."""
    seen, out = {}, []
    for n in names:
        k = seen.get(n, 0)
        seen[n] = k + 1
        out.append(n if k == 0 else f"{n}.{k}")
    return out


def parse(path):
    d = open(path, "rb").read()
    if not d.startswith(b"EDB"):
        raise ValueError(f"{path}: no EDB magic")
    # header text ends where the 'U' padding starts; split conservatively on the
    # first line that fails to parse
    lines = d[: d.index(b"U" * 4)].decode("ascii", errors="replace").split("\n")
    stn = next(l for l in lines if l.startswith("stn:"))
    _, name, nfields, nrecords = stn.split(":")
    size = int(next(l for l in lines if l.startswith("sizest:")).split(":")[1])
    base = int(next(l for l in lines if l.startswith("b:")).split(":")[1])
    fields = []
    for l in lines:
        p = l.split(":")
        if len(p) == 3 and p[0] in ("s", "f", "i") and p[1].isdigit():
            fields.append((p[0], int(p[1]), p[2]))
    if len(fields) != int(nfields):
        raise ValueError(f"{path}: header says {nfields} fields, parsed {len(fields)}")
    if base + int(nrecords) * size > len(d):
        raise ValueError(f"{path}: {nrecords} x {size}B records overrun the file")

    offs = sorted(o for _, o, _ in fields) + [size]
    span = {o: next(x for x in offs if x > o) - o for _, o, _ in fields}
    names = _dedup([n for _, _, n in fields])
    records = []
    for ro in range(base, base + int(nrecords) * size, size):
        r = d[ro : ro + size]
        rec = {}
        for (t, o, _), n in zip(fields, names):
            if t == "s":
                rec[n] = r[o : o + span[o]].split(b"\0")[0].decode("ascii",
                                                                   errors="replace")
            elif t == "f":
                rec[n] = struct.unpack_from("<f", r, o)[0]
            else:
                rec[n] = struct.unpack_from("<i", r, o)[0]
        records.append(rec)
    return Exdb(name, list(zip(fields, names)), records)


def _fmt(v):
    return f"{v:.6g}" if isinstance(v, float) else str(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--schema", action="store_true",
                    help="print schema (name, fields, record count) only")
    ap.add_argument("--tsv", action="store_true", help="tab-separated output")
    a = ap.parse_args()
    for path in a.files:
        e = parse(path)
        cols = [n for _, n in e.fields]
        if len(a.files) > 1 or a.schema:
            print(f"# {path}: {e.name}, {len(e.records)} records")
        if a.schema:
            for (t, o, _), n in e.fields:
                print(f"#   {t}:{o}:{n}")
            continue
        rows = [[_fmt(r[c]) for c in cols] for r in e.records]
        if a.tsv:
            print("\t".join(cols))
            for row in rows:
                print("\t".join(row))
        else:
            w = [max(len(c), *(len(r[i]) for r in rows)) if rows else len(c)
                 for i, c in enumerate(cols)]
            print("  ".join(c.ljust(x) for c, x in zip(cols, w)))
            for row in rows:
                print("  ".join(v.ljust(x) for v, x in zip(row, w)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
