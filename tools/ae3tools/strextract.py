#!/usr/bin/env python3
"""Extract FMVs from Ape Escape 3's `.str` movie container (debug/us/movie/*.str).

  DATA.BIN  ──VFI──>  debug/us/movie/*.str      "str\\0" container, 22 movies
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

AUDIO. Each gap is zero padding followed by `hdr.audio_blk` (0x4000) bytes of ADPCM, with
the audio end-aligned against the next chunk. The gaps vary in length; the audio payload
does not. The last group carries no audio. This is not a guess: hdr.audio_total equals
preload + (groups-1) * audio_blk exactly, and every byte before each audio payload is zero.
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
  ae3 strextract --data DATA.BIN --all --out extracted/fmv --mkv
"""
import argparse
import math
import os
import re
import struct
import subprocess
import sys

try:
    from .sbt2srt import convert_bytes
except ImportError:
    from sbt2srt import convert_bytes

SECTOR = 2048
TAGS = (b"Mpeg2Video", b"GroupOfDataInfo")

# PS/PS2 SPU ADPCM predictor coefficients, scaled by 1/64.
ADPCM_COEF = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))


class Hdr:
    """The 0x38-byte `.str` header. Names for words we have evidence for; rest stays raw."""

    def __init__(self, d: bytes):
        if len(d) < SECTOR:
            raise ValueError(f"truncated str header sector: {len(d)} bytes")
        if d[:4] != b"str\0":
            raise ValueError("not a str container (bad magic)")
        if any(d[0x38:SECTOR]):
            raise ValueError("nonzero padding in str header sector at 0x38")
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
        if not all((self.fields, self.field_rate, self.groups, self.rate, self.channels,
                    self.interleave, self.audio_blk, self.preload, self.audio_total)):
            raise ValueError("zero value in required str header field")
        channel_group = self.interleave * self.channels
        if self.interleave % 16 or self.audio_blk % channel_group or self.preload % channel_group:
            raise ValueError("invalid channel/interleave arithmetic in str header")
        expected_audio = self.preload + (self.groups - 1) * self.audio_blk
        if self.audio_total != expected_audio:
            raise ValueError(
                f"declared audio total {self.audio_total} != preload/group total "
                f"{expected_audio}")
        if SECTOR + self.preload > len(d):
            raise ValueError(
                f"audio preload ends at 0x{SECTOR + self.preload:x}, past EOF 0x{len(d):x}")

    @property
    def fps(self):
        return self.field_rate / 200.0   # fields/100 -> frames/sec

    @property
    def duration(self):
        return self.fields / (self.field_rate / 100.0)


