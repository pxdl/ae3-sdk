#!/usr/bin/env python3
"""SE (sound-effect) banks: inspect, census, decode waveforms to WAV.

Format spec: docs/formats/SE.md. Every field below is provenanced there against
the EE parser in SCUS_975.01: the SShd / "Jam" container (magic @+0x0C) shared
with BGM (BGM.md); the bank parser `vab_set` FUN_00402938 (a 6-slot chunk table
relocated in place to absolute pointers at bank+0x30..+0x44); `vab_get_seseq`
FUN_00402b18 (the two-level sequence chunk); and the SE note-on FUN_003f8690
(positional tone indexing, tone byte 0 = voice cut-group).

An SE bank's instrument data is byte-compatible with a BGM bank -- the .bd is
the same PS-ADPCM and the 16-byte tone record is identical -- but it carries its
OWN little sequences (the seseq chunk) instead of a separate .mid, and every
program is positional "drum-form" (header byte 0 = 0xFF): a cue plays a fixed
key, not a melodic range, so the note-on selects tone `(note - key0)` directly.
"""
import argparse
import struct
import sys
import wave
from pathlib import Path

SShd = b"SShd"
# PS-ADPCM, identical to the BGM bank decoder (BGM.md §4 / FMV.md).
COEF = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]
F_END, F_REPEAT, F_LOOPSTART = 0x01, 0x02, 0x04
SAMPLE_HZ = 44100

# Header chunk slots (s32 each, @+0x10). Names match BGM.md's header; SE fills the
# three that BGM leaves -1 (seseq/unk/seprog) and empties BGM's melodic prog slot.
SLOTS = ["prog", "vel", "lfo", "seseq", "unk", "seprog"]

# tone record flag bits (byte 15) -- same layout as BGM, but SE consumes bit 0.
F_REVERB, F_PROG_LFO, F_LFO_ON, F_PROG_BEND, F_CUT = 0x80, 0x40, 0x20, 0x10, 0x01


class SEError(ValueError):
    pass


def _jam_table(d, at):
    """A Jam offset table: s16 count (=LAST index) then count+1 u16 byte-offsets.
    Returns the raw u16 list (0xFFFF = empty slot)."""
    if not 0 <= at < len(d) - 2:
        raise SEError(f"chunk offset {at:#x} out of range")
    n = struct.unpack_from("<h", d, at)[0] + 1
    if n < 1 or at + 2 + n * 2 > len(d):
        raise SEError(f"chunk at {at:#x}: implausible count {n - 1}")
    return list(struct.unpack_from(f"<{n}H", d, at + 2))


def parse_bank(d, bd_size=None, name="?"):
    """Parse one .hd -> dict. Structural only; raises SEError on any violation."""
    if len(d) < 0x80 or d[0x0C:0x10] != SShd:
        raise SEError(f"{name}: no SShd magic at 0x0C")
    hd_size, bd_len, zero = struct.unpack_from("<3I", d, 0)
    if zero != 0:
        raise SEError(f"{name}: word@0x08 = {zero:#x}, expected 0")
    if bd_size is not None and bd_len != bd_size:
        raise SEError(f"{name}: bd_size field {bd_len} != .bd file {bd_size}")

    slots = dict(zip(SLOTS, struct.unpack_from("<6i", d, 0x10)))
    # The parser relocates each present slot to base+offset; -1 = absent.
    for nm in ("seseq", "unk", "seprog"):
        if slots[nm] == -1:
            raise SEError(f"{name}: SE chunk '{nm}' is absent (-1)")

    hybrid = slots["prog"] != -1          # mgs_saru also carries a BGM melodic prog chunk
    mode = 3 if struct.unpack_from("<i", d, 0x7c)[0] == -1 else 4  # vab_set FUN_00402938

    # space_c ships CORRUPT (SE.md §7): its unk chunk is bloated by 0x180, so the seprog
    # slot is stale and the .hd tail is 0x300 not 0x180. Report it rather than misparse.
    if len(d) - hd_size != 0x180:
        raise SEError(f"{name}: .hd tail {len(d) - hd_size:#x} != 0x180 -- corrupt bank "
                      f"(known: space_c, SE.md §7)")

    bank = {
        "name": name, "size": len(d), "hd_size": hd_size, "bd_size": bd_len,
        "slots": slots, "hybrid": hybrid, "mode": mode,
        "hd_tail": len(d) - hd_size,      # measured filesize - hd_size (0x180, or 0x300 space_c)
    }

    # --- SE program chunk (+0x24 seprog): drum-form programs, positional tones.
    bank["programs"] = _parse_programs(d, slots["seprog"], name)
    if hybrid:
        bank["programs_bgm"] = _parse_programs(d, slots["prog"], name, allow_empty=True)

    # --- velocity chunk (+0x14): s16 count + 128-byte curve (count 0, curve identity).
    v = slots["vel"]
    bank["velocity"] = d[v + 2: v + 2 + 128]

    # --- LFO chunk (+0x18): same Jam convention; 120-byte entries (60 pitch + 60 amp),
    # the final entry EOF-truncated to 64 bytes (60 pitch + 4). 43/101 banks carry one.
    bank["lfo"] = None
    if slots["lfo"] != -1:
        bank["lfo"] = [seseq_off for seseq_off in _jam_table(d, slots["lfo"]) if seseq_off != 0xFFFF]

    # --- sequence chunk (+0x1c seseq): two-level Jam chunk, cue -> layer -> event stream.
    bank["cues"] = _parse_seseq(d, slots["seseq"], slots["unk"], name)
    return bank


