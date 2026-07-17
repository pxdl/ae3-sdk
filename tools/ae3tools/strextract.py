#!/usr/bin/env python3
"""Extract FMVs from Ape Escape 3's `.str` movie container (debug/us/movie/*.str).

  DATA.BIN  ──VFI──>  debug/us/movie/*.str      "str\\0" container, 24 movies
                        ├─ MPEG-2 elementary video  (chunked, tag "Mpeg2Video")
                        └─ PS-ADPCM audio           (untagged, in the chunk gaps)

The `.str` files are NOT MPEG program streams -- there is no 0x000001BA pack header and no
PES layer anywhere in them. Video is a *raw MPEG-2 elementary stream* sliced into tagged
chunks (fed straight to the PS2's IPU), and audio is raw SPU2 ADPCM living in the untagged
space between them. So both halves are recovered by walking the chunk table, and ffmpeg
takes the resulting .m2v directly.

LAYOUT
  0x000  file header (see HDR below), rest of the 0x800 sector zero-padded
  0x800  audio preload -- hdr.preload bytes, untagged
         then, repeating: GroupOfDataInfo chunk, its Mpeg2Video chunks, an audio gap

CHUNK (uniform for every tag)
  +0x00  char[16]  tag, NUL-padded ("Mpeg2Video", "GroupOfDataInfo")
  +0x10  u32       index   -- video: field index of the chunk's first frame
  +0x14  u32       payload size in bytes
  +0x18  u32[2]    zero
  +0x20  payload, then padding to a 16-byte boundary

GroupOfDataInfo payload is u32[4] = (fields_in_group, video_chunks_in_group, ?, 0). The
third word does not match this group's video *or* audio byte count and is left unnamed --
nothing here needs it. See docs/formats/FMV.md for how the rest was cross-checked.

AUDIO. Each gap is `hdr.audio_blk` (0x4000) bytes of ADPCM followed by padding to the next
0x800 sector -- the gaps vary in length, the audio in them does not. The last group carries
no audio. This is not a guess: hdr.audio_total == preload + (groups-1) * audio_blk exactly
(0x44000 == 0x10000 + 13*0x4000), and the shortest gap in the file is exactly 0x4000.
Channels are interleaved in hdr.interleave (0x400) byte blocks; ADPCM predictor state is
per-channel and must persist across blocks.

FIELDS, NOT FRAMES. hdr.fields (251) counts *fields* at hdr.field_rate (5994 = 59.94Hz),
i.e. 125.5 frames at 29.97 -- it is not a frame count. Two independent checks agree:
the GroupOfDataInfo counts sum to exactly 251, and the MPEG-2 sequence header says
frame_rate_code=4 (29.97) with field_order=tt.

Provenance: format derived by structural analysis of the container (tags are self-describing)
and verified empirically -- the walk consumes every byte, the per-group chunk counts match
the chunks actually found, and ffmpeg decodes the result with zero errors. See docs/formats/FMV.md.

LEGAL: this is Sony's video. Personal study only. Output stays local,
never under assets/. Do NOT redistribute any extracted asset with the recreation.

Usage:
  tools/strextract.py --list
  ae3 strextract --data DATA.BIN --glob 'new_scene01' --out extracted/fmv
  ae3 strextract --data DATA.BIN --all --out extracted/fmv --mp4
"""
import argparse
import os
import re
import struct
import subprocess
import sys

SECTOR = 2048
TAGS = (b"Mpeg2Video", b"GroupOfDataInfo")

# PS/PS2 SPU ADPCM predictor coefficients, scaled by 1/64.
ADPCM_COEF = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))