def decode_adpcm(data: bytes) -> bytes:
    """PS-ADPCM -> signed 16-bit PCM. 16-byte frames, 28 samples each."""
    if len(data) % 16:
        raise ValueError(f"ADPCM length {len(data)} is not a whole number of frames")
    out = bytearray(len(data) // 16 * 28 * 2)
    write = 0
    s1 = s2 = 0
    for p in range(0, len(data), 16):
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
                sample = nib << 12
                if sample & 0x8000:                  # sign-extend the 4-bit sample
                    sample -= 0x10000
                sample >>= shift
                sample += (s1 * f0 + s2 * f1) >> 6
                sample = max(-32768, min(32767, sample))
                struct.pack_into("<h", out, write, sample)
                write += 2
                s2, s1 = s1, sample
    return bytes(out)


def wav(pcm_per_ch, rate):
    """Interleave per-channel PCM and wrap in a RIFF header."""
    if not pcm_per_ch or any(len(pcm) != len(pcm_per_ch[0]) for pcm in pcm_per_ch):
        raise ValueError("PCM channels have unequal lengths")
    ch = len(pcm_per_ch)
    samples = len(pcm_per_ch[0]) // 2
    body = bytearray(samples * ch * 2)
    write = 0
    for i in range(samples):
        for pcm in pcm_per_ch:
            body[write:write + 2] = pcm[i * 2:i * 2 + 2]
            write += 2
    byte_rate = rate * ch * 2
    return (b"RIFF" + struct.pack("<I", 36 + len(body)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, ch, rate, byte_rate, ch * 2, 16) +
            b"data" + struct.pack("<I", len(body)) + bytes(body))


def _chunk(d, off, expected_tag):
    if off + 32 > len(d):
        raise ValueError(f"truncated {expected_tag.decode()} header at 0x{off:x}")
    tag = d[off:off + 16].rstrip(b"\0")
    if tag != expected_tag:
        raise ValueError(
            f"expected {expected_tag.decode()} at 0x{off:x}, found {tag!r}")
    _index, size, reserved0, reserved1 = struct.unpack_from("<4I", d, off + 16)
    if reserved0 or reserved1:
        raise ValueError(f"nonzero chunk reserved word at 0x{off + 0x18:x}")
    payload = off + 32
    padded_end = payload + ((size + 15) & ~15)
    if payload + size > len(d) or padded_end > len(d):
        raise ValueError(
            f"truncated {expected_tag.decode()} payload at 0x{payload:x}: "
            f"{size} bytes exceed EOF 0x{len(d):x}")
    if any(d[payload + size:padded_end]):
        raise ValueError(f"nonzero chunk padding at 0x{payload + size:x}")
    return d[payload:payload + size], padded_end


def demux(d: bytes):
    """Walk and fully validate the chunk table. Returns (hdr, video, audio, groups)."""
    h = Hdr(d)
    video_parts = []
    audio_parts = [d[SECTOR:SECTOR + h.preload]]
    groups = []
    video_chunks = 0
    off = SECTOR + h.preload

    for group_index in range(h.groups):
        payload, off = _chunk(d, off, b"GroupOfDataInfo")
        if len(payload) != 16:
            raise ValueError(
                f"group {group_index} payload is {len(payload)} bytes instead of 16")
        group = struct.unpack("<4I", payload)
        if group[3] != 0:
            raise ValueError(f"group {group_index} reserved word is 0x{group[3]:08x}")
        groups.append(group)

        for _ in range(group[1]):
            payload, off = _chunk(d, off, b"Mpeg2Video")
            video_parts.append(payload)
            video_chunks += 1

        if group_index < h.groups - 1:
            next_group = d.find(b"GroupOfDataInfo\0", off)
            if next_group < 0:
                raise ValueError(
                    f"group {group_index} has no following group after 0x{off:x}")
            gap_size = next_group - off
            if gap_size < h.audio_blk:
                raise ValueError(
                    f"audio gap at 0x{off:x} is {gap_size} bytes, "
                    f"smaller than {h.audio_blk}")
            padding_end = next_group - h.audio_blk
            if any(d[off:padding_end]):
                raise ValueError(f"nonzero leading audio-gap padding at 0x{off:x}")
            audio_parts.append(d[padding_end:next_group])
            off = next_group

    if any(d[off:]):
        raise ValueError(f"nonzero trailing data starts at 0x{off:x}")
    if len(groups) != h.groups:
        raise ValueError(f"walked {len(groups)} groups, header declares {h.groups}")
    fields = sum(group[0] for group in groups)
    if fields != h.fields:
        raise ValueError(f"group fields {fields} != header {h.fields}")
    declared_chunks = sum(group[1] for group in groups)
    if video_chunks != declared_chunks:
        raise ValueError(f"walked {video_chunks} video chunks, groups declare {declared_chunks}")

    audio = b"".join(audio_parts)
    if len(audio) != h.audio_total:
        raise ValueError(f"walked {len(audio)} audio bytes, header declares {h.audio_total}")
    return h, b"".join(video_parts), audio, groups


def _display_aspect(m2v):
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-of", "csv=p=0", m2v],
        capture_output=True, text=True, check=True)
    try:
        width, height = (int(value) for value in result.stdout.strip().split(",")[:2])
    except (TypeError, ValueError) as error:
        raise RuntimeError(f"ffprobe returned invalid dimensions: {result.stdout!r}") from error
    divisor = math.gcd(width * 7, height * 6)
    return width * 7 // divisor, height * 6 // divisor


def _read_exact(stream, offset, size, label):
    stream.seek(offset)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError(
            f"{label}: read {len(data)} bytes at 0x{offset:x}, expected {size}")
    return data


def _subtitle_key(movie_name):
    match = re.fullmatch(r"new_(scene\d\d)", movie_name)
    return match.group(1) if match else None


def _write_subtitle(movie_name, entries, stream, outdir):
    key = _subtitle_key(movie_name)
    if key is None:
        return None
    paths = [f"debug/us/movie/{key}{extension}" for extension in (".bin", ".sbt")]
    present = [path in entries for path in paths]
    if not any(present):
        return None
    if not all(present):
        raise ValueError(f"{movie_name}: incomplete subtitle pair for {key}")
    bin_data = _read_exact(stream, *entries[paths[0]], paths[0])
    sbt_data = _read_exact(stream, *entries[paths[1]], paths[1])
    rendered, _times, _total = convert_bytes(bin_data, sbt_data)
    subtitle_dir = os.path.join(outdir, "subs")
    os.makedirs(subtitle_dir, exist_ok=True)
    path = os.path.join(subtitle_dir, key + ".srt")
    with open(path, "w", encoding="utf-8", newline="\n") as output:
        output.write(rendered)
    return path


