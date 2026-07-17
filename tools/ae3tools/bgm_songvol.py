#!/usr/bin/env python3
"""Per-song BGM song volumes from the game's own mastering database.

Parses static/exdb_sound/bgm_desc.exdb.exdb (schema ExdbBgmDesc) and prints
one line per distinct .mid:  <midi-stem> <songvol> <volume_scale>

songvol = trunc(127 * volume_scale * slider * dolby), the value the game writes
to the driver at cue start (verified live against the running game). Defaults
model the common listening condition: slider 1.0 (the in-game options menu
ranges 0..1.2; an options reset writes 0.7), stereo output (dolby factor 1.0;
Dolby Pro Logic II mode would be 0.6).

Usage: ae3 songvol --exdb PATH [--slider F] [--dolby]
"""
import argparse, sys

from . import exdb


def volume_scales(path):
    """-> {midi_stem: volume_scale}; asserts records agree on duplicates."""
    scales = {}
    for r in exdb.parse(path).records:
        if not r["midi"].endswith(".mid"):
            continue
        stem, vs = r["midi"][:-4], r["volume_scale"]
        if stem in scales and abs(scales[stem] - vs) > 1e-6:
            raise SystemExit(f"conflicting volume_scale for {stem}")
        scales[stem] = vs
    return scales


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slider", type=float, default=1.0,
                    help="bgm_volume options float (menu range 0..1.2; reset 0.7)")
    ap.add_argument("--dolby", action="store_true",
                    help="apply the Dolby Pro Logic II 0.6 BGM factor")
    ap.add_argument("--exdb", default=None, metavar="PATH",
                    help="path to bgm_desc.exdb.exdb (extracted from DATA.BIN "
                         "at static/exdb_sound/bgm_desc.exdb.exdb)")
    a = ap.parse_args()
    if a.exdb is None:
        ap.error("--exdb PATH is required (extract it with `ae3 extract`)")
    dolby = 0.6 if a.dolby else 1.0
    for stem, vs in sorted(volume_scales(a.exdb).items()):
        sv = int(127 * vs * a.slider * dolby)       # the game's float trunc
        print(f"{stem}\t{min(sv, 126)}\t{vs:.4g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
