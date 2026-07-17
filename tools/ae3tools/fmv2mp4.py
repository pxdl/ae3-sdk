#!/usr/bin/env python3
"""Convert extracted FMVs (.m2v + .wav from `ae3 strextract`) to quality-first .mp4.

WHY RE-ENCODE AT ALL: MPEG-2 in .mp4 is spec-legal but QuickTime won't play it, so
`-c:v copy` yields an unplayable file. The bit-exact originals stay as .m2v/.mkv --
these .mp4s are the convenience copy. Nothing here overwrites the originals.

INTERLACING: the source is a mix, verified with ffmpeg's idet (not just trusting the
container flag):
  new_scene*/advertise/million  -> truly progressive (401 progressive / 0 TFF)
  new_play*/rc4/dolby_pl2       -> truly interlaced  (372 TFF / 0 progressive)
Interlaced files get yadif=1 (bob): every field becomes its own frame -> 59.94 fps.
yadif=0 would halve the frame rate and throw away half the real motion. Detected per
file from the stream rather than hardcoded.

ASPECT: SAR 7:6 -- the video's declared square samples are WRONG for how the PS2
showed it. Ground truth from the game executable (see docs/formats/FMV.md §5): the
game runs a 512x448 NTSC framebuffer (sceGsResetGraph(0,1,2,0) then w=512,h=448) on
a 4:3 TV, so SAR = (4/3)/(512/448) = 7:6 -- pixels 16.7% wider than tall. setsar
tags it rather than rescaling, so no resampling loss; the player does the stretch.

LEGAL: Sony's video. Personal study only; output stays local.
"""
import argparse
import glob
import os
import subprocess
import sys


def field_order(m2v):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=field_order", "-of", "default=nw=1", m2v],
        capture_output=True, text=True, check=True).stdout
    # `-of csv=p=0` returns "tt," -- with a trailing comma -- which silently fails
    # every string compare; parse key=value and strip instead.
    return out.split("=", 1)[-1].strip().strip(",")


def main():
    ap = argparse.ArgumentParser(
        description="Convert extracted .m2v+.wav FMV pairs to playable .mp4 "
                    "(deinterlace as needed, correct PS2 aspect, x264 CRF).")
    ap.add_argument("dir", nargs="?", default="extracted/fmv",
                    help="directory of .m2v/.wav pairs from `ae3 strextract` "
                         "(default: extracted/fmv)")
    ap.add_argument("out", nargs="?", default=None,
                    help="output directory (default: DIR/mp4)")
    ap.add_argument("--crf", default=os.environ.get("CRF", "15"),
                    help="x264 CRF (default 15: visually lossless here; source "
                         "is ~3.7 Mbps MPEG-2)")
    a = ap.parse_args()
    outdir = a.out if a.out is not None else os.path.join(a.dir, "mp4")
    os.makedirs(outdir, exist_ok=True)

    for m2v in sorted(glob.glob(os.path.join(a.dir, "*.m2v"))):
        n = os.path.splitext(os.path.basename(m2v))[0]
        wav = os.path.join(a.dir, n + ".wav")
        if not os.path.isfile(wav):
            print(f"  ! {n}: no .wav, skipping", file=sys.stderr)
            continue

        fo = field_order(m2v)
        if fo not in ("tt", "bb", "progressive"):
            print(f"  ! {n}: field_order='{fo}' unrecognised, refusing to guess",
                  file=sys.stderr)
            continue

        if fo in ("tt", "bb"):
            parity = "0" if fo == "tt" else "1"
            vf = f"yadif=mode=1:parity={parity},setsar=7/6,format=yuv420p"
            rate, note = "60000/1001", f"interlaced {fo} -> bob 59.94"
        else:
            vf = "setsar=7/6,format=yuv420p"
            rate, note = "30000/1001", "progressive 29.97"

        dst = os.path.join(outdir, n + ".mp4")
        # -r before -i: a raw ES has no timestamps, so the input rate must be
        # stated. -shortest: drops the ~0.89s of audio still in the stream buffer
        # when video ends (see docs/formats/FMV.md §3 -- expected, not a sync bug).
        subprocess.run(
            ["ffmpeg", "-y", "-v", "error", "-fflags", "+genpts",
             "-r", "30000/1001", "-i", m2v, "-i", wav,
             "-map", "0:v", "-map", "1:a",
             "-vf", vf, "-r", rate,
             "-c:v", "libx264", "-crf", str(a.crf), "-preset", "slow",
             "-pix_fmt", "yuv420p",
             "-c:a", "aac", "-b:a", "256k",
             "-movflags", "+faststart", "-shortest", dst],
            check=True)

        size = subprocess.run(["du", "-h", dst], capture_output=True,
                              text=True).stdout.split("\t")[0]
        print(f"  {n:<16} {note:<26} {size}")
    print(f"-> {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
