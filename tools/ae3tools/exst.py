#!/usr/bin/env python3
"""EXST (.x) streamed ADPCM: inspect, census, decode to WAV.

Format spec: docs/formats/EXST.md. Every header field below is provenanced
there against the EE parser (SCUS_975.01, sg2exst.c module at 0x3f6xxx):
channels s16@+0x06, rate u32@+0x08, loop flag u32@+0x0c, loop start and
stream length u32@+0x10/+0x14 in 2048-byte sectors, per-channel volume L/R
u32[8]@+0x18/+0x38, per-channel reverb flag u32[8]@+0x58. Payload is
PS-ADPCM in 2048-byte sectors, each split contiguously between the channels
(2048/channels bytes per channel per sector).

The decoder is the standard PS-ADPCM algorithm (same coefficient table and
clamping as the BGM bank decoder). Unlike bank decoding, streams are decoded
to the end of the payload: data flags do not terminate decode (the corpus
carries exactly two stray end flags mid-file, spec §2). 16 files overstate
the header length field (spec §4); the actual whole-sector payload on disk
is the trustworthy bound and is what gets decoded.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

MAGIC = b"EXST"
HDR = 0x78
SECTOR = 0x800
COEF = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]
F_END, F_REPEAT, F_LOOPSTART = 0x01, 0x02, 0x04


def parse_header(data: bytes, name: str = "?") -> dict:
    if data[:4] != MAGIC:
        raise ValueError(f"{name}: not an EXST file (magic {data[:4]!r})")
    ch = struct.unpack_from("<H", data, 0x06)[0]
    if not 1 <= ch <= 8:
        raise ValueError(f"{name}: channel count {ch} out of range 1..8")
    return {
        "channels": ch,
        "rate": struct.unpack_from("<I", data, 0x08)[0],
        "loop": struct.unpack_from("<I", data, 0x0C)[0],
        "loop_start": struct.unpack_from("<I", data, 0x10)[0],
        "length": struct.unpack_from("<I", data, 0x14)[0],
        "vol_l": list(struct.unpack_from("<8I", data, 0x18)),
        "vol_r": list(struct.unpack_from("<8I", data, 0x38)),
        "reverb": list(struct.unpack_from("<8I", data, 0x58)),
    }


def actual_sectors(filesize: int) -> int:
    return (filesize - HDR) // SECTOR


def deinterleave(payload: bytes, channels: int) -> list:
    """Per 2048-byte sector: 2048/channels contiguous bytes per channel."""
    if channels == 1:
        return [payload]
    per = SECTOR // channels
    outs = [bytearray() for _ in range(channels)]
    for s in range(0, len(payload) - SECTOR + 1, SECTOR):
        for c in range(channels):
            outs[c] += payload[s + c * per : s + (c + 1) * per]
    return [bytes(o) for o in outs]


def decode_stream(body: bytes) -> list:
    """PS-ADPCM decode of a whole channel stream. -> list of s16 samples."""
    pcm, h1, h2 = [], 0, 0
    for p in range(0, len(body) - 15, 16):
        f = body[p]
        shift, filt = f & 0x0F, (f >> 4) & 0x0F
        if shift > 12:
            shift = 9
        if filt > 4:
            filt = 0
        c0, c1 = COEF[filt]
        for i in range(14):
            b = body[p + 2 + i]
            for nib in (b & 0x0F, b >> 4):
                s = nib << 12
                if s & 0x8000:
                    s -= 0x10000
                s = (s >> shift) + ((h1 * c0 + h2 * c1) >> 6)
                s = -32768 if s < -32768 else (32767 if s > 32767 else s)
                pcm.append(s)
                h2, h1 = h1, s
    return pcm


def trailing_pad_frames(streams: list) -> int:
    """Trailing run of flag-2 silent pad frames, minimum across channels."""
    runs = []
    for st in streams:
        run = 0
        for p in range(len(st) - 16, -1, -16):
            if st[p + 1] == F_REPEAT and st[p + 2 : p + 16] == b"\x00" * 14:
                run += 1
            else:
                break
        runs.append(run)
    return min(runs) if runs else 0


def decode_file(data: bytes, name: str, trim_pad: bool = False):
    """-> (header, [per-channel pcm], warnings)"""
    h = parse_header(data, name)
    warns = []
    actual = actual_sectors(len(data))
    if h["length"] != actual:
        warns.append(f"header length {h['length']} sectors != actual payload {actual}; decoding actual")
    payload = data[HDR : HDR + actual * SECTOR]
    streams = deinterleave(payload, h["channels"])
    if trim_pad:
        pad = trailing_pad_frames(streams)
        if pad:
            streams = [st[: len(st) - pad * 16] for st in streams]
    return h, [decode_stream(st) for st in streams], warns


def write_wav(path: Path, chans: list, rate: int) -> None:
    n = min(len(c) for c in chans)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(len(chans))
        w.setsampwidth(2)
        w.setframerate(rate)
        if len(chans) == 1:
            frames = struct.pack(f"<{n}h", *chans[0][:n])
        else:
            inter = [0] * (n * len(chans))
            for c, pcm in enumerate(chans):
                inter[c :: len(chans)] = pcm[:n]
            frames = struct.pack(f"<{len(inter)}h", *inter)
        w.writeframes(frames)


def info(path: Path) -> None:
    data = path.read_bytes()
    h = parse_header(data, path.name)
    actual = actual_sectors(len(data))
    per = SECTOR // h["channels"]
    samples = actual * (per // 16) * 28
    dur = samples / h["rate"] if h["rate"] else 0.0
    line = (f"{path.name}: {h['channels']}ch {h['rate']} Hz, "
            f"{actual} sectors, {samples} samples/ch ({dur:.2f}s)")
    if h["loop"]:
        line += f", LOOP from sector {h['loop_start']}"
    if h["length"] != actual:
        line += f"  [WARN header says {h['length']} sectors]"
    ch = h["channels"]
    line += f", volL={h['vol_l'][:ch]} volR={h['vol_r'][:ch]}"
    if any(h["reverb"][:ch]):
        line += f", reverb={h['reverb'][:ch]}"
    print(line)


def census(root: Path, deep: bool = False) -> int:
    files = sorted(root.rglob("*.x"))
    if not files:
        print(f"no .x files under {root}", file=sys.stderr)
        return 1
    from collections import Counter

    rates, chans = Counter(), Counter()
    mismatch, badframes = [], 0
    nframes = 0
    for f in files:
        data = f.read_bytes()
        h = parse_header(data, f.name)
        rates[h["rate"]] += 1
        chans[h["channels"]] += 1
        actual = actual_sectors(len(data))
        if (len(data) - HDR) % SECTOR:
            mismatch.append((f.name, "payload not whole sectors"))
        elif h["length"] != actual:
            mismatch.append((f.name, f"header {h['length']} != actual {actual}"))
        if deep:
            pay = data[HDR:]
            for p in range(0, len(pay) - 15, 16):
                nframes += 1
                hd = pay[p]
                if (hd & 0x0F) > 12 or ((hd >> 4) & 0x0F) > 4:
                    badframes += 1
    print(f"{len(files)} files; channels {dict(chans)}; rates {dict(rates)}")
    print(f"length-field mismatches: {len(mismatch)}")
    for name, why in mismatch:
        print(f"  {name}: {why}")
    if deep:
        print(f"frames {nframes}, invalid shift/filter {badframes}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+", type=Path, help=".x file(s), or a directory with --census")
    ap.add_argument("--census", action="store_true", help="survey all .x under a directory")
    ap.add_argument("--deep", action="store_true", help="with --census: validate every ADPCM frame")
    ap.add_argument("--decode", action="store_true", help="decode to WAV")
    ap.add_argument("--trim-pad", action="store_true", help="drop the trailing silent-pad frames")
    ap.add_argument("-o", "--out", type=Path, help="output WAV path (single input) or directory")
    args = ap.parse_args()

    if args.census:
        rc = 0
        for p in args.paths:
            rc |= census(p, deep=args.deep)
        return rc

    for p in args.paths:
        if args.decode:
            data = p.read_bytes()
            h, chans, warns = decode_file(data, p.name, trim_pad=args.trim_pad)
            for w in warns:
                print(f"{p.name}: {w}", file=sys.stderr)
            if args.out and (args.out.is_dir() or len(args.paths) > 1):
                args.out.mkdir(parents=True, exist_ok=True)
                out = args.out / (p.stem + ".wav")
            else:
                out = args.out or p.with_suffix(".wav")
            write_wav(out, chans, h["rate"])
            print(f"{p.name} -> {out} ({h['channels']}ch {h['rate']} Hz, {min(len(c) for c in chans)} samples)")
        else:
            info(p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
