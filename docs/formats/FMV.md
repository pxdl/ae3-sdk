# FMV — the `.str` movie container

The US and PAL retail discs each contain **22 `.str` movies** inside the
DATA.BIN (VFI) archive, under `debug/us/movie/*.str` and
`debug/uk/movie/*.str`, respectively. The two measured corpora share the same
header and tagged-video format but use two different audio-lane layouts:

- **Video** is a raw **MPEG-2 elementary stream** — Sony's original bitstream,
  recovered **bit-exact** (never re-encoded), sliced across tagged chunks.
- **Audio** is **PS-ADPCM, 48 kHz stereo**, stored in one or five untagged lanes
  between chunks and decoded one lane at a time.

Tools in the `ae3tools` package (`ae3` CLI):

```
ae3 strextract --list
ae3 strextract --glob new_scene01 --out <outdir> --mkv
ae3 strextract --all           --out <outdir> --mkv
```

`ae3 strextract` writes `<name>.m2v` (bit-exact original video), `<name>.wav`
(decoded audio), and with `--mkv` a lossless MPEG-2/FLAC `.mkv` tagged with the
proven 7:6 sample aspect. Matching subtitle sidecars are decoded and included as
SubRip automatically. `ae3 sbt2srt` can decode those sidecars separately (§6);
`ae3 fmv2mp4 --captions` makes playable captioned `.mp4` convenience copies (§5b).

The CLI examples above describe the US one-lane oracle. The browser SDK's
`extract-web/src/fmv.ts` inspector and demuxer cover both retail layouts
measured below.

> The extracted video is Sony's copyrighted content. This document specifies the
> container format only; the SDK ships no game data. See `NOTICE.md` for the
> project's data policy.

**The container self-describes.** Every measured movie contains literal ASCII
`Mpeg2Video` and `GroupOfDataInfo` tags inline in the data. The one trap:
**`.str` is not an MPEG program stream.** There is no `0x000001BA` pack header
or `0x000001E0` video PES header in the concatenated video payload of any of the
44 measured US/PAL retail files — only the MPEG elementary-stream start codes,
including `0x000001B3` sequence headers. ffmpeg/VLC therefore cannot open a
`.str` directly. Concatenating the `Mpeg2Video` payloads in file order yields
the `.m2v`.

```
DATA.BIN
  ├─ debug/us/movie/*.str      22 movies, one audio lane
  └─ debug/uk/movie/*.str      22 movies, one or five audio lanes
       ├─ Mpeg2Video chunks     raw MPEG-2 elementary stream
       └─ untagged regions      PS-ADPCM lane preloads and per-group blocks
```

## 1. File header (`0x38` bytes, rest of the 0x800 sector zero-padded)

The numeric examples below are from a one-lane US file. The fields have the
same offsets in both layouts; audio sizes are **per lane** in the five-lane
layout.

| off | example | meaning |
|---|---:|---|
| `+0x00` | `"str\0"` | magic |
| `+0x08` | 251 | total 59.94 Hz timeline ticks (§4; historically named `fields`) |
| `+0x0c` | 5994 | timeline tick rate ×100; this remains 5994 in PAL files |
| `+0x10` | 14 | `GroupOfDataInfo` count |
| `+0x20` | 48000 | audio sample rate |
| `+0x24` | 2 | channels per audio lane |
| `+0x28` | `0x400` | per-channel interleave block |
| `+0x2c` | `0x4000` | audio bytes per lane in each non-final group gap |
| `+0x30` | `0x10000` | preload bytes per audio lane |
| `+0x34` | `0x44000` | total audio bytes per lane |

Let `channel_group = channels * interleave`. The measured headers satisfy:

```
sample_rate == 48000
channels == 2
interleave % 16 == 0
audio_block % channel_group == 0
preload % channel_group == 0
audio_bytes == preload + (groups - 1) * audio_block
```

The decoder intentionally rejects other sample rates or channel counts: only
48 kHz stereo is proven in the measured corpora and in the WAV output contract.

`+0x14`/`+0x18`/`+0x1c` vary per file and remain unidentified. No parser
decision depends on them.

