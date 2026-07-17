# DATA.BIN — VFI container format

`DATA.BIN` (from the game disc) is a **VFI** container: a hierarchical archive of
3994 files across 254 directories, 2.14 GB in total. The container is fully recovered —
every file enumerates with its real directory path, all bytes are accounted for, the `.sz`
compression codec is identified, the nested `.pck` sub-container is documented, and models
and textures are located.

> **Legal.** Extracted data is Sony's copyrighted content. This document describes the
> container format only; it does not redistribute any asset. Do not redistribute extracted
> assets.

## Pipeline overview

```
DATA.BIN                VFI container — 3994 files, 254 dirs, real directory tree
  └─ *.sz               [u32 decompressed_size][raw deflate][Adler-32 BE]   (§2)
       └─ *.pck         PCK container
            ├─ I3D_BIN    3D model   (see docs/formats/I3D.md)
            └─ TIM2       texture    (standard PS2 format)
```

---

## 1. Header

| off | type | field | value in DATA.BIN |
|---|---|---|---|
| `+0x00` | u32 | magic `'VFI\0'` = `0x00494656` | ✓ |
| `+0x04` | u32 | version | 1 |
| `+0x08` | u32 | **DATA_OFF** (in sectors) → data starts at `0x1f800` | 63 |
| `+0x0c` | u32 | zero | 0 |
| `+0x10` | u16 | **FILES** | 3994 |
| `+0x12` | u16 | **FOLDERS** | 266 |
| `+0x14` | u32 | **INFO_OFF** → file entry table (self-relative) | `0x3e88` |
| `+0x18` | u32 | dummy | `0x1e340` |
| `+0x1c` | u32 | INFO_END | `0x1f47c` |

`DATA_OFF` is a sector count: byte offset = value × `0x800`, so 63 × `0x800` = `0x1f800`,
where the first embedded payload begins.

`INFO_OFF` is **self-relative**: the reader loads the word at `+0x14` and adds it to the base
address (`FUN_00100958`: `lw t7,0x14(a0); addu a0,a0,t7`). In DATA.BIN that word is `0x3e88`.
Note that `+0x10` is the *middle* of the header, not the start of the entry table — reading a
flat offset table from `+0x10` yields non-monotonic garbage.

### File entry

Variable length, walked sequentially starting from `INFO_OFF`:

| off | type | field |
|---|---|---|
| `+0x00` | u16 | **ENTRY_SIZE** (advance by this) |
| `+0x02` | u16 | **PARENT_OFF** (0 = root) |
| `+0x04` | u32 | **OFFSET in SECTORS** → byte offset = value × `0x800` |
| `+0x08` | u32 | **SIZE** in bytes |
| `+0x0c` | str | **NAME** (`ENTRY_SIZE − 12` bytes) |

### Folder record

Located at `FOLDERS_OFF + PARENT_OFF`, where `FOLDERS_OFF` (`0x1e340`, the header `+0x18`
"dummy" value) is the end of the file table:

```
[u16 ENTRY_SIZE][u16 NEXT_OFF][u16 PARENT_OFF][u16 dummy][str PATH]
```

To reconstruct a file's full path, walk `PARENT_OFF` up the folder chain until it reaches 0,
prepending each `PATH`.

### Name-hash index

A sorted `u16` name-hash index sits at `+0x20`: pairs of `[u16 hash][u16 entry_index]`, with
`hash_count` = FILES. The reader (`FUN_0010070c`) **binary-searches** this array for
lookup-by-name, breaking hash collisions with a `strcmp` against the entry's name at
`entry+0xc`. The hashes are strictly ascending (a binary search requires it):
`7, 11, 32, 43, 45, 69, 77, 90…`.

Extraction does not need the index (walk the entry table directly), but it is a useful
self-check: `0x20 + 3994 × 4` = `0x3e88` = `INFO_OFF` exactly — the hash array ends precisely
where the entry table begins.

### Verification (census)