class Hdr:
    """The 0x38-byte `.str` header. Names for words we have evidence for; rest stays raw."""

    def __init__(self, d: bytes):
        if d[:4] != b"str\0":
            raise ValueError("not a str container (bad magic)")
        w = struct.unpack_from("<14I", d, 0)
        self.fields = w[2]          # +0x08 total FIELDS (not frames) -- == sum of group counts
        self.field_rate = w[3]      # +0x0c 5994 = 59.94Hz field rate -> 29.97 fps
        self.groups = w[4]          # +0x10 GroupOfDataInfo count
        self.rate = w[8]            # +0x20 audio sample rate (48000)
        self.channels = w[9]        # +0x24 audio channels (2)
        self.interleave = w[10]     # +0x28 per-channel interleave block (0x400)
        self.audio_blk = w[11]      # +0x2c audio bytes per group gap (0x4000)
        self.preload = w[12]        # +0x30 audio preload before the first group (0x10000)
        self.audio_total = w[13]    # +0x34 total audio bytes == preload + (groups-1)*audio_blk

    @property
    def fps(self):
        return self.field_rate / 200.0   # fields/100 -> frames/sec

    @property
    def duration(self):
        return self.fields / (self.field_rate / 100.0)


def decode_adpcm(data: bytes) -> bytes:
    """PS-ADPCM -> signed 16-bit PCM. 16-byte frames, 28 samples each."""
    out = bytearray()
    s1 = s2 = 0
    for p in range(0, len(data) - 15, 16):
        hdr = data[p]
        shift, filt = hdr & 0x0F, (hdr >> 4) & 0x0F
        if shift > 12:      # SPU treats an out-of-range shift as 9
            shift = 9
        if filt > 4:        # unused filters behave as 0 rather than indexing off the table
            filt = 0
        f0, f1 = ADPCM_COEF[filt]
        for i in range(14):
            b = data[p + 2 + i]
            for nib in (b & 0x0F, b >> 4):          # low nibble first
                s = nib << 12
                if s & 0x8000:                       # sign-extend the 4-bit sample
                    s -= 0x10000
                s >>= shift
                s += (s1 * f0 + s2 * f1) >> 6
                s = max(-32768, min(32767, s))
                out += struct.pack("<h", s)
                s2, s1 = s1, s
    return bytes(out)


def wav(pcm_per_ch, rate):
    """Interleave per-channel PCM and wrap in a RIFF header."""
    ch = len(pcm_per_ch)
    n = min(len(p) for p in pcm_per_ch) // 2
    body = bytearray()
    for i in range(n):
        for c in range(ch):
            body += pcm_per_ch[c][i * 2:i * 2 + 2]
    ba = rate * ch * 2
    return (b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, ch, rate, ba, ch * 2, 16) +
            b"data" + struct.pack("<I", len(body)) + bytes(body))


def demux(d: bytes):
    """Walk the chunk table. Returns (hdr, video_es, audio_bytes, groups)."""
    h = Hdr(d)
    video, audio, groups = bytearray(), bytearray(d[SECTOR:SECTOR + h.preload]), []
    off = SECTOR + h.preload
    while off < len(d) - 32:
        tag = d[off:off + 16].rstrip(b"\0")
        if not (tag and all(32 <= b < 127 for b in tag)):
            # Untagged: an audio gap, laid out [zero padding][audio_blk of ADPCM] so the
            # audio ENDS flush against the next chunk. The padding LEADS -- reading
            # audio_blk from the start of the gap instead injects a burst of silence and
            # truncates real samples once per group (~0.3s), which is audible as stutter.
            nxt = min((x for x in (d.find(t, off) for t in TAGS) if x > 0), default=len(d))
            blk = min(h.audio_blk, nxt - off)          # this gap's audio, end-aligned
            want = min(blk, h.audio_total - len(audio))  # never exceed the declared total
            if want > 0:
                audio += d[nxt - blk:nxt - blk + want]
            off = nxt
            continue
        idx, size = struct.unpack_from("<II", d, off + 16)
        if tag == b"Mpeg2Video":
            video += d[off + 32:off + 32 + size]
        elif tag == b"GroupOfDataInfo":
            groups.append(struct.unpack_from("<4I", d, off + 32))
        off += 32 + ((size + 15) & ~15)
    return h, bytes(video), bytes(audio), groups