## 2. Chunk format (uniform for every tag)

```
+0x00  char[16]  exact NUL-padded tag ("Mpeg2Video" | "GroupOfDataInfo")
+0x10  u32       timeline index
+0x14  u32       payload size
+0x18  u32[2]    zero
+0x20  payload, then zero padding to a 16-byte boundary
```

The entire 16-byte tag field is exact: the name is followed only by zero bytes.
The two reserved words and every byte of payload padding are zero.

`GroupOfDataInfo` payload is `u32[4] =
(ticks_in_group, video_chunks_in_group, unknown, 0)`. Tick and chunk counts are
positive. Group header indices equal the cumulative tick count before that
group. Video indices are nondecreasing and fall within their containing
group's tick interval. The third payload word remains unidentified.

## 3. Audio lanes, preloads, and gaps

The number of audio lanes is not stored in a named header field. It is derived
from the only two first-group positions proven on retail data. With
`channel_group = channels * interleave`:

```
one-lane first group  = 0x800 + preload
five-lane first group = 0x800 + 5 * (preload + 2 * channel_group)
```

For the measured values (`preload=0x10000`, `channel_group=0x800`):

```
one lane:
  0x00000  header sector
  0x00800  lane 0 preload (0x10000)
  0x10800  first GroupOfDataInfo

five lanes:
  0x00000  header sector
  0x00800  five-lane lead-in (5 * 2 * 0x800 = 0x5000)
  0x05800  lane 0 preload (0x10000)
  0x15800  lane 1 preload
  0x25800  lane 2 preload
  0x35800  lane 3 preload
  0x45800  lane 4 preload
  0x55800  first GroupOfDataInfo
```

The `0x5000` five-lane lead-in is structurally bounded but not PS-ADPCM and its
semantics remain unidentified; it is never decoded. A different first-group
position is an unsupported layout, not a reason to scan arbitrarily for a tag.

After each non-final group's video chunks, both layouts use:

```
zero padding (< 0x800 bytes)
lane 0 audio_block
[lane 1 audio_block ... lane 4 audio_block]   # five-lane layout only
next GroupOfDataInfo
```

The audio aggregate is **end-aligned** against the next group. Across both
retail corpora the leading padding is `0x000`…`0x7f0`, always zero. Per-lane
audio totals still obey the header formula; the five-lane file physically
carries five times `audio_bytes`, excluding the unidentified lead-in. The SDK
demuxes lane 0 and validates every lane.

Every decoded range is a whole number of `channel_group` blocks and 16-byte
ADPCM frames. Proven frame headers have filter `0`…`4`, shift `0`…`12`, and
flags `0` or `2`; other values are rejected rather than coerced. Channels
interleave in `0x400` blocks. Predictor history is per channel and persists
across blocks and group boundaries. Codec arithmetic is standard PS/PS2 SPU
ADPCM: 16-byte frames produce 28 samples.

> **Do not read audio at the raw gap start.** In a one-lane gap that prepends
> zero padding and truncates the same amount of live audio. In a five-lane gap
> it also confuses padding and lane boundaries. Locate the next group, subtract
> `audio_tracks * audio_block`, then select the wanted lane.

### The delivered audio is slightly longer than the video

The selected lane starts at t=0; the preload is a streamer buffer, not a
timestamp offset. Across the measured retail files, decoded audio exceeds the
MPEG picture duration by about 0.89–1.19 s. Muxers may use `-shortest` to drop
the residue. Do not shift audio by the preload duration.

## 4. Timeline ticks, fields, and frames

`+0x08`, group payload word 0, and chunk indices use the same 59.94 Hz timeline
counter. The TypeScript API retains the historical property name `fields`, but
PAL proves that these values are not universally physical display fields:

1. `GroupOfDataInfo` tick counts sum exactly to header `+0x08` in all 44
   measured retail files.
2. US video uses MPEG `frame_rate_code=4` (30000/1001 fps), so decoded pictures
   are approximately `ticks/2`.