def _parse_programs(d, chunk, name, allow_empty=False):
    progs = []
    for i, o in enumerate(_jam_table(d, chunk)):
        if o == 0xFFFF:
            progs.append(None)
            continue
        a = chunk + o
        h = d[a:a + 8]
        if h[0] != 0xFF:
            if allow_empty:                 # BGM-style melodic prog (hybrid bank) -- not our focus
                progs.append({"melodic": True, "off": o})
                continue
            raise SEError(f"{name}: SE program {i} header byte0 {h[0]:#x} != 0xFF (not drum-form)")
        key0, keyN = h[6], h[7]
        n = keyN - key0 + 1
        tones = []
        for t in range(n):
            r = d[a + 8 + t * 16: a + 8 + t * 16 + 16]
            tones.append({
                "cut": r[0],                # byte 0: voice cut-group (SE-specific; 0 = none)
                "b1": r[1],                 # byte 1: voice-init param (SE-specific)
                "root": r[2], "fine": r[3] - 256 if r[3] > 127 else r[3],
                "addr": struct.unpack_from("<H", r, 4)[0],
                "adsr1": struct.unpack_from("<H", r, 6)[0],
                "adsr2": struct.unpack_from("<H", r, 8)[0],
                "vol": r[11], "pan": r[12],
                "bend": h[4] if (r[15] & F_PROG_BEND) else r[13],
                "lfo": h[5] if (r[15] & F_PROG_LFO) else r[14],
                "flags": r[15], "key": key0 + t,
            })
        progs.append({"vol": h[1], "key0": key0, "keyN": keyN, "tones": tones})
    return progs


def _parse_seseq(d, seseq, hi, name):
    """Two-level Jam chunk. BOTH cue and layer offset values are relative to the
    SESEQ BASE (vab_get_seseq: `return seseq + off`). Cue offsets are even (they
    are >>1'd into the u16 table); layer offsets are byte-granular event streams."""
    cues = []
    for ci, cue_off in enumerate(_jam_table(d, seseq)):
        if cue_off == 0xFFFF:
            cues.append(None)
            continue
        cue_at = seseq + cue_off
        if not (seseq < cue_at < hi) or cue_off & 1:
            raise SEError(f"{name}: cue {ci} offset {cue_off:#x} out of range / odd")
        layers = []
        for layer_off in _jam_table(d, cue_at):
            if layer_off == 0xFFFF:
                continue
            st = seseq + layer_off
            if not (seseq < st < hi):
                raise SEError(f"{name}: cue {ci} layer offset {layer_off:#x} out of range")
            end = d.find(b"\xff\x2f\x00", st, hi)   # FF 2F 00 = end-of-stream (as in SMF)
            layers.append({"off": layer_off, "bytes": d[st: end + 3] if end != -1 else None})
        cues.append(layers)
    return cues


def decode_adpcm(body, start):
    """Decode one waveform starting at byte `start`. -> (pcm ints, loop_off or None).
    Byte-identical to bgm.decode_adpcm (the gate oracle)."""
    pcm, h1, h2, loop = [], 0, 0, None
    p = start
    while p + 16 <= len(body):
        f = body[p]
        shift, filt, flag = f & 0x0F, (f >> 4) & 0x0F, body[p + 1]
        if shift > 12:
            shift = 9
        if filt > 4:
            filt = 0
        c0, c1 = COEF[filt]
        if flag & F_LOOPSTART:
            loop = len(pcm)
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
        p += 16
        if flag & F_END:
            if not flag & F_REPEAT:
                return pcm, None
            return pcm, (loop if loop is not None else 0)
    return pcm, None


def waveform_addrs(bank):
    """Unique .bd byte offsets referenced by the bank's tones, ascending."""
    seen = set()
    for prog in bank["programs"]:
        if prog is None:
            continue
        for t in prog["tones"]:
            if t["addr"] != 0xFFFF:
                seen.add(t["addr"] * 8)
    return sorted(seen)


def _write_wav(path, pcm):
    """Raw waveform to 44100 Hz mono s16 WAV, for inspection. Loop points are not
    embedded (SE playback is the next milestone); the loop offset from decode is
    what the gate checks against the oracle."""
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_HZ)
        w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))