def extract(name, blob, outdir, mkv=False, subtitle=None, quiet=False):
    try:
        h, video, audio, _groups = demux(blob)
    except ValueError as error:
        raise ValueError(f"{name}: {error}") from error

    os.makedirs(outdir, exist_ok=True)
    m2v = os.path.join(outdir, name + ".m2v")
    with open(m2v, "wb") as stream:
        stream.write(video)

    # Deinterleave -> per-channel ADPCM -> PCM. State is per channel, so decode each
    # channel's blocks as one continuous stream.
    channels = h.channels
    block = h.interleave
    channel_group = block * channels
    if len(audio) % channel_group:
        raise ValueError(
            f"{name}: audio length {len(audio)} is not divisible by "
            f"{channels} channels x {block}-byte interleave")
    streams = [bytearray(len(audio) // channels) for _ in range(channels)]
    write_offsets = [0] * channels
    for offset in range(0, len(audio), channel_group):
        for channel in range(channels):
            start = offset + channel * block
            write = write_offsets[channel]
            streams[channel][write:write + block] = audio[start:start + block]
            write_offsets[channel] += block
    pcm = [decode_adpcm(bytes(stream)) for stream in streams]
    wav_path = os.path.join(outdir, name + ".wav")
    with open(wav_path, "wb") as stream:
        stream.write(wav(pcm, h.rate))

    audio_seconds = len(pcm[0]) / 2 / h.rate
    if not quiet:
        print(f"  {name}: {h.fields} fields / {h.fps:.2f}fps = {h.duration:.2f}s | "
              f"video {len(video):,}B | audio {len(audio):,}B -> "
              f"{audio_seconds:.2f}s {h.rate}Hz x{channels}")

    if not mkv:
        return

    aspect_width, aspect_height = _display_aspect(m2v)
    output_path = os.path.join(outdir, name + ".mkv")
    command = [
        "ffmpeg", "-y", "-v", "error", "-fflags", "+genpts",
        "-r", f"{h.field_rate}/200", "-i", m2v, "-i", wav_path,
    ]
    if subtitle:
        command.extend(["-i", subtitle])
    command.extend([
        "-map", "0:v", "-map", "1:a", "-c:v", "copy", "-c:a", "flac",
        "-aspect", f"{aspect_width}:{aspect_height}",
        "-fps_mode", "passthrough", "-shortest",
    ])
    if subtitle:
        command.extend([
            "-map", "2:s", "-c:s", "srt",
            "-metadata:s:s:0", "language=eng", "-disposition:s:0", "default",
        ])
    command.append(output_path)
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{name}: ffmpeg failed: {result.stderr.strip()}")
    if not quiet:
        print(f"      -> {os.path.basename(output_path)} "
              f"({os.path.getsize(output_path):,}B)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True, metavar="DATA_BIN",
                    help="path to DATA.BIN (on the game disc)")
    ap.add_argument("--manifest", default="extracted/databin/MANIFEST.tsv")
    ap.add_argument("--out", default="extracted/fmv")
    ap.add_argument("--glob", help="substring match on the movie name")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--mkv", action="store_true",
                    help="also mux a lossless MPEG-2/FLAC/SubRip .mkv via ffmpeg")
    a = ap.parse_args()

    entries = {}
    movies = []
    with open(a.manifest, encoding="utf-8") as manifest:
        for line in manifest:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 4 or not parts[2].isdigit() or not parts[3].isdigit():
                continue
            path = parts[0]
            entries[path] = (int(parts[2]), int(parts[3]))
            if path.startswith("debug/us/movie/") and path.endswith(".str"):
                movies.append((os.path.basename(path)[:-4], *entries[path]))

    if a.list:
        for n, off, sz in movies:
            print(f"  {n:22} @{off:>11,}  {sz:>12,} B")
        print(f"  {len(movies)} movies, {sum(s for _,_,s in movies):,} B total")
        return

    todo = movies if a.all else [m for m in movies if a.glob and a.glob in m[0]]
    if not todo:
        print("nothing selected (use --all, --glob, or --list)", file=sys.stderr)
        sys.exit(1)

    with open(a.data, "rb") as stream:
        for name, offset, size in todo:
            blob = _read_exact(stream, offset, size, name + ".str")
            subtitle = _write_subtitle(name, entries, stream, a.out) if a.mkv else None
            extract(name, blob, a.out, mkv=a.mkv, subtitle=subtitle)


if __name__ == "__main__":
    main()