3. PAL video uses MPEG `frame_rate_code=3` (25 fps) while header `+0x0c` remains
   5994, so decoded pictures are approximately `ticks * 25 / 59.94`.
4. The small terminal difference is at most nine pictures across the measured
   corpora and comes from the terminating chunks.

Group indices equal cumulative ticks. Video-chunk indices are nondecreasing,
start at zero, and remain inside the current group's tick interval.

## 5. Retail corpus coverage

Both retail corpora contain 22 movies. All 44 pass the strict chunk walk, audio
validation, demux, and MPEG metadata inspection.

### US retail

All 22 use the one-lane layout and MPEG 30000/1001 fps.

| files | count | stored size | scan |
|---|---:|---|---|
| `new_scene01`…`12`, `new_advertise` | 13 | 512×320 | progressive |
| `new_play01`…`06`, `new_rc4` | 7 | 512×448 | top-field-first |
| `new_million` | 1 | 512×352 | progressive |
| `new_dolby_pl2` | 1 | 512×384 | top-field-first |

### PAL retail

All 22 use MPEG 25 fps. Seventeen use the five-lane layout:
`new_scene01`…`11` and `new_play01`…`06`. The other five use the one-lane
layout: `new_scene12`, `new_advertise`, `new_dolby_pl2`, `new_million`, and
`new_rc4`.

| files | count | stored size | scan |
|---|---:|---|---|
| `new_scene01`…`12`, `new_advertise` | 13 | 512×320 | progressive |
| `new_play01`…`06` | 6 | 512×512 | top-field-first |
| `new_dolby_pl2` | 1 | 512×448 | progressive |
| `new_million` | 1 | 512×352 | progressive |
| `new_rc4` | 1 | 512×384 | progressive |

Region, filename, and disc serial are not parser inputs. The first-group
formula and MPEG sequence metadata select the supported layout and display
aspect.

## 5a. Display aspect — NTSC SAR 7:6, PAL SAR 4:3

Every measured stream sets MPEG-2 `aspect_ratio_information=1` (square
samples), but that flag does not describe how the PS2's non-square framebuffer
was shown on a 4:3 television.

Ground truth from the display setup in `SCUS_975.01`:

```
0x0015af20  addiu a1,zero,1 ; a0=0 ; a2=2 ; a3=0
0x0015af30  jal   0x00403c10        <-- sceGsResetGraph(0, INTERLACE, NTSC, FIELD)
0x0015af3c  addiu a3,zero,448       <-- display height
0x0015af44  addiu a2,zero,512       <-- display width
0x0015af50  addiu t7,zero,512
0x0015af58  movn  a3,t7,t6          <-- PAL flag: PAL -> h=512
0x0015af60  jal   0x00403f00        <-- set display environment
```

The game therefore presents a 512×448 NTSC framebuffer or a 512×512 PAL
framebuffer on a 4:3 display:

```
NTSC SAR = (4/3) / (512/448) = 7/6
PAL  SAR = (4/3) / (512/512) = 4/3
```

The MPEG sequence selects this without a filename or serial check:
`frame_rate_code=4` is the measured NTSC corpus and uses 7:6;
`frame_rate_code=3` is the measured PAL corpus and uses 4:3. Other AE3 frame
rates are unsupported because no display-aspect rule has been proven for them.

| region | stored | movies | SAR | resulting DAR |
|---|---|---|---|---|
| US | 512×448 | `new_play*`, `new_rc4` | 7:6 | **4:3** |
| US | 512×320 | `new_scene*`, `new_advertise` | 7:6 | 28:15 |
| US | 512×384 | `new_dolby_pl2` | 7:6 | 14:9 |
| US | 512×352 | `new_million` | 7:6 | 56:33 |
| PAL | 512×512 | `new_play*` | 4:3 | **4:3** |
| PAL | 512×448 | `new_dolby_pl2` | 4:3 | 32:21 |
| PAL | 512×384 | `new_rc4` | 4:3 | 16:9 |
| PAL | 512×352 | `new_million` | 4:3 | 64:33 |
| PAL | 512×320 | `new_scene*`, `new_advertise` | 4:3 | 32:15 |