# ------------------------------------------------------------------ subcommands
def cmd_info(bank):
    s = bank["slots"]
    print(f"{bank['name']}: {bank['size']} B .hd + {bank['bd_size']} B .bd   "
          f"mode {bank['mode']}{'  HYBRID(+BGM prog)' if bank['hybrid'] else ''}")
    print(f"  hd_size {bank['hd_size']}  (filesize - {bank['hd_tail']:#x}; driver ignores it)")
    print("  chunks: " + "  ".join(f"{k}={'-1' if v == -1 else hex(v)}" for k, v in s.items()))
    ident = bank["velocity"] == bytes(range(128))
    print(f"  velocity: 128 B, {'identity ramp' if ident else 'CUSTOM curve'}")
    print(f"  lfo: {len(bank['lfo']) if bank['lfo'] else 0} entries")
    progs = [p for p in bank["programs"] if p]
    ntones = sum(len(p["tones"]) for p in progs)
    print(f"  SE programs: {len(progs)}  tones: {ntones}  waveforms: {len(waveform_addrs(bank))}")
    for i, p in enumerate(bank["programs"]):
        if p is None:
            continue
        print(f"    prog {i}: keys {p['key0']}..{p['keyN']} ({len(p['tones'])} tones) vol {p['vol']}")
    live = [c for c in bank["cues"] if c]
    nl = sum(len(c) for c in live)
    print(f"  cues: {len(live)} live / {len(bank['cues'])} slots, {nl} layers")


def cmd_cues(bank):
    for ci, layers in enumerate(bank["cues"]):
        if not layers:
            continue
        print(f"cue {ci}: {len(layers)} layer(s)")
        for li, lyr in enumerate(layers):
            b = lyr["bytes"]
            head = " ".join(f"{x:02x}" for x in (b[:24] if b else []))
            print(f"    layer {li} (+{lyr['off']:#05x}, {len(b) if b else '?'} B): {head}"
                  f"{' ...' if b and len(b) > 24 else ''}")


def cmd_decode(bank, body, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    n = 0
    for a in waveform_addrs(bank):
        pcm, _loop = decode_adpcm(body, a)
        if not pcm:
            continue
        _write_wav(outdir / f"{bank['name']}_{a:06x}.wav", pcm)
        n += 1
    print(f"{bank['name']}: {n} waveforms -> {outdir}")


def cmd_census(root):
    hds = sorted(Path(root).rglob("*.hd"))
    print(f"{len(hds)} SE banks under {root}")
    tot_prog = tot_tone = tot_cue = tot_layer = tot_wave = lfo_banks = hybrids = 0
    fails = 0
    for p in hds:
        bd = p.with_suffix(".bd")
        try:
            bank = parse_bank(p.read_bytes(), bd.stat().st_size if bd.exists() else None, p.stem)
        except SEError as e:
            print("  FAIL", e)
            fails += 1
            continue
        progs = [x for x in bank["programs"] if x]
        tot_prog += len(progs)
        tot_tone += sum(len(x["tones"]) for x in progs)
        tot_wave += len(waveform_addrs(bank))
        live = [c for c in bank["cues"] if c]
        tot_cue += len(live)
        tot_layer += sum(len(c) for c in live)
        lfo_banks += 1 if bank["lfo"] else 0
        hybrids += 1 if bank["hybrid"] else 0
    print(f"  parsed {len(hds) - fails}/{len(hds)}   "
          f"programs {tot_prog}  tones {tot_tone}  waveforms {tot_wave}")
    print(f"  cues {tot_cue}  layers {tot_layer}  lfo-banks {lfo_banks}  hybrid {hybrids}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paths", nargs="+", type=Path, help=".hd file(s), or a directory with --census")
    ap.add_argument("--census", action="store_true", help="survey all .hd under a directory")
    ap.add_argument("--cues", action="store_true", help="dump the cue/layer/event structure")
    ap.add_argument("--decode", action="store_true", help="decode every waveform to WAV")
    ap.add_argument("-o", "--out", type=Path, help="output directory for --decode")
    a = ap.parse_args(argv)

    if a.census:
        for root in a.paths:
            cmd_census(root)
        return 0

    for p in a.paths:
        bd = p.with_suffix(".bd")
        body = bd.read_bytes() if bd.exists() else b""
        try:
            bank = parse_bank(p.read_bytes(), len(body) if body else None, p.stem)
        except SEError as e:
            print("error:", e, file=sys.stderr)
            return 1
        if a.decode:
            cmd_decode(bank, body, a.out or Path(f"{p.stem}_se"))
        elif a.cues:
            cmd_cues(bank)
        else:
            cmd_info(bank)
    return 0


if __name__ == "__main__":
    sys.exit(main())
