# FMV — the `.str` movie container

The game's full-motion video lives in **22 `.str` movies** inside the DATA.BIN
(VFI) archive, under `debug/us/movie/*.str` (~861 MB total). Each `.str` is a
self-describing container holding one movie:

- **Video** is a raw **MPEG-2 elementary stream** — Sony's original bitstream,
  recovered **bit-exact** (never re-encoded), sliced across tagged chunks.
- **Audio** is **PS-ADPCM, 48 kHz stereo**, stored untagged in the gaps between
  chunks and decoded to PCM.

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

> The extracted video is Sony's copyrighted content. This document specifies the
> container format only; the SDK ships no game data. See `NOTICE.md` for the
> project's data policy.

**The container self-describes.** A hexdump of any movie shows literal ASCII
`Mpeg2Video` and `GroupOfDataInfo` tags inline in the data — naming the chunks
names the format. The one trap: **`.str` is not an MPEG program stream.** There
is no `0x000001BA` pack header and no `0x000001E0` PES header anywhere in any of
the 22 files — only bare `0x000001B3` sequence headers. ffmpeg/VLC therefore
cannot open a `.str` directly (which is what makes them look encrypted at first
glance; they are not). The video is a raw **elementary** stream — fed straight
to the PS2's IPU, which wants exactly that with no muxing — so concatenating the
chunk payloads in file order yields a valid `.m2v`.

```
DATA.BIN            VFI container
  └─ debug/us/movie/*.str      "str\0" container — 22 movies, ~861 MB
       ├─ Mpeg2Video chunks     raw MPEG-2 elementary stream (no PS/PES layer)
       └─ untagged gaps         PS-ADPCM audio, 48 kHz stereo
```

## 1. File header (`0x38` bytes, rest of the 0x800 sector zero-padded)

| off | value (`dolby_pl2`) | meaning |
|---|---|---|
| `+0x00` | `"str\0"` | magic |
| `+0x08` | 251 | **total FIELDS** — not frames. See §4 |
| `+0x0c` | 5994 | field rate ×100 → **59.94 Hz** → 29.97 fps |
| `+0x10` | 14 | `GroupOfDataInfo` count |
| `+0x20` | 48000 | audio sample rate |
| `+0x24` | 2 | audio channels |
| `+0x28` | `0x400` | per-channel interleave block |
| `+0x2c` | `0x4000` | audio bytes per group gap |
| `+0x30` | `0x10000` | audio preload before the first group |
| `+0x34` | `0x44000` | **total audio bytes** |

`+0x14`/`+0x18`/`+0x1c` vary per file and are **not identified** — nothing in
the pipeline needs them and no hypothesis survived cross-checking, so they are
left raw rather than guessed at.

## 2. Chunk format (uniform for every tag)

```
+0x00  char[16]  tag, NUL-padded   ("Mpeg2Video" | "GroupOfDataInfo")
+0x10  u32       index             video: field index of the chunk's first frame
+0x14  u32       payload size
+0x18  u32[2]    zero
+0x20  payload, then padding to a 16-byte boundary
```

`GroupOfDataInfo` payload is `u32[4]` = `(fields_in_group, video_chunks_in_group, ?, 0)`.
The third word is **unidentified** — it matches neither the group's video nor
its audio byte count, and nothing here needs it.

## 3. Audio — where it hides

The audio is **untagged**, so a naive chunk walk from `0x800` dies instantly on
it. Layout:

```
0x800                      audio preload, hdr.preload (0x10000) bytes
then repeating:            GroupOfDataInfo | its Mpeg2Video chunks | audio gap
```

Each gap is **zero padding FIRST, then `hdr.audio_blk` (`0x4000`) bytes of
ADPCM**, so the audio ends flush against the next chunk. The gaps vary in length
(`0x4000`…`0x47e0`); the audio inside them does not. The last group carries no
audio. Cross-checked four ways:

- `hdr.audio_total == preload + (groups-1) * audio_blk` — `0x44000 == 0x10000 + 13*0x4000`, exact.
- the **shortest gap in the file is exactly `0x4000`**, i.e. the case with zero padding.
- every gap ends on a `0x800` boundary.
- the leading region of every gap is **all zeros** — 13/13, 59/59, 148/148 gaps
  across `dolby_pl2`, `scene11`, `play01`. The bytes immediately before the next
  tag are live ADPCM.