- entries walked: **3994 / 3994** = FILES ✓
- `sector × 0x800 + size ≤ EOF` for **3994 / 3994** ✓
- printable names: **3994 / 3994** ✓
- all 12 `.irx` files identify as *"ELF 32-bit LSB PlayStation 2 IOP module, MIPS"* with real
  Sony names — `padman`, `mcman`, `mcserv`, `libsd`, `sio2man`, `sifcmd`, `dbcman`, and
  `sg2iopm1.irx` (the SG2 engine's own IOP module) ✓
- a MIDI parser walks **68 / 68** `.mid` through every `MTrk` chunk to a clean EOF, 0 bad ✓
- the 68 `.mid` + 163 `SShd` `.hd` + 163 `.bd` census matches the documented layout for this
  title (sequences are MIDI; headers/banks remain SShd) ✓

---

## 2. `.sz` — DEFLATE with an Adler-32 trailer

```
[u32 decompressed_size][raw deflate stream][u32 Adler-32 of the payload, BIG-endian]
```

This is exactly a **zlib stream with its 2-byte header stripped**: deflate body followed by
the standard big-endian Adler-32 checksum of the uncompressed data. Verified corpus-wide
(2026-07-17): **1706 / 1706** `.sz` files in the container inflate to their exact declared
size AND carry the exact Adler-32 trailer, 0 exceptions.

Two equivalent ways to read one:

- `zlib.decompress(blob[4:], -15)` — raw-mode inflate; zlib stops at the end of the deflate
  stream and silently ignores the 4 trailing checksum bytes (how the trailer went unnoticed
  until the web port).
- Prepend a valid zlib header (e.g. `78 9C`) to `blob[4:]` and inflate as a zlib stream —
  required for the WHATWG `DecompressionStream`, whose `"deflate-raw"` mode *rejects*
  trailing bytes as junk; `"deflate"` mode consumes the trailer and verifies the checksum.

---

## 3. `.pck` — PCK container

```
'PCK\0' | u32 INFO_OFF | u32 FILES
```

Member entries begin at `INFO_OFF`, 16 bytes each:

```
[u32 name_off][u32 ATTR_OFF][u32 offset][u32 size]
```

> **Caveat: `ATTR_OFF` is an offset, not a flags bitfield.** Across all 36508 members it takes
> 1029 distinct values for `model` alone, and no bit is constant within any one kind — it does
> not behave as a bitfield. It is a **second offset into the same string pool as `name_off`**,
> pointing at a whitespace-separated **attribute string** whose first word is the declared type.

Examples of attribute strings:

```
tm2                                          a texture
i3r static bg prio=1000 start=0 end=10000    a model, with draw attributes
cl2 pictname=meka_zenmai_o                    an extra palette for that texture
i3c geom=ape_nrm_head_b parent=jnt_armL1      collision, bound to a bone
```

Evidence that it is an offset: in `ci_saveload/ui.pck` every texture carries `ATTR_OFF` = `0x10`
and `blob[0x10:]` is literally `"tm2"`; the rest carry `0x3f` and `blob[0x3f:]` is `"uis"`. The
value is constant across a run and changes exactly where the member kind changes.

### Type schema (all 36508 members)

| type | n | | type | n | | type | n |
|---|---|---|---|---|---|---|---|
| `tm2` | 11931 | | `cl2` | 1863 | | `nav` | 290 |
| `i3m` | 8404 | | `chb` | 1337 | | `i3c_s` | 232 |
| `i3r` | 7345 | | `cts` | 1201 | | `ltb` | 170 |
| `i3c` | 2022 | | `exdb` | 574 | | `fog` | 132 |
| | | | `uis` | 508 | | `rsv`/`occ`/`ops`/`ipc`/`sys`/`ico` | 101 |
| | | | `anx` | 398 | | | |

Attribute tokens: `prio` (4890), `start`/`end` (4890), `geom`/`parent` (1965), `pictname`
(1863), `auto_animation` (243), `fog` (140), and `anim_tex_{node,mat,layer,mod_u,mod_v}_{0,1,2}`.
Bare words: `static` (2171), `bg` (190), `lighting` (26), `semitrans` (1).

Two consequences of the attribute string:

1. **6574 members have no recognisable magic** of their own; the declared type is the only
   thing that names them.
2. **`parent=` names a bone.** `col_ape_mg1_b` appears 8× with `parent=jnt_armL1`,
   `jnt_armR1`, `jnt_spin1`, `jnt_footL1`, … — a per-bone hitbox rig, recorded only in the
   attribute string. Member names are **not unique** (579 pairs collide on name *and* type with
   different bytes), so an emitted sidecar table (one row per member) is the authority for a
   member's identity, not its filename.

### The `static` attribute and world placement

The bare word `static` marks a model whose world placement is baked into its own vertices — the
mesh is correct at the identity transform. Non-`static` models are positioned by an external
transform stored in the level scripts / `.plc` / `.pck` (not yet reversed). Worked example, the
50 models in `travel_a/bg`:

- **29 `static`** → placement baked into vertices (21 sit far from the origin; 8 are large level
  meshes centred near it, 157–6087 units across).
- **21 not `static`** → all 21 have their centre exactly at the origin; their transform lives
  elsewhere and cannot be recovered from the model alone.

### Extractor caveats

- **11 member names begin with `/`** (`asia_a`'s `/f_asi_ita04`, all 8 of `space_e`'s `/z_*`,
  two in `onsen_e`). A naive path join treats these as absolute and escapes the output
  directory; strip the leading `/` before joining.
- Some types are written with an extension that reflects the magic rather than the token: `i3r`
  members carry `I3D_BIN` (write `.i3d`) and `i3c_s` members carry `I3D_I3C` (write `.i3c`),
  because no `i3d` type token exists in the archive. The sidecar table keeps the raw token.

---

## 4. Directory tree (3994 files, 254 dirs)

```
irx/3.0/                    12 IOP modules (padman, libsd, sg2iopm1, ...)
debug/us/stage/<name>/      the levels — 168 of them
    bg.pck.sz               level geometry + textures   (162 members in toyhouse_c)
    character.pck.sz        characters + textures       ( 99 members)
    navigation.pck.sz       nav / collision             (  2)
    exdb.pck.sz             enemy params
    apelodtex.pck.sz        LOD textures
    area.eff.sz / area.eft.sz   effects
    area.luc / placeholder.plc / actionseq.asq / monkeydb.xml.sz / text.bin
debug/us/sound/stream/      1158 .x  = EXST streamed audio ("EXtended STream")
debug/us/sound/bgm/         192 = 70 stems: .hd + .bd + .mid   (see docs/formats/BGM.md)
debug/us/sound/se/          202  (sound effects; likely the same bank format)
debug/us/movie/             22 .str FMVs (861 MB) + sceneNN.bin/.sbt subtitle pairs
                                                          (see docs/formats/FMV.md)
```

Example stage names: `toyhouse_a..c`, `travel_a`, `arabian_a..e`, `asia_a..e`, `bay_a/c`,
`castle_a`, …

Notes on paths that do **not** go through the VFI/pck route above:

- **BGM is sequenced.** Each song is a soundbank (`.hd` header + `.bd` raw PS-ADPCM body) plus a
  standard MIDI file that any DAW opens. The bank is Sony's "Jam" format (`SShd` at `0x0C`).
  See `docs/formats/BGM.md`.
- **`.x` (`EXST`) is streamed audio, not a model format.** All 1158 live under
  `debug/us/sound/stream`. Models are inside `*.pck`.
- **`debug/us/movie/*.str` is its own container**, unrelated to VFI/pck — a `"str\0"` chunk
  format holding a raw MPEG-2 *elementary* stream plus untagged PS-ADPCM. It has no pack/PES
  layer, so ffmpeg cannot open a `.str` directly. See `docs/formats/FMV.md`.

---

## 5. Models & textures

- **TIM2** — the standard, fully documented PS2 texture format. 63 / 78 textures in `toyhouse_c`
  parse with sane dimensions (128×128, 128×64, 8-bit indexed + 256-colour CLUT,
  `total_size + 16 == filesize`). The 15 remaining are all `ape_lod_*` at 2432 B with zeroed
  dimensions — a different sub-format or an offset bug, still open.
- **I3D_BIN** — magic `'I3D_'` + `'BIN\0'`. A proprietary format also used by *Rule of Rose*
  (shared middleware). Prior art:
  - Noesis plugin `fmt_i3dg.py` — <https://github.com/Durik256/Noesis-Plugins> — meshes
  - `fmt_RuleOfRose_PS2__i3d` — bones
  - Reported support: mesh + bones **yes**; skinning **no**; `I3D_I3M` animation **no**
  - Discussion: [ZenHAX](https://zenhax.com/viewtopic.php@t=13790.html) ·
    [ResHax](https://reshax.com/topic/406-ape-escape-3rule-of-rose-i3d-file/)
- **`I3D_I3M`** — the animation format, unsolved everywhere.

See `docs/formats/I3D.md`.

---

## 6. Tools

The `ae3tools` package provides the CLI:

| command | does |
|---|---|
| `ae3 vfiparse` | parse header + entries + folder paths + self-checks |
| `ae3 extract` | extract members; `--manifest` / `--glob` / `--all` / `--raw`; auto deflate + pck |

```bash
ae3 vfiparse                                              # header + proofs + tree
ae3 extract --manifest                                   # full 3994-file listing
ae3 extract --glob 'debug/us/stage/toyhouse_c/*' --out extracted/databin
```

One stage (`toyhouse_c`) → 14 files → 5 PCKs → 125 `.i3d` + 78 `.tm2`, 7.6 MB.

---

## 7. Open questions

- **`I3D_BIN`** meshes: use/port the Noesis plugin. Skinning and `I3D_I3M` animation: unsolved.
- The 15 odd `ape_lod_*.tm2` (2432 B, zeroed dimensions) — different sub-format or an offset bug.
- `.plc` / `.luc` / `.asq` / `.rdi` / `navigation.pck` semantics — the level-script transforms
  (which position non-`static` models), nav mesh, and collision.
- Header `+0x18` "dummy" (used as `FOLDERS_OFF`), entry `+0x00` upper bits, and the use of
  `INFO_END`.

---

## Provenance

The VFI layout was reverse-engineered from the game and verified two independent ways.

1. **The game's own MIPS reader.** The magic `'VFI\0'` = `0x00494656` is too large for one MIPS
   immediate, so the compiler must emit `lui 0x0049` + `ori 0x4656`. Scanning `.text` for that
   pair found exactly one site in the 6.5 MB binary:

   ```
   0x001008e0  3c0f0049  lui  t7,0x0049
   0x001008e4  8c8e0000  lw   t6,0(a0)
   0x001008e8  35ef4656  ori  t7,t7,0x4656
   0x001008ec  15cf0009  bne  t6,t7,0x00100914
   ```

   That one site named the whole reader API, which was decompiled to derive the header, entry,
   folder, and hash-index layouts (`FUN_00100958` resolves `INFO_OFF`; `FUN_0010070c`
   binary-searches the hash index).

2. **aluigi's `ape_escape_vfi.bms`** QuickBMS script, an independent reimplementation. It agrees
   on the structure and confirmed three field semantics that were otherwise ambiguous from the
   ELF alone: `DATA_OFF` is a sector count (63 × `0x800` = `0x1f800`, where the first embedded
   payload sits), `FOLDERS` = 266 (resolving into the real 254-directory tree), and the entry's
   `ENTRY_SIZE` / `PARENT_OFF` pair (a sequential walk lands on exactly 3994 / 3994 entries).

Semantics derived this way were then confirmed empirically rather than trusted: `.sz` = raw
DEFLATE was verified by decompressing 400 files to their exact declared sizes, and `ATTR_OFF`
was confirmed to be a string-pool offset (not a flags bitfield) by reading the attribute strings
it points at. The BMS does not use the name-hash index at `+0x20`; that was recovered solely
from the ELF reader.
