# EXST — `.x` streamed-ADPCM format (`sound/stream/*.x`)

The streamed-audio family of Ape Escape 3: 1158 `.x` files (341 MB) under
`debug/us/sound/stream/` inside DATA.BIN — cutscene voice lines (`c_*`),
the 593 phone-call messages (`phone_*`), minigame speech and music
(`mgs_*`, `mgm_*`), jingles and credits music. Magic `EXST` = Sony **SG2**
library "ExSt" (external stream) module, source file `sg2exst.c`
(`sg2lib version2005_06_12`, ELF `.rodata 0x727460`). The payload is
standard PS-ADPCM streamed sector-by-sector to SPU2 voices — one voice per
channel.

> **Legal.** Extracted data is Sony's copyrighted content. This document
> describes the format only; it does not redistribute any asset.

Every claim below is either read from the shipped EE executable
(`SCUS_975.01`, addresses cited) or measured across the whole 1158-file US
corpus; the census and the parser agree everywhere. Facts that hold in the
corpus but are not enforced by the parser are marked *(corpus)*.

## 1. Header (0x78 bytes)

One header per file, followed immediately by the payload. The engine copies
the 0x78 bytes verbatim into its work context at `ctx+0x28`
(`FUN_003f6ef8`) and reads every field from that copy.

| off | type | field | provenance (EE) | corpus |
|---|---|---|---|---|
| `+0x00` | u32 | magic `'EXST'` (`0x54535845`) | validated in `Sg2ExStAdpcmOpenFake` = `FUN_003f6e48` (`piVar3[10]`; assert names the function, `sg2exst.c` line 0x11c) | all 1158 |
| `+0x04` | u32 | **channels << 16** | parser reads the count as s16 at `+0x06` (`FUN_003f6bd0`, `lh` @`0x3f6c10`); the dolby downmix compares the full u32 `== 0x20000` (`____SetVolCommon` `FUN_003f7020`); the in-memory header builders store `ch << 16` (`FUN_00318b20`, `FUN_0024bdc8`) | 1 ×1110, 2 ×48; low u16 always 0 |
| `+0x08` | u32 | **sample rate, Hz** | passed straight to the driver set-pitch call `FUN_00402780(voice_mask, rate)` (`FUN_003f76e0`); pitch-percent variant `rate*pct/100` (`FUN_003f7590`) | 24000 ×1110 (all mono), 48000 ×46, 44100 ×2 (`mgm_d12`, `mgm_t01`; both stereo) |
| `+0x0c` | u32 | **loop flag** — 0 = one-shot, else loop | feeder end-of-stream branch on `ctx[0xd]`: 0 → status 0x40 (stop); nonzero → seek to loop start, status 0x20 (`FUN_003f7ba8`) | 0 in all files |
| `+0x10` | u32 | **loop start**, 2048-byte sectors | `ctx+0x240 = hdr[+0x10] << 11` (`FUN_003f6ef8`, `sll ..,11` @`0x3f6f74`); loop wrap seeks here (`FUN_003f7850(ctx,2,ctx[0x90],0)` in `FUN_003f7ba8`) and the streamed-byte counter restarts from it (`____StatusWork` `FUN_003f78d0`, status 0x20 → `ctx[0x92]=ctx[0x90]`) | 0 in all files |
| `+0x14` | u32 | **stream length**, 2048-byte sectors | `ctx+0x244 = hdr[+0x14] << 11` (`FUN_003f6ef8`, `sll ..,11` @`0x3f6f80`); the feeder stops/loops when bytes streamed (`ctx[0x92]`) reach it (`FUN_003f7ba8`) | `0x78 + len*0x800 ==` file size in 1142/1158; 16 files overstate it (§4) |
| `+0x18` | u32[8] | **per-channel volume L** | `____SetVolCommon` (`FUN_003f7020`): `VL = hdr[+0x18][ch] * vol_pct[ch]/100`, clamped to 0x3FFF (SPU2 max); debug print "slot:%d vl:%d vr:%d" | `[0] = 0x407F` in every file (clamps to max); mono: only `[0]` set |
| `+0x38` | u32[8] | **per-channel volume R** | same site, second array (`ctx+0x60+ch*4`) | mono: `[0] = 0x407F`; stereo: `[0]=0`, `[1] = 0x407F` — ch0 hard left, ch1 hard right |
| `+0x58` | u32[8] | **per-channel reverb (effect-bus) flag** | read in `FUN_003f6bd0` (`piVar8 = ctx+0x80`): nonzero → voice bit OR'd into the per-core effect mask `DAT_0072c668` via `FUN_003ffa78`, zero → cleared via `FUN_003ffae8`; the 60 Hz voice flush ships the mask to the IOP as command 9 (`FUN_003ffc70` @`0x3ffd84`). Same mask the BGM note-on sets for tone flag 0x80 "reverb bus" (`0x3faf50`) | 0 in all files |