> ⚠ **The audio is END-aligned in the gap. Do not read it from the gap start.**
> Reading from the start prepends the padding as silence and truncates the same
> number of real samples *once per group* (~every 0.3 s) — clearly audible as
> stutter/skipping. The proof is arithmetic: gap 1's padding is `0x120` = 288 B
> → 288/16*28 = **504 samples**, and a start-aligned decode produces a silence
> burst of exactly 504 samples at that spot. Reading end-aligned drops the
> per-group bursts to zero. A validity check on the ADPCM bytes will **not**
> catch this — zeros are a legal silent frame and pass every sanity test. Only
> the all-zero *leading* region reveals the layout.

Channels interleave in `0x400` blocks; **ADPCM predictor state is per-channel
and must persist across blocks** (decode each channel as one continuous stream,
not block by block). Codec is standard PS/PS2 SPU ADPCM: 16-byte frames → 28
samples, 5 filters, shift>12 → 9.

### The audio is ~0.89 s longer than the video, and that is correct

Every one of the 22 movies delivers a **constant ~48,600 bytes (0.886 s) more
audio than the video consumes** — identical for the 4 s bumper and the 160 s
cutscene alike. That is the streamer's steady-state buffer lead, exactly what a
preload-then-refill model predicts: delivered `audio_total`, consumed
`duration × 54,857 B/s`. It is not a sync bug and not a decode error.
`-shortest` at mux time drops the residue. **Audio starts at t=0 — do not
"correct" for the preload by shifting it.**

## 4. Fields, not frames — the one counter-intuitive bit

`+0x08` (251) reads like a frame count and **is not**. It counts *fields* at
59.94 Hz. Three independent checks agree:

1. the `GroupOfDataInfo` field counts sum to **exactly** `+0x08` (13×18 + 17 = 251);
2. the MPEG-2 sequence header says `frame_rate_code=4` (29.97) — half of 5994/100;
3. actual decoded frame counts land at ≈ fields/2 across all 22 files —
   `new_advertise` 6213 fields → **3106** frames (6213/2 = 3106.5), and every
   other file within a few frames (the deficit is the trailing terminator chunk,
   payload size 16).

## 5. The 22 movies

All 512 px wide, square *stored* samples, 29.97 fps, 48 kHz stereo. **22/22
decode with zero errors.**

| group | files | height | scan | notes |
|---|---|---|---|---|
| `new_scene01`…`12` | 12 | 320 | progressive | story cutscenes, 18 s – 2:40 |
| `new_play01`…`06` | 6 | 448 | interlaced `tt` | gameplay/demo footage |
| `new_advertise` | 1 | 320 | progressive | 1:44 attract reel |
| `new_million` | 1 | 352 | progressive | 1:08 |
| `new_rc4` | 1 | 448 | interlaced `tt` | 0:39 |
| `new_dolby_pl2` | 1 | 384 | interlaced `tt` | 4 s Dolby Pro Logic II bumper |

Longest: `new_scene03` (2:40, 9578 fields). Shortest: `new_dolby_pl2` (4.19 s).

- The `new_play*` / `new_rc4` / `new_dolby_pl2` streams are **interlaced**
  (`tt`), confirmed with `idet` (372 TFF / 0 progressive), not just trusting the
  flag. The `new_scene*` / `advertise` / `million` streams are genuinely
  progressive (401 progressive / 0 TFF). The `.m2v` and `.mkv` preserve both
  as-is. Deinterlace at *view* time — doing it at extract time would bake in a
  lossy transform.

## 5a. Display aspect — SAR 7:6, from the ELF

**The streams' own aspect flag is a lie.** Every `.str` sets MPEG-2
`aspect_ratio_information=1` (square samples), so a player shows the cutscenes at
1.6:1 and round objects come out visibly too narrow. Square samples are *not*
how the PS2 displayed them.

Ground truth, read from `SCUS_975.01` rather than guessed:

```
0x0015af20  addiu a1,zero,1 ; a0=0 ; a2=2 ; a3=0
0x0015af30  jal   0x00403c10        <-- sceGsResetGraph(0, INTERLACE, NTSC, FIELD)
0x0015af3c  addiu a3,zero,448       <-- display height
0x0015af44  addiu a2,zero,512       <-- display width
0x0015af50  addiu t7,zero,512
0x0015af58  movn  a3,t7,t6          <-- t6 = PAL flag @0x00635480: PAL -> h=512
0x0015af60  jal   0x00403f00        <-- set def disp env (a0=&env, a2=w, a3=h)
```

So the game runs a **512×448 NTSC** framebuffer (512×512 PAL) on a 4:3 TV:

> **SAR = (4/3) / (512/448) = 7:6** — pixels 16.7 % *wider* than tall.