Tag the SAR; never rescale the stored master. Use `setsar=7/6` for the measured
NTSC streams and `setsar=4/3` for the measured PAL streams.

## 5b. Convenience `.mp4`s — `ae3 fmv2mp4`

The extracted `.m2v` + `.wav` are the masters. `ae3 fmv2mp4` makes viewable
copies. **MPEG-2 in `.mp4` is spec-legal but QuickTime won't play it**, so
`-c:v copy` produces an unplayable file — hence the re-encode. Quality-first
settings:

- **x264 `-crf 15 -preset slow`**, yuv420p — visually lossless against a
  ~3.7 Mbps MPEG-2 source at this resolution.
- **Interlaced files get `yadif=mode=1` (bob)**, detected from the stream:
  59.94 fps output for NTSC or 50 fps for PAL. Every field becomes a frame;
  `mode=0` would discard half the temporal motion.
- **Region-derived SAR** (§5a): 7:6 for measured NTSC, 4:3 for measured PAL.
- AAC 256k. The source is 4-bit ADPCM (already lossy), so this is transparent in
  practice.

SAR can also be fixed on an existing `.mp4` with **no re-encode**, but the MP4
muxer rewrites `pasp` from stream metadata, so the bitstream filter alone is
silently ignored — **both** flags are needed:

```
ffmpeg -i in.mp4 -c copy -aspect 28:15 -bsf:v h264_metadata=sample_aspect_ratio=7/6 out.mp4
```

## 5c. Note on AI upscaling — do NOT descale

No tool in this SDK performs AI upscaling; this note records one property of the
source that matters if you build one.

The frames are **native-grid** MPEG-2. Descaling — inverting a *production*
upscale, as you would for a 1080p master that was actually drawn at ~480p and
upscaled during mastering — has **nothing to invert here**: 512×320 MPEG-2 *is*
the native grid. Descaling would destroy real detail, not recover it.

Verified with `getnative`, not assumed. The descale-error curve falls
**monotonically** toward native and never dips:

| frame | min error | at | interior dip? |
|---|---|---|---|
| `scene01` f2400 | 0.000018 | **309p** (top of range) | none — best interior 280p is worse |
| `scene01` f3000 | 0.000477 | **310p** | best interior (294p) is 3.0× worse |
| `play01` f600 | 0.012112 | **310p** | best interior (294p) is 1.1× worse |

A genuine native resolution shows a **sharp dip to ~0** with error **rising
again above it**. Here the minimum always sits at the top of the tested range
(309–310p of 320), i.e. "as close to native as we let it get" — the signature of
no upscale.

> ⚠ **`getnative` still prints a "best guess" (280p/240p/301p). Ignore it.** It
> always names local minima even when none are real; these are noise-level
> wiggles on a monotonic slope and they **disagree across kernels**. Read the
> error *curve*, never the guess. Descaling to a spurious 280p would throw away
> ~13 % of the real vertical detail.

If upscaling: deinterlace (if needed) → upscale on the **native grid** → a
single final resize to target with the aspect baked in. Correcting the aspect
*before* inference resamples first and lets the model amplify interpolation
blur — exactly one resample, at the end, output square-pixel. Because the pixels
are SAR 7:6 (§5a), the target width must come from the DAR
(`target_h * (src_w*7)/(src_h*6)`), **not** the square-pixel
`target_h * (src_w/src_h)`, which would bake the horizontal squish in
permanently.

## 6. Subtitles — `movie/*.bin` + `*.sbt`

Ten cutscenes have subtitle sidecars. Two files per cutscene, joined by index.
Tool: `ae3 sbt2srt` (decodes to `.srt`, muxable into the `.mkv`).

**Name mismatch:** the movies are `new_sceneNN.str` but the subtitles are
`sceneNN.bin` / `sceneNN.sbt` — no `new_` prefix. Only `scene01`–`07`, `09`,
`10`, `11` have pairs; **there is no `scene08` or `scene12` pair** on the disc
(and no subs for `play*` / `advertise` / `million` / `rc4`).

