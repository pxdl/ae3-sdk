# SE — sound-effect banks (`sound/se/*.hd` + `*.bd`)

The sound-effect family of Ape Escape 3: **101 `.hd`/`.bd` bank pairs** (20.1 MB)
under `debug/us/sound/se/` inside DATA.BIN — one bank per area or context
(`arabian`, `bay_a..e`, `boss_*`, `space_a..j`, `toyhouse_a..h`, the character
voice banks `p_N_voice_b/g`, `pcvoice*`, the Metal-Gear crossover `mgs*`, …).

An SE bank is the **same Sony "Jam" soundbank as a BGM bank** — `SShd` at
`0x0C`, the same 16-byte PS-ADPCM tone record, the same `.bd` codec — so this
document is written as **deltas against [`BGM.md`](BGM.md)**; read that first.
The one structural difference is where the *sequence* lives: a BGM song ships a
separate standard `.mid`, whereas an SE bank **carries its own little sequences
inside the `.hd`** (the seseq chunk) and its programs are always positional, so
a cue plays a fixed key rather than a melodic range.

> **Legal.** Extracted data is Sony's copyrighted content. This document
> describes the format only; it does not redistribute any asset.

Every claim below is either read from the shipped EE executable (`SCUS_975.01`,
addresses cited) or measured across the whole 101-bank corpus; the census and
the parser agree everywhere. Facts that hold in the corpus but are not enforced
by the driver are marked *(corpus)*.

---

## 1. Relationship to the BGM bank

Both formats are Sony's "Jam" bank (`SShd` at `0x0C`; [`BGM.md`](BGM.md) §2).
They share one loader: the sg2 library function whose asserts name it
**`vab_set`** (`FUN_00402938`, source `sg2vab.c`). What differs is which of the
header's six chunk slots each fills. The header slot table
([`BGM.md`](BGM.md) §3) has always carried six named `s32` offsets; BGM leaves
three of them −1 and this format is what fills them:

| slot | `+off` | BGM bank | SE bank |
|---|---|---|---|
| program | `+0x10` | `0x80` (melodic) | **−1** (100/101) |
| velocity | `+0x14` | varies | `0x80` |
| LFO | `+0x18` | −1 (only `s_20_park`) | present in **43/101** |
| **SE sequence** | `+0x1C` | −1 (all 62) | **present** (all 101) |
| **unknown** | `+0x20` | −1 (all 62) | **present** (all 101) |
| **SE program** | `+0x24` | −1 (all 62) | **present** (all 101) |

So an SE bank is the mirror image of a BGM bank: it empties BGM's melodic
**program** slot (`+0x10`) and populates the three **SE** slots BGM leaves
empty. Its instruments live in the **SE program chunk (`+0x24`)**, not the
melodic program chunk. (One bank, `mgs_saru`, is a **hybrid** — it carries
*both* a melodic program chunk at `+0x10=0x80` and an SE program chunk — because
that Metal-Gear cutscene bank is driven both ways.)

## 2. Header

Identical to BGM ([`BGM.md`](BGM.md) §3) except the slot fill above and the two
size prefixes:

```
0x00  u32  hd_size   == filesize - 0x180  (100/101; space_c: -0x300, see §7)   -- IGNORED by the driver
0x04  u32  bd_size   == the .bd file size (101/101)                            -- IGNORED by the driver
0x08  u32  0                                                                   (101/101)
0x0C  char[4] "SShd"                                                           -- the ONLY field validated
0x10  s32[6]  chunk offsets (table above); -1 = absent
0x28..0x80    -1 filler (101/101)
```

**Only `+0x0C` is validated.** `vab_set` (`FUN_00402938`) checks
`*(u32*)(bank+0x0C) == 0x64685353 'SShd'` and reads nothing at `+0x00` or
`+0x04`. So `hd_size` and `bd_size` are informational — an authoring-tool
artifact the driver never consults. `hd_size` is a genuine under-count: the real
chunk data runs all the way to EOF (proven by computing the last program's end
from its own header — it lands at filesize exactly, gap 0, on all 58 banks whose
last chunk is the SE program chunk), so `filesize − hd_size` is a fixed `0x180`
that corresponds to no boundary. Our parser still checks both prefixes as a
sanity gate; neither is load-bearing.