All remaining header bytes are zero in all files, and no reader exists for
them in the module.

Authoring note *(corpus)*: mono files put full volume on both sides of
channel 0; stereo files hard-pan channel 0 left and channel 1 right. The
authored value 0x407F exceeds the 0x3FFF clamp by ~0.8% — effectively
"full volume".

## 2. Payload — sector-interleaved PS-ADPCM

Payload = `length` (header `+0x14`) × 2048-byte sectors starting at file
offset 0x78. Standard PS-ADPCM 16-byte frames
(`[filter<<4|shift][flags][14 nibble-data bytes]` — same codec as the BGM
banks, see BGM.md):

- **Interleave: each 2048-byte sector is split contiguously between the
  channels — `2048/channels` bytes per channel per sector** (mono 2048,
  stereo 1024 = 64 frames). Provenance: the per-channel SPU buffer slice is
  `0x800 / channels` with channel *i* at `base + i*(0x800/ch)`
  (`FUN_003f6bd0`: `addiu 0x800` @`0x3f6bd4`, `div` by the channel count @`0x3f6c14`). Confirmed empirically: in
  stereo files the audio→padding transition appears at the same relative
  position in *both* halves of the final audio sector (e.g. `D11_can01.x`
  sector 201: both halves end with exactly 3 pad frames), which no other
  layout produces.
- **Frame validity** *(corpus)*: 21,347,072 frames across all 1158 files;
  zero illegal shifts (>12) and zero illegal filters (>4).
- **Flags** *(corpus)*: `0x00` on body frames; `0x02` on silent padding
  frames (data bytes all zero — every flag-2 frame in the corpus is
  silent) which pad each channel's final partial sector to the sector
  boundary. Two stray `0x01` (end) flags exist in the whole corpus
  (`phone_038.x` payload 0xDFF0, `phone_448.x` payload 0x13FF0, each the
  last frame of a mid-file sector) — authoring noise. The EE code never
  reads payload flags (voices are keyed on/off EE-side); what the SPU2
  hardware makes of those two stray end flags on console is unverified.

Decoding is the shared PS-ADPCM algorithm (coefficients
`{0,0},{60,0},{115,-52},{98,-55},{122,-60}`, shift, two-tap history —
bit-exact reference: the BGM decoder).

## 3. Runtime model (EE side, for context)

Statuses (`ctx+0x00`): 1 opened → 2 preloading (`FUN_003f7cc8`) → 8
`SG_EXST_STATUS_PRELOAD_END` → 0x10 playing (`Sg2ExStAdpcmPlay` =
`FUN_003f75e8`; keys on all channel voices, applies volumes and pitch) →
0x20 loop-wrap / 0x40 stopping (`FUN_003f7708` frees the voices) · 0x80 =
paused (pitch forced to 0, `FUN_003f76a0`). Bit 0x8000 = read-starved
(pitch 0 + muted until data arrives, `FUN_003f7a18`). Play before preload
completes sets `SG_EXST_MODE_PLAY_AUTO_START` (flag bit 4).

Each channel gets one SPU2 voice from the engine's 48-voice pool
(`DAT_00746ef0`; index/24 = core, index%24 = voice bit — `FUN_003f6bd0`).
Reads go through an installable callback (`DAT_0072c610`) into a command
queue of (state, addr, size) triples (`ctx+0xA8`, 32 entries); sector data
is forwarded to the IOP/SPU2 by the sound driver.

Filenames are built from code, not from a cue database: templates
`debug/%s/sound/stream/%s.x` (`.rodata 0x614e18`) and `stream/%s.x`
(`0x614e08`). No Exdb table references `.x` names (the `SeDesc` ×114
database covers the `sound/se` bank family instead). A
`debug/%s/sound/stream.bin` template also exists (`0x614dd0`) but no such
file ships on the US disc.