- **`.sbt`** = timing table.
  ```
  +0x00  "sbt\0"
  +0x04  u32 count                       (46 for scene01)
  +0x08  f32 first_start, f32 total      total == the movie runtime
  +0x10  count x { u32 index, u32 0, f32 start_sec, f32 end_sec }
  ```
  Exact: `0x10 + count*0x10 == filesize` (752 == 16 + 46*16). Units are
  **seconds** — identified by the header's second float (157.465 s) matching
  `new_scene01`'s runtime (157.41 s) to within a frame, and confirmed across all
  10 files: the **last cue's end time equals the declared total exactly, every
  time**.

- **`.bin`** = the text. A generic **typed-property container**, magic
  `0x72312487` (bytes `87 24 31 72`). Not the Exdb param format, and not used
  elsewhere in this spec set.
  ```
  +0x00  u32 magic, u32 count, then u32 section offsets at +0x08/+0x10/+0x18/+0x20:
         names, index, records, text
  names    count x 0x28 : char[16] name ("subtitle_0"...), pad, u32 1, u32 -> index off
  index    count x 0x08 : u32 3, u32 -> record offset
  records  count x 0x18 : typed fields; the LAST u32 is the offset into the text blob
  text     NUL-terminated UTF-8
  ```
  Every section lands where the arithmetic says (46*0x28 → 0x760 ≈ 0x770; 46*8 →
  0x8e0 ≈ 0x8f0; 46*0x18 → 0xd40 ≈ 0xd50), each padded to 16. `ae3 sbt2srt`
  walks the records directly — the ordering matches, so it is one lookup instead
  of chasing name→index→record.

### The text is UTF-8, not ASCII — and two traps

> ⚠ **Decode it as UTF-8, strictly.** Decoding as ASCII with `errors="replace"`
> silently mangles every multi-byte char into replacement glyphs: a single
> U+3000 renders on screen as **three "?" boxes**. Proof the codec really is
> UTF-8 rather than a custom codepage: the only non-ASCII bytes in the whole blob
> are exactly **27 × `0xe3` and 54 × `0x80`** — i.e. 27 well-formed `e3 80 80`
> (U+3000) sequences and nothing else — and all 46 strings then decode with
> **zero** errors. Use strict decoding; `"replace"` is what lets this fail
> quietly. (A 2005 US PS2 build storing subtitles as UTF-8 is worth noting;
> presumably the text pipeline was shared with the JP build.)

> ⚠ **Every subtitle is exactly TWO lines**, and the game bottom-aligns short
> ones by padding the *top* line with a lone U+3000 (ideographic space). In
> `scene01`: 27 of 46 are `[U+3000-only, text]`, the other 19 are `[text,
> text]`. That spacer is a device for the game's fixed 2-line renderer; SRT
> already bottom-aligns, so `ae3 sbt2srt` drops whitespace-only lines rather than
> emit a leading blank.

Verified: 0 replacement chars and 0 whitespace-only lines across all 10 `.srt`,
and ffmpeg parses them as `subrip`. The `.mkv`s then carry `mpeg2video` +
`flac` + `subrip`.

## 7. Audio: is any of this Dolby Pro Logic II? — partly open

The game ships a `new_dolby_pl2.str` logo bumper, and the ELF has a real
**three-way audio option**: the strings `stereo` / `monaural` / `dolby` sit
contiguous at `0x5f5aa0`–`0x5f5ab8` as the values of `sound_output_method`
(`0x5f64d0`). `dolby` is **not** just an asset name — at `0x002a3380` each of the
three is read by the project's usual `Exdb*` named-field loader
(`param_get(name, &out)` → `swc1` into its own float slot at struct +248 /
+252), so each output mode carries tunable parameters.

**What is established about the FMV audio:** it is genuine wide stereo. Mid/Side
energy is substantial (S/M ≈ −9 dB median, sustained across 100 % of windows)
with L/R correlation well below 1.0 (0.76–0.93). A collapsed or mono-ised decode
would show S/M ≈ −∞ and corr ≈ 1.0, so the extraction is not destroying whatever
is there.