def extract(name, blob, outdir, mp4=False, quiet=False):
    h, video, audio, groups = demux(blob)

    # Consistency checks -- these are what proved the layout; keep them enforced.
    n_fields = sum(g[0] for g in groups)
    if n_fields != h.fields:
        print(f"  ! {name}: group fields {n_fields} != header {h.fields}", file=sys.stderr)
    if len(groups) != h.groups:
        print(f"  ! {name}: {len(groups)} groups != header {h.groups}", file=sys.stderr)

    os.makedirs(outdir, exist_ok=True)
    m2v = os.path.join(outdir, name + ".m2v")
    with open(m2v, "wb") as f:
        f.write(video)

    # Deinterleave -> per-channel ADPCM -> PCM. State is per channel, so decode each
    # channel's blocks as one continuous stream.
    ch = h.channels
    streams = [bytearray() for _ in range(ch)]
    blk = h.interleave
    for i in range(0, len(audio) - blk + 1, blk * ch):
        for c in range(ch):
            streams[c] += audio[i + c * blk:i + (c + 1) * blk]
    pcm = [decode_adpcm(bytes(s)) for s in streams]
    wavp = os.path.join(outdir, name + ".wav")
    with open(wavp, "wb") as f:
        f.write(wav(pcm, h.rate))

    asec = min(len(p) for p in pcm) / 2 / h.rate
    if not quiet:
        print(f"  {name}: {h.fields} fields / {h.fps:.2f}fps = {h.duration:.2f}s | "
              f"video {len(video):,}B | audio {len(audio):,}B -> {asec:.2f}s {h.rate}Hz x{ch}")

    if mp4:
        out = os.path.join(outdir, name + ".mkv")
        # -c:v copy: keep Sony's original MPEG-2 bitstream bit-exact, no second generation
        # of loss. A raw ES carries no timestamps, so -r states the rate and +genpts
        # synthesises the PTS the muxer refuses to write packets without.
        # -shortest drops the audio tail still sitting in the stream buffer when video ends.
        r = subprocess.run(
            ["ffmpeg", "-y", "-v", "error", "-fflags", "+genpts",
             "-r", f"{h.field_rate}/200", "-i", m2v, "-i", wavp,
             "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "flac",
             "-fps_mode", "passthrough", "-shortest", out],
            capture_output=True, text=True)
        if r.returncode:
            print(f"  ! {name}: ffmpeg failed: {r.stderr.strip()[:200]}", file=sys.stderr)
        elif not quiet:
            print(f"      -> {os.path.basename(out)} ({os.path.getsize(out):,}B)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True, metavar="DATA_BIN",
                    help="path to DATA.BIN (on the game disc)")
    ap.add_argument("--manifest", default="extracted/databin/MANIFEST.tsv")
    ap.add_argument("--out", default="extracted/fmv")
    ap.add_argument("--glob", help="substring match on the movie name")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--mp4", action="store_true", help="also mux to .mp4 via ffmpeg")
    a = ap.parse_args()

    movies = []
    for line in open(a.manifest):
        p = line.rstrip("\n").split("\t")
        if len(p) >= 4 and p[0].startswith("debug/us/movie/") and p[0].endswith(".str"):
            movies.append((os.path.basename(p[0])[:-4], int(p[2]), int(p[3])))

    if a.list:
        for n, off, sz in movies:
            print(f"  {n:22} @{off:>11,}  {sz:>12,} B")
        print(f"  {len(movies)} movies, {sum(s for _,_,s in movies):,} B total")
        return

    todo = movies if a.all else [m for m in movies if a.glob and a.glob in m[0]]
    if not todo:
        print("nothing selected (use --all, --glob, or --list)", file=sys.stderr)
        sys.exit(1)

    with open(a.data, "rb") as f:
        for n, off, sz in todo:
            f.seek(off)
            extract(n, f.read(sz), a.out, mp4=a.mp4)


if __name__ == "__main__":
    main()