The same NTSC/PAL 448/512 select recurs in the render path at `0x005332f0`
(`sltiu`+`movz` on the same flag), which then converts it to GS fixed-point
(`sll t5,t5,4`) and centres it against the 2048 GS origin — i.e. the framebuffer
height really is what the movie quad is laid out against.

**Three independent confirmations:**
1. `640 & 448` appear together **nowhere** in `.text`; `512 & 448` appear at 11
   sites — the framebuffer is 512-wide, so the pixels cannot be square on a 4:3
   screen.
2. The `new_play*`/`new_rc4` movies are authored at **exactly 512×448** — the
   framebuffer height on the nose. Apply SAR 7:6 and they land on **exactly DAR
   4:3**. A full-screen capture landing precisely on the TV aspect is not a
   coincidence.
3. That 1:1 mapping (play\* fills 448 with no vertical scale) means the 320-tall
   cutscenes are **letterboxed**, not stretched — so SAR 7:6 applies to them
   too, giving DAR 28:15 (1.867:1), a cinematic letterbox. A stretch-to-fill
   would instead need SAR **5:6** and make the picture *narrower* — the opposite
   of what the content shows.

| stored | movies | SAR | → DAR |
|---|---|---|---|
| 512×448 | `new_play*`, `new_rc4` | 7:6 | **4:3** (full screen) |
| 512×320 | `new_scene*`, `new_advertise` | 7:6 | 28:15 (1.867:1, letterboxed) |
| 512×384 | `new_dolby_pl2` | 7:6 | 14:9 |
| 512×352 | `new_million` | 7:6 | 56:33 |

**Tag it, never rescale it.** `setsar=7/6` (or an `-aspect` remux) costs nothing
and stays lossless; resampling to 597 px wide would throw away real detail for
no gain.

## 5b. Convenience `.mp4`s — `ae3 fmv2mp4`

The extracted `.m2v` + `.wav` are the masters. `ae3 fmv2mp4` makes viewable
copies. **MPEG-2 in `.mp4` is spec-legal but QuickTime won't play it**, so
`-c:v copy` produces an unplayable file — hence the re-encode. Quality-first
settings:

- **x264 `-crf 15 -preset slow`**, yuv420p — visually lossless against a
  ~3.7 Mbps MPEG-2 source at this resolution.
- **Interlaced files get `yadif=mode=1` (bob) → 59.94 fps**, detected per file
  from the stream. Every field becomes its own frame, so all the real motion
  survives; `mode=0` would halve the frame rate and throw half of it away.
- **`setsar=7/6`** (§5a) — tagged, not rescaled.
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

Format derived by structural analysis (the tags are self-describing) and
verified empirically, per the project rule that a claim needs a second source or
an empirical test:

- the chunk walk validates and consumes every tagged payload, audio gap, and
  final sector-padding byte (nonzero unparsed data is an error);
- per-group video-chunk counts from `GroupOfDataInfo` **sum to exactly** the
  number of `Mpeg2Video` chunks actually walked (81/81 in `dolby_pl2`) — so no
  chunk is silently dropped;
- ffmpeg decodes all 22 results with **0 errors**, at frame counts matching
  fields/2;
- frames spot-checked visually (real cutscene/gameplay imagery, no corruption);
- audio verified as signal, not noise: RMS −15.4 dB with a zero-crossing rate of
  0.00027. Mis-decoded ADPCM sits near 0 dB RMS with a ZCR ≈ 0.5.

### Verifying the audio — the block-phase test

The end-alignment issue (§3) is found and killed with `ffmpeg -af
silencedetect`, which reports the *timestamp* of every silent interval.
**Counting silences proves nothing** — cutscenes are full of natural dialogue
pauses, and `new_scene01` legitimately has 132 of them. The bug's signature is
*periodicity*, so test the phase instead:

```
block   = audio_blk/2/16*28 / rate   = 0.298667 s   (one group's audio)
preload = preload  /2/16*28 / rate   = 1.194667 s
phase(t) = ((t - preload) / block) mod 1     # ~0 => sits ON a block boundary
```

| | mid-content silences | on a block boundary |
|---|---|---|
| start-aligned (buggy) | 3 | block index **3.0000, 6.0000, 8.0000** — exact integers |
| end-aligned (correct) | 0 | — |

Correctly extracted, silences scatter uniformly (mean phase distance 0.23–0.29,
where 0.25 is random) — i.e. natural pauses. Anything clustering at phase ≈ 0
means the gap layout has been misread. Re-run this before trusting a change to
the audio path. `ae3 strextract` treats header arithmetic, bounds, padding,
declared group/field/chunk/audio totals, and full container consumption as hard
runtime validation. A truncated or structurally inconsistent file fails without
writing a partial result.