The game also *synthesizes* EXST headers in RAM to route other audio
through the same streamer (`FUN_00318b20`, `FUN_0024bdc8` — both store the
magic, `ch<<16`, rate, and sector count into a 0x78-byte block and call
the open path) — the reason header facts can be read off those writers as
well as the parser.

## 4. Corpus anomalies (US disc)

16 files' `length` field (header `+0x14`) claims more sectors than the
file actually contains (header value vs actual payload sectors):

| file | ch | rate | header | actual |
|---|---|---|---|---|
| `mgm_d12.x` | 2 | 44100 | 152 | 120 |
| `mgm_t01.x` | 2 | 44100 | 4031 | 4030 |
| `mgs_s01_02.x` | 1 | 24000 | 92 | 56 |
| `mgs_s01_05.x` | 1 | 24000 | 80 | 44 |
| `mgs_s01_06.x` | 1 | 24000 | 88 | 52 |
| `mgs_s01_11.x` | 1 | 24000 | 84 | 48 |
| `mgs_s01_16.x` | 1 | 24000 | 92 | 56 |
| `mgs_s01_21.x` | 1 | 24000 | 96 | 60 |
| `mgs_s01_30.x` | 1 | 24000 | 108 | 72 |
| `mgs_s01_35.x` | 1 | 24000 | 88 | 52 |
| `mgs_s01_45.x` | 1 | 24000 | 104 | 68 |
| `mgs_s02_03.x` | 1 | 24000 | 84 | 48 |
| `mgs_s03_05.x` | 1 | 24000 | 96 | 60 |
| `mgs_s03_13.x` | 1 | 24000 | 80 | 44 |
| `mgs_s07_14.x` | 1 | 24000 | 44 | 8 |
| `phone_395_02.x` | 1 | 24000 | 64 | 28 |

The mechanism is measurable *(corpus, 2026-07-19)*: **all 14 mono files
overstate by exactly 36 sectors** (5.376 s at 24 kHz; only the two stereo
music files differ — `mgm_d12` by 32, `mgm_t01` a plain off-by-one). The
voice corpus is authored with a standard trailing silent tail of ~42
sectors (median pad: `phone_` 42.0, `mgs_` 42.0, `c_` 41.5 — the ~6 s
tails §2 describes), and **the only voice files with under 10 sectors of
pad are exactly these 14** (4–7.5 each). So one authoring batch encoded
these payloads with ~36 fewer sectors of trailing silence than the header
field describes — the discrepancy sits entirely inside the silent tail,
which is why it shipped: on console the engine (whose stop condition is
the header value) hits EOF ~5.4 s early *during silence*, and nothing
audible changes. Consistent with a late revision batch: 12 of the 13 bad
`mgs_` files have `_a`/`_b` variant siblings with correct headers and
standard pads, and `phone_395_02` is itself a variant slot beside a
healthy `phone_395`. (Content language does not correlate: Japanese-language
takes exist elsewhere in the US phone set with correct headers — the batch
is mechanical, not a localization marker.) Tools should
treat the *actual* payload size (`(filesize - 0x78) & ~0x7FF`) as the
trustworthy bound and surface the discrepancy.

## 5. Tooling

`ae3 exst` (in `tools/ae3tools/exst.py`): header inspection (default),
census over a directory (`--census`), and WAV decode (`--decode`) using
a bit-exact PS-ADPCM decoder with per-sector deinterleave. Decode writes
every payload frame including the flag-2 silent padding tail (`--trim-pad`
drops the trailing pad run, shortened equally across channels).

Runtime: the C library `ae3_exst_*` (`core/exst.c`; API in `core/ae3synth.h`)
— header parse, per-sector decode with per-channel history in a state object,
trailing-pad scan — with `harness/exstdump` as its native CLI and the
`AE3Exst` class (`wasm/js/ae3synth.mjs`) as the wasm binding. The Python tool
stays the differ oracle: the shared golden vectors (`tests/`) pin native ==
wasm == the `ae3 exst --decode` WAV framing, and the private corpus gate
holds the C decoder sample-identical to the oracle across all 1158 files.