**Mode.** `vab_set` sets the VabDesc kind from `bank+0x7c`:
`(bank+0x7c == -1) ? 3 : 4`. `+0x7c` is inside the −1 filler on the whole corpus
(BGM and SE), so the kind is **always 3**; the `4` branch is unreached here.

## 3. The parser relocates the chunk table in place

`vab_set` (`FUN_00402938`, called by `FUN_00402e38` after it allocates a free
VabDesc slot) resolves each of the six offsets to an **absolute pointer written
back into the bank's own `−1` filler region**, at `bank+0x30..+0x44`:

```
for slot i in 0..5:   *(bank + 0x30 + 4*i) = (bank[+0x10+4*i] == -1) ? 0 : bank + bank[+0x10+4*i]
```

so `bank+0x30`=prog, `+0x34`=vel, `+0x38`=LFO, `+0x3c`=seseq, `+0x40`=unk,
`+0x44`=SE-prog. The VabDesc entry (`DAT_0074f840 + id*0x2c`, 32 slots) then
caches the bank base; the accessors index off these resolved pointers:

| accessor | reads | returns |
|---|---|---|
| `vab_get_prog` `FUN_00402cf8` | `bank+0x30` | melodic program `idx` (BGM; `0xFFFF`=empty) |
| **`vab_get_seprog`** `FUN_00402c68` | `bank+0x44` | SE program `idx` |
| **`vab_get_seseq`** `FUN_00402b18` | `bank+0x3c` | one sequence event stream (§6) |

Each is the standard Jam offset table ([`BGM.md`](BGM.md) §3): `s16 count`
(= **last index**, not the count) then `count+1` u16 byte-offsets, `base +
offset[idx]`.

## 4. SE program chunk (`+0x24`)

The bank's instruments. Same Jam chunk convention and same 8-byte program
record + inline 16-byte tones as BGM, but **every SE program is "drum-form"**:

- **program header byte 0 == `0xFF`** in all 156 SE programs across the corpus.
  So a program is always the drum-kit layout ([`BGM.md`](BGM.md) §3): byte 6 =
  `key0` (low key), byte 7 = `keyN` (high key), **tone count = `keyN − key0 + 1`**,
  one tone per key.
- The note-on selects a tone **positionally**: `tone = tones[note − key0]`
  (`FUN_003f8690`, `(note − key0) * 0x10`), guarded by `note >= key0`. There is
  **no key-range scan and no stack flag** — the melodic machinery BGM uses
  (`BGM.md` §8) does not apply.

Census (100 clean banks + `space_c` via its corrected slot): **156 programs,
5923 tones, and every tone sample address passes the end-flag test** — each is
frame-aligned in the `.bd` and immediately preceded by an end-flagged frame,
exactly the test that validates BGM ([`BGM.md`](BGM.md) §4). The `.bd` codec is
byte-for-byte the BGM decoder (gate below).

## 5. Tone record — deltas vs BGM

The 16-byte tone record ([`BGM.md`](BGM.md) §3) is **field-for-field identical**
where it matters to synthesis — root `+2`, fine `+3`, sample addr `+4..5`,
ADSR1/2 `+6`/`+8`, volume `+11`, pan `+12` (clamped 1..127), bend `+13`/prog[4],
LFO `+14`/prog[5], flags `+15` — all read the same way by the SE note-on. Three
deltas, all read from `FUN_003f8690`:

- **byte 0 = voice cut-group** (BGM: key-low, unused here because indexing is
  positional). If nonzero, the note-on first calls `FUN_003ff7f8(module, byte0)`,
  which scans the 48-voice pool for an **active** voice tagged with the same
  cut-group value and **force-stops it** (sets the release bit, issues key-off).
  This is the standard "a sound must not overlap itself" mechanism — looping and
  one-shot cues that should be monophonic share a nonzero group; `0` = no cut.
- **byte 1 = a voice-init parameter** (BGM: key-high, unused here). Passed to the
  voice setup `FUN_003fe360(2, voice, …, byte1)`; value `0x0A` dominates the
  corpus *(corpus)*. Its precise effect is a playback detail (next milestone).
- **flag bit 0 (`0x01`) is LIVE here.** In BGM this bit reaches `FUN_003fed10`
  but the BGM note-on never tests it ([`BGM.md`](BGM.md) §8 "Open: tone flags
  bit 0" — the consumer it could not find). The SE note-on **is** that consumer:
  `flags & 1 → FUN_003fed10(voice)` (`cmd[0] |= 0x04`). 34% of tones set it.
- **flag `0x20` hard-arms the LFO.** BGM treats `0x20` as inert (it needs a live
  CC1+CC2 pair, `BGM.md` §8); the SE note-on instead calls the rate setter with
  a fixed rate and the depth setter with `0x7f` whenever `0x20` is set, so an SE
  tone can carry built-in vibrato with no controller input. Flags `0x80` reverb,
  `0x40` use-prog-LFO, `0x10` use-prog-bend behave as in BGM.

## 6. SE sequence chunk (`+0x1C`) — the embedded cues

Where BGM ships a `.mid`, an SE bank stores its sequences inline as a
**two-level Jam chunk**, proven by `vab_get_seseq(vab, cue, layer)`
(`FUN_00402b18`):

```
seseq = bank + 0x3c                        # resolved base (ushort*)
cue_off   = seseq[1 + cue]                 # u16, byte offset from seseq base; even
inner     = seseq + cue_off                # this cue's sub-table
layer_off = *(u16*)(inner + 2 + 2*layer)   # u16, byte offset from seseq base; byte-granular
return      seseq + layer_off              # -> the layer's event stream
```

Both levels use the Jam `[count=lastindex][u16 offsets]` convention, and — the
one subtlety — **every offset value is relative to the seseq base**, not to its
own sub-table (the accessor returns `seseq + off`). Outer offsets are even (they
are `>>1`'d into the u16 array); inner (layer) offsets are byte-granular pointers
into a **shared event-stream pool** that follows the tables.

So a **cue** (one sound-effect id) owns one or more parallel **layers**, each an
event stream. Census: **564 live cues, 2699 layers** across the corpus; every
layer stream ends in the bytes **`FF 2F 00`** — the same end-of-track marker as
Standard MIDI (2699/2699). The stream body is an SMF-like event list (observed
status bytes `0xA0..` note, `0xB0..` control); its exact grammar and timing are
**playback semantics — the next milestone**, deliberately not modelled here.

The trigger side is game code: `kick3D(bank, req, vol, …)` (`FUN_0035b258`,
debug string `kick3D() [%20s] bank:%2d req:%2d vol:%02d`) opens a cue via
`FUN_003fc3c8`, which resolves `(cue, layer)` through `vab_get_seseq`, installs
the event-stream pointer into a module, and lets the 60 Hz walker step it into
`FUN_003f8690`. A cue's bank/priority/reverb/3D-distance defaults come from the
separate **`SeDesc` table in the exdb design data** (parsed by `ae3 exdb`), not
from the bank — that table is the cue database; measuring how it routes into
bank/req is part of the playback milestone.

## 7. The other chunks, and the one corrupt bank

- **Velocity chunk (`+0x14`, at `0x80`).** `s16 count` (= 0) then a **128-byte
  identity ramp** `0,1,…,127` (101/101) — velocity passes through unchanged, so
  SE dynamics are not remapped. (The count/curve pair matches BGM
  [`BGM.md`](BGM.md) §3; BGM's is likewise identity.)
- **LFO chunk (`+0x18`).** Present in **43/101** banks. Same Jam convention and
  same 120-byte entries as `s_20_park` ([`BGM.md`](BGM.md) §8): 60-byte pitch
  waveform + 60-byte amplitude waveform, and the **final entry is EOF-truncated
  to 64 bytes** (60 pitch + 4) on all 43 — identical to the BGM case, and for the
  same reason (the amplitude half is never armed).
- **Unknown chunk (`+0x20`).** A fixed **784-byte block, byte-identical across
  all 101 banks** (16-byte header `64 00 …` + 48× 16-byte records with an
  incrementing index and constant `{vol 0x64, pan 0x40, 0x04, 0x64}` fields —
  a default 48-entry parameter table stamped by the authoring tool). `vab_set`
  resolves it to `bank+0x40` but **no traced code reads it**; being constant, it
  carries no per-bank information.
- **`space_c` is shipped corrupt.** Its unknown chunk is bloated by `0x180`
  (24 duplicated records), which shifts the SE program chunk to `0x8d2` while the
  header slot still reads the stale `0x752` and the `.hd` tail is `0x300` instead
  of `0x180`. The driver, reading the stale slot, would misparse it too — the
  file is effectively broken (an unused `space_*` variant). The tool refuses it
  by the tail signature rather than silently correcting it; the gate isolates it
  as the one known-corrupt bank and still codec-checks its `.bd` via the
  corrected slot.

## 8. Runtime model (EE side, for context)

The SE module shares the 48-voice SPU2 pool with BGM (round-robin, never steals;
`sg2slotctrl.c`, `BGM.md` §M5). The note-on is **`FUN_003f8690`** — the "4-byte
event" API that [`BGM.md`](BGM.md) §8 identified as *not* the BGM path; it is the
SE path. It reads the same tone fields as BGM's note-on plus the three deltas in
§5, allocates a voice, and sets it up through the same `FUN_003fe*` voice
setters and the same IOP command executor (`sg2iopm1.irx`). **Cue sequencing,
`SeDesc` consumption, and the event-stream grammar are the next milestone** —
they are new driver surface and are being measured, not guessed, before any
runtime library or player surface is built.

## 9. Tooling

`ae3 se` (`ae3tools/se.py`):

```
ae3 se BANK.hd                 # header, chunk table, programs, cue/layer counts
ae3 se --census DIR            # survey every .hd under DIR
ae3 se BANK.hd --cues          # dump the cue -> layer -> event-stream structure
ae3 se BANK.hd --decode -o DIR # decode every .bd waveform to WAV (44100 Hz mono)
```

Gate `checks/check_se.py` (private): over all 101 banks, every referenced `.bd`
waveform is decoded by both `se.decode_adpcm` and the `bgm.py` oracle and
required to be **sample-identical incl. loop point** (2180 waveforms), plus the
structural invariants of §§2–7 (drum-form programs, the end-flag address test,
the two-level seseq with `FF 2F 00` termination, the constant unk chunk,
`space_c` isolated). PASS on the whole corpus.

## 10. Provenance

Format facts are read from `SCUS_975.01`: the parser `vab_set` `FUN_00402938`
and the accessors `FUN_00402b18` (`vab_get_seseq`), `FUN_00402c68`
(`vab_get_seprog`), `FUN_00402cf8` (`vab_get_prog`) in `sg2vab.c`; the SE note-on
`FUN_003f8690` and the cut-group scan `FUN_003ff7f8`; the cue-open `FUN_003fc3c8`
and `kick3D` `FUN_0035b258`. Source-file and function names are the sg2 library's
own `.rodata` assert strings (`sg2vab.c` `0x727a60`, `vab_get_seseq` `0x727a70`,
`vab_loadbybin_hd`/`_bd` `0x7140e8`/`0x714118`). Everything else is a census over
the 101-bank US corpus. Function-level notes:
`decomp/functions_bgm/se/NOTES.md` (private repo).
