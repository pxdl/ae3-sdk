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
import math
import os
import struct
import sys
import tempfile

REC_SIZE = 0x18


def srt_time(t):
    if t < 0:
        raise ValueError(f"negative subtitle timestamp {t}")
    total_ms = round(t * 1000)
    h, remainder = divmod(total_ms, 3_600_000)
    m, remainder = divmod(remainder, 60_000)
    s, ms = divmod(remainder, 1000)
    return f"{h:02}:{m:02}:{s:02},{ms:03}"


def _require_range(data, offset, size, label):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(
            f"{label} range 0x{offset:x}..0x{offset + size:x} exceeds "
            f"0x{len(data):x}-byte file")


def parse_sbt(d):
    _require_range(d, 0, 0x10, "sbt header")
    if d[:4] != b"sbt\0":
        raise ValueError("bad sbt magic")
    n = struct.unpack_from("<I", d, 4)[0]
    expected_size = 0x10 + n * 0x10
    if expected_size != len(d):
        raise ValueError(f"sbt size {len(d)} != 0x10 + {n}*0x10")
    first, total = struct.unpack_from("<2f", d, 8)
    if not math.isfinite(first) or not math.isfinite(total) or first < 0 or total < first:
        raise ValueError(f"invalid sbt range {first}..{total}")

    out = []
    previous_start = -1.0
    for i in range(n):
        idx, zero, start, end = struct.unpack_from("<IIff", d, 0x10 + i * 0x10)
        if idx != i:
            raise ValueError(f"sbt cue {i} has index {idx}")
        if zero != 0:
            raise ValueError(f"sbt cue {i} reserved word is 0x{zero:08x}")
        if (not math.isfinite(start) or not math.isfinite(end) or
                start < 0 or start > end or end > total or start < previous_start):
            raise ValueError(f"sbt cue {i} has invalid range {start}..{end}")
        out.append((idx, start, end))
        previous_start = start
    if out and not math.isclose(first, out[0][1], rel_tol=0, abs_tol=1e-5):
        raise ValueError(f"sbt first timestamp {first} != cue 0 start {out[0][1]}")
    return out, total


def parse_bin(d):
    _require_range(d, 0, 0x28, "bin header")
    magic, n = struct.unpack_from("<2I", d, 0)
    if magic != 0x72312487:
        raise ValueError(f"bad bin magic 0x{magic:08x}")
    section_words = [struct.unpack_from("<2I", d, o) for o in (0x08, 0x10, 0x18, 0x20)]
    if any(zero != 0 for _, zero in section_words):
        raise ValueError("bin section offset has a nonzero reserved word")
    names, index, records, text = (offset for offset, _ in section_words)
    if not (0x28 <= names <= index <= records <= text <= len(d)):
        raise ValueError(
            f"invalid bin section order 0x{names:x}, 0x{index:x}, "
            f"0x{records:x}, 0x{text:x}")
    _require_range(d, names, n * 0x28, "bin names")
    _require_range(d, index, n * 0x08, "bin index")
    _require_range(d, records, n * REC_SIZE, "bin records")

    out = []
    for i in range(n):
        record = records + i * REC_SIZE
        off = struct.unpack_from("<I", d, record + 0x14)[0]
        start = text + off
        _require_range(d, start, 1, f"bin string {i}")
        end = d.find(b"\0", start)
        if end < 0:
            raise ValueError(f"bin string {i} at 0x{start:x} is not NUL-terminated")
        try:
            value = d[start:end].decode("utf-8", errors="strict")
        except UnicodeDecodeError as error:
            raise ValueError(
                f"bin string {i} has invalid UTF-8 at 0x{start + error.start:x}") from error
        lines = [line for line in value.split("\n") if line.strip("　 \t")]
        out.append("\n".join(lines))
    return out


def render_srt(times, texts):
    if len(times) != len(texts):
        raise ValueError(f"{len(times)} timings != {len(texts)} strings")
    cues = []
    for i, ((_idx, start, end), text) in enumerate(zip(times, texts), 1):
        cues.append(f"{i}\n{srt_time(start)} --> {srt_time(end)}\n{text}\n")
    return "\n".join(cues)


def convert_bytes(bin_data, sbt_data):
    times, total = parse_sbt(sbt_data)
    texts = parse_bin(bin_data)
    return render_srt(times, texts), times, total


def convert(name, binp, sbtp, outdir):
    with open(binp, "rb") as stream:
        bin_data = stream.read()
    with open(sbtp, "rb") as stream:
        sbt_data = stream.read()
    rendered, times, total = convert_bytes(bin_data, sbt_data)
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, name + ".srt")
    with open(path, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(rendered)
    last_end = times[-1][2] if times else 0.0
    print(f"  {name}: {len(times)} subtitles, last ends {last_end:.2f}s "
          f"(sbt total {total:.2f}s)")
    return path


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

    with tempfile.TemporaryDirectory(prefix="ae3-sbt-") as tmp, open(a.data, "rb") as f:
        for k, v in sorted(pairs.items()):
            paths = {}
            for ext in (".bin", ".sbt"):
                off, sz = v[ext]
                f.seek(off)
                paths[ext] = os.path.join(tmp, k + ext)
                with open(paths[ext], "wb") as output:
                    output.write(f.read(sz))
            convert(k, paths[".bin"], paths[".sbt"], a.out)


if __name__ == "__main__":
    main()