> ⚠ **Do not claim the FMV audio is (or is not) DPL2-matrixed on the basis of a
> phase histogram.** A cross-spectrum phase test over `dolby_pl2` / `scene01` /
> `play01` puts 76–85 % of energy within ±30° and only 4–5 % near ±90°. That is
> **not** evidence against a matrix: a real DPL2 mix is mostly front-channel
> content and would look the same. The test cannot separate "modest surround
> component" from "no matrix" (it was read both ways during analysis). It proves
> wide stereo, nothing more.

**Compatibility is a non-question, by construction.** DPL2 is ordinary 2-channel
stereo — the matrix rides inside it. The channels are decoded losslessly with
nothing touched, so whatever is present survives bit-exact in the `.wav` /
`.mkv` (FLAC). **Use those for surround, not the `.mp4`** — its AAC is lossy and
perturbs phase, which is exactly what a matrix decoder steers on.

**Open, unverified:**
- **Where `sound_output_method=dolby` actually acts.** Plausibly the runtime 3D
  positional mix (SPU2 voice panning/polarity), not the pre-rendered FMV or BGM
  streams, which are already-mixed stereo. The config field has not been traced
  into the audio path.
- **Channel order.** `ae3 strextract` assumes interleave block 0 is **Left**. If
  it is Right, the mix still decodes but the stereo image — and any surround
  steering — is mirrored. Nothing in the container settles it, and the Mid/Side
  test cannot: it is symmetric under a swap. Would need the SPU2 voice setup in
  the ELF.

---

## Provenance

The format was derived from the self-describing tags and measured with direct,
bounded reads from the retail disc images and their VFI extents. No movie was
copied out of an image and no proprietary payload is stored in this SDK.

Measured coverage:

| corpus | movies | layouts | groups | video chunks |
|---|---:|---|---:|---:|
| US retail | 22 | 22 one-lane | 5,544 | 20,324 |
| PAL retail | 22 | 5 one-lane, 17 five-lane | 5,635 | 17,327 |

For all 44 files:

- header arithmetic and zero padding validate;
- the first group matches exactly one proven layout formula;
- exact chunk tags, indices, reserved words, payload bounds, and 16-byte
  padding validate;
- group tick totals and walked video-chunk totals match their declarations;
- every non-final gap contains less than one sector of leading zeros followed
  by exactly one or five complete audio blocks;
- every lane's ADPCM frame headers, interleave arithmetic, and per-lane byte
  total validate;
- final unconsumed data is zero and shorter than one sector;
- demuxed MPEG metadata reports code 4 / SAR 7:6 for all US files and code 3 /
  SAR 4:3 for all PAL files.

Asset inspection checks each candidate's 80-byte group/video-header extent
before issuing a positioned read. The first video payload is capped at
`0x10000` bytes (the measured maximum is 55,068), and the complete inspection
prefix is capped at `0x70000` bytes. Malformed header offsets therefore cannot
turn metadata inspection into a movie-sized read.

Before WAV allocation or header writes, the decoder validates integral
per-channel ADPCM frame/sample counts, block alignment, byte rate, RIFF `u32`
sizes, and the final allocation. WAV output is capped at 64 MiB; the largest
measured retail result is 30,851,116 bytes.

The historical US oracle was additionally decoded with ffmpeg at matching
frame counts and visually spot-checked. That external decode claim is not
silently extended to PAL; PAL coverage above records the SDK's structural
inspection and demux result.

### Verifying end-aligned audio

The one-lane block-phase test remains useful for detecting a start-aligned
demux:

```
block   = audio_block/2/16*28 / rate
preload = preload/2/16*28 / rate
phase(t) = ((t - preload) / block) mod 1
```

Start-aligned extraction creates silence bursts exactly on block boundaries;
end-aligned extraction does not. In the five-lane layout, first subtract all
five lane blocks from the next group offset, then select a lane. Header
arithmetic, bounds, tags, zero padding, lane count, audio frame headers,
declared totals, and full container consumption are hard runtime gates. A
structurally inconsistent file fails without returning a partial demux.

