#!/usr/bin/env python3
"""Decode the FMV subtitles (debug/us/movie/sceneNN.bin + .sbt) into SubRip (.srt).

Two files per cutscene, joined by index. NOTE the name mismatch: the movies are
`new_sceneNN.str` but the subtitles are `sceneNN.bin`/`sceneNN.sbt` (no `new_` prefix).

  sceneNN.sbt -- TIMINGS
    +0x00  "sbt\\0"
    +0x04  u32 count
    +0x08  f32 first_start, f32 total_duration   (duration matches the movie runtime --
                                                  that cross-check is what identifies the
                                                  unit as SECONDS)
    +0x10  count x { u32 index, u32 0, f32 start_sec, f32 end_sec }
    Exact: 0x10 + count*0x10 == filesize (752 == 16 + 46*16).

  sceneNN.bin -- TEXT. A generic typed-property container, magic 0x72312487.
    +0x00  u32 magic, u32 count, then u32 section offsets: names, index, records, text
    names   count x 0x28 : char[16] name ("subtitle_0"...), pad, u32 1, u32 -> index off
    index   count x 0x08 : u32 3, u32 -> record offset
    records count x 0x18 : typed fields; the LAST u32 is the offset into the text blob
    text    NUL-terminated **UTF-8**, '\\n' for line breaks

TEXT IS UTF-8, NOT ASCII. Decoding it as ASCII silently mangles every multi-byte char into
replacement glyphs -- U+3000 (ideographic space) becomes three "?" boxes on screen. Proof
it is really UTF-8 and not some custom codepage: the only non-ASCII bytes in the whole blob
are exactly 27 x 0xe3 and 54 x 0x80, i.e. 27 well-formed `e3 80 80` (U+3000) sequences and
nothing else; all 46 strings then decode with zero errors.

Every subtitle is exactly TWO lines. The game bottom-aligns short ones by padding the top
line with a lone U+3000 (27 of 46 in scene01); the rest carry text on both lines. That
spacer is a device for the game's own fixed 2-line renderer -- SRT already bottom-aligns,
so we drop whitespace-only lines rather than emit a blank first line.
    Every section lands where the arithmetic says (46*0x28 -> 0x760 ~ 0x770, 46*8 -> 0x8e0
    ~ 0x8f0, 46*0x18 -> 0xd40 ~ 0xd50), each padded to 16.

We walk the records directly (record i at records + i*0x18) rather than chasing the
name->index->record chain: the ordering is identical and it is one lookup instead of three.

LEGAL: the dialogue is Sony's. Personal study only; output stays local.

Usage:
  tools/sbt2srt.py --list
  ae3 sbt2srt --data DATA.BIN --all --out extracted/fmv/subs
"""
import argparse
import os
import struct
import sys

REC_SIZE = 0x18


def srt_time(t):
    if t < 0:
        t = 0.0
    h = int(t // 3600); m = int(t % 3600 // 60); s = int(t % 60)
    ms = int(round((t - int(t)) * 1000))
    if ms == 1000:
        s += 1; ms = 0
    return f"{h:02}:{m:02}:{s:02},{ms:03}"


def parse_sbt(d):
    if d[:4] != b"sbt\0":
        raise ValueError("bad sbt magic")
    n = struct.unpack_from("<I", d, 4)[0]
    first, total = struct.unpack_from("<2f", d, 8)
    if 0x10 + n * 0x10 != len(d):
        print(f"  ! sbt size {len(d)} != 0x10 + {n}*0x10", file=sys.stderr)
    out = []
    for i in range(n):
        idx, _z, a, b = struct.unpack_from("<IIff", d, 0x10 + i * 0x10)
        out.append((idx, a, b))
    return out, total


def parse_bin(d):
    magic, n = struct.unpack_from("<2I", d, 0)
    if magic != 0x72312487:
        raise ValueError(f"bad bin magic 0x{magic:08x}")
    # section offsets are the u32s at +0x08, +0x10, +0x18, +0x20 (each followed by a 0)
    _names, _index, records, text = (struct.unpack_from("<I", d, o)[0]
                                     for o in (0x08, 0x10, 0x18, 0x20))
    out = []
    for i in range(n):
        off = struct.unpack_from("<I", d, records + i * REC_SIZE + 0x14)[0]
        end = d.find(b"\0", text + off)
        # strict: a decode error means the codec assumption is wrong, and we want to hear
        # about it rather than paper over it with replacement glyphs.
        s = d[text + off:end].decode("utf-8")
        # Drop the game's vertical-alignment spacer lines (lone U+3000). Keep real text.
        lines = [ln for ln in s.split("\n") if ln.strip("　 \t")]
        out.append("\n".join(lines))
    return out


def convert(name, binp, sbtp, outdir):
    times, total = parse_sbt(open(sbtp, "rb").read())
    texts = parse_bin(open(binp, "rb").read())
    if len(times) != len(texts):
        print(f"  ! {name}: {len(times)} timings vs {len(texts)} strings", file=sys.stderr)
    n = min(len(times), len(texts))
    lines = []
    for i in range(n):
        _idx, a, b = times[i]
        lines.append(f"{i+1}\n{srt_time(a)} --> {srt_time(b)}\n{texts[i]}\n")
    os.makedirs(outdir, exist_ok=True)
    p = os.path.join(outdir, name + ".srt")
    with open(p, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"  {name}: {n} subtitles, last ends {times[n-1][2]:.2f}s (sbt total {total:.2f}s)")
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True, metavar="DATA_BIN",
                    help="path to DATA.BIN (on the game disc)")
    ap.add_argument("--manifest", default="extracted/databin/MANIFEST.tsv")
    ap.add_argument("--out", default="extracted/fmv/subs")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    files = {}
    for line in open(a.manifest):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 4 and p[0].startswith("debug/us/movie/") and p[0].endswith((".bin", ".sbt")):
            base, ext = os.path.splitext(os.path.basename(p[0]))
            files.setdefault(base, {})[ext] = (int(p[2]), int(p[3]))
    pairs = {k: v for k, v in files.items() if ".bin" in v and ".sbt" in v and k != "blank"}

    if a.list:
        for k, v in sorted(pairs.items()):
            print(f"  {k:12} bin={v['.bin'][1]:>6}B  sbt={v['.sbt'][1]:>5}B")
        print(f"  {len(pairs)} subtitle pairs")
        return
    if not a.all:
        print("use --all or --list", file=sys.stderr); sys.exit(1)

    tmp = "/tmp/_sbt"
    os.makedirs(tmp, exist_ok=True)
    with open(a.data, "rb") as f:
        for k, v in sorted(pairs.items()):
            paths = {}
            for ext in (".bin", ".sbt"):
                off, sz = v[ext]
                f.seek(off)
                paths[ext] = os.path.join(tmp, k + ext)
                open(paths[ext], "wb").write(f.read(sz))
            convert(k, paths[".bin"], paths[".sbt"], a.out)


if __name__ == "__main__":
    main()
