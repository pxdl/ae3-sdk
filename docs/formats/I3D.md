# I3D_ — the SG2 model / animation / collision format family

`I3D_` is the container SG2 uses for models, skeletal animation, and collision. It is
**three distinct sub-formats** sharing a four-byte `I3D_` prefix but **not** a common
header:

- **`I3D_BIN`** — the model container: a node tree carrying geometry, materials, skin,
  skeleton, and bone names.
- **`I3D_I3M`** — skeletal animation: named tracks of index-compressed keys.
- **`I3D_I3C`** — collision: an indexed triangle mesh plus a quantised BVH, one AABB and
  one collision material per file.

Coverage across the shipped game corpus: **154/154** `I3D_BIN`, **106/106** `I3D_I3M`,
**50/50** `I3D_I3C`, and **256/256** `.tm2` textures parse and round-trip. `ae3 gltf`
exports all 154 models to `.glb` with skeleton, skin, and textures; `ae3 i3c` decodes all
50 collision files including the quantised BVH bounds; `ae3 i3manim` evaluates the
animation tracks and reproduces a coherent, correctly looping run cycle.

> **Legal.** This document describes Sony's data for interoperability and clean-room study.
> It does not redistribute any asset. Do not redistribute extracted assets, and keep any
> extracted output out of version control — it is not covered by the SDK licence. See
> `NOTICE.md`.

The assets are **Maya-exported** (`polySurfaceShape`, `lambert` shader names survive in the
string tables). The mesh payload layout is shared with other games on this engine family
(e.g. *Rule of Rose*); see [Geometry provenance](#geometry-provenance).

---

## Container tags

None of the three tags is ever materialised as an immediate in code; each lives in
`.rodata` as a string and is compared with `memcmp`:

| tag | `.rodata` | compared by |
|---|---|---|
| `I3D_BIN\0` | `0x00720138` | `FUN_003a7390` |
| `I3D_I3M\0` | `0x007201d0` | `FUN_003a7620` |
| `I3D_I3C\0` | `0x007201d8` | `FUN_003a7b88` |

Adjacent `.rodata` carries a build stamp (`2005/12/16,17:48:46`) and, at `0x00720140`, an
untyped 36-entry `u32` table whose purpose is not established.

**The three tags do NOT share a header.** Each sub-format has its own, and `+0x0c` means
different things in each (a fixup flag in BIN, a file size in I3M and I3C). Sniff the full
eight bytes to tell members apart. The extractor sub-tag / PCK `flags` field
(`0x12a` = I3D, `0x2ec` = TIM2) distinguishes I3D from TIM2 but does **not** distinguish
the three I3D sub-formats — read the 8-byte tag to emit `.i3d` / `.i3m` / `.i3c`.

---

## 1. `I3D_BIN` — the node tree

`FUN_003a73e8` is the loader: it checks the tag, requires `*(u32*)(f+8) == 0x00100001`,
then treats `f+0x0c` as a **fixup state** (0 = offsets, 1 = pointers) and calls
`FUN_003a85e0(f+0x10, f+0x10)`. **Both args are `f+0x10`, so every offset in the file is
relative to `f+0x10`, and `f+0x10` is the root node.** A flat read from the file base
fails for this reason.

```
+0x00  char[8]  'I3D_BIN\0'
+0x08  u32      version == 0x00100001
+0x0c  u32      fixup state (0 on disk)
+0x10  node     ROOT  (type must be 0x52 -- FUN_003a7390 checks byte f+0x17 & 0x7f)
```

**Node — 16 bytes**, from `FUN_003a85e0` (fixup) / `FUN_003a8490` (unfixup):

| off | field | notes |
|---|---|---|
| `+0x00` | `DATA` | offset → payload (0 = none) |
| `+0x04` | `packed` | bits 0-23 `CHILD_COUNT`; bits 24-30 `TYPE` (`>>0x18 & 0x7f`); bit 31 = **skip the type handler** |
| `+0x08` | `CHILDREN` | offset → child array, **stride 0x10** |
| `+0x0c` | `EXTRA` | offset → **name** in the tail string table |

Children recurse: `FUN_003a85e0(iVar4, param_2); iVar4 += 0x10`.

**`EXTRA` is a name pointer.** On `ape_slm_body_c.i3d` the tail string table from `0x1b99c`
reads `\0CombinedMesh0000\0CombinedMeshInstance0000\0sortOff__body1\0...` and sibling
`EXTRA` values `0x1b9c7` / `0x1b9d6` land exactly on `sortOff__body1` (+43) and
`sortOff__body2` (+58).

### Types

`FUN_003a8408` **rejects** 13 obsolete types: `04, 16, 24, 2c, 2e, 2f, 36, 3f, 40, 41, 48,
4e, 51`. Everything else in `0x00..0x59` is valid (the handler tables are 90 entries). Two
parallel 90-entry tables of function pointers sit back to back:

```
0x0069a1a8  unfixup handlers    0x0069a310  fixup handlers
```

There are 76 distinct handlers; the 15 sharing the no-op `0x003a8488` are the blacklist
plus types 0 and 1 (valid, but carrying no internal pointers).

**Types named by their own node names** (the string table is self-documenting):

| type | meaning | evidence |
|---|---|---|
| `0x52` | file root | 154 nodes / 154 files; enforced by the loader |
| `0x25` | **Material** | `Material0000`, `lambert31` (a Maya shader) |
| `0x2d` | **Mesh / CombinedMesh** | `CombinedMesh0000` |
| `0x59` | **Mesh instance** | `CombinedMeshInstance0000`, `polySurface959\|polySurfaceShape977` |
| `0x2a` | **Bone / joint** | see §5; paired with `0x2b`, 2151 each corpus-wide |
| `0x53`,`0x54` | file-level singletons | exactly 154 each = 1 per file |
| `0x33` | 23915 nodes (the bulk) | not established |
| `0x47` | 10812 nodes (index lists) | see §4 |

A fuller per-type census is in §4.

### Payload pointers are payload-self-relative

`FUN_003a85e0` calls each handler with `a0` = the **relocated payload pointer**. The type
`0x2d` handler (`0x003a99b8`) does `lw t7,0(a0); addu t7,t7,a0` — so offsets **inside** a
payload are relative to that payload's own start, not the file base. Layout for `0x2d`:

```
+0x00  u32  offset (self-rel) -> array of COUNT self-rel offsets
+0x04  u16  COUNT
```

> The handler-table entries point **inside** functions that are only reached through an
> indirect table call, so a call-graph-following disassembler never disassembles them:
> they appear as data (`PTR_LAB_*`) with no containing function, and must be read as raw
> instructions to be verified.

### Verification

- **154 / 154** `I3D_BIN` files parse; every node type valid, no cycles.
- Root type is `0x52` on all 154.
- Name offsets land exactly on string boundaries.

---

## 2. `I3D_I3M` — animation

`FUN_003a7680` (fixup) / `FUN_003a7760` (unfixup) relocate exactly four pointers against
`param_1` = **the file base (offset 0)** — *not* `+0x10` like BIN — and flip the u16 at
**`+0x16`** (1 = offsets on disk).

```
+0x00  char[8]  'I3D_I3M\0'
+0x08  u32      version == 0x00020001         (FUN_003a7668)
+0x0c  u32      FILE SIZE                     (== len(file) on 106/106)
+0x10  u16      ?  (seen: 7, 13, 29, 31 -- tracks COMPS: 7/31 -> COMPS=4, 13/29 -> COMPS=5;
                    looks like a bitmask, purpose not established)
+0x12  u16      COMPS -- u16 slots per key    (4 in 24 files, 5 in 82)
+0x14  u16      TRACK_COUNT                   (FUN_003a7860)
+0x16  u16      fixup state (1 = offsets)
+0x18  f32      DURATION (seconds)
+0x1c  ptr      TRACKS -> TRACK_COUNT x 12
+0x20  ptr      pool X -- 16-byte elements (f32[4], a position); indexed by comp2.
                EMPTY in 59/104 (then +0x20 == +0x28 and the two alias).
+0x24  ptr      pool A -- f32[]; indexed by comp0 (time) and comp1 (1/dt)
+0x28  ptr      pool Y -- 8-byte elements (4 x s16); indexed by comp3 (unit quaternion)
                and comp4. Lies BETWEEN +0x20 and +0x24: the file order is always
                poolX -> pool Y -> poolA -> EOF (104/104).
```

**Track — 12 bytes** (fixup relocates `+0x00` and `+0x08`):

```
+0x00  ptr  NAME       <- PROVEN: FUN_003a78f8 (find_track_by_name) strcmps against it
+0x04  u16  ?          (equals the header COMPS on every track observed)
+0x06  u16  KEY_COUNT  (FUN_003a79e0)
+0x08  ptr  KEYS -> KEY_COUNT x COMPS x u16
```

Track names are joints: `jnt_armL1`, `jnt_armL2`, `jnt_armR1`, `jnt_toeR`, `jnt_top`…
`FUN_0050f028` binds them by name to a skeleton, confirming the role (see §5 for the name
table they resolve against).

**Keys are index-compressed.** `FUN_003a7a40(anim, track, key)`:

```c
idx = *(u16*)(track.keys + COMPS*key*2);   // stride = COMPS*2 bytes
return *(f32*)(poolA + idx*4);
```

`FUN_003a7ad8` walks component 0 of every key (`puVar4 += COMPS`) into an output array.

### Verification

- `+0x0c` == real file size: **106 / 106**.
- Track table ends **exactly** where the first name string begins: **104 / 104** non-empty.
- Key arrays are **contiguous** across all tracks with stride `COMPS*2` (on
  `ape_nrmb_run_b`: 57 tracks end at `0x122e`, and `+0x20` = `0x1230`).
- Component 0 decodes to times within `[0, DURATION]`: **104 / 104** non-empty.
- `ape_nrmb_run_b.i3d`: DURATION `0.4s`, times land on exact 1/30 steps
  (0, 0.0333, 0.0667 … 0.4) = **13 keys**, matching `KEY_COUNT` exactly ⇒ **30 fps source**.
- 2 files (`a_toy_c_takibi_on`, `toy_c_kemuri`) are **empty animations**: 0 tracks,
  0 duration, all four pointers = EOF, body filled with the `0xfe967699` debug sentinel.

### Key components 1..COMPS-1

There are **two** pools between `+0x20` and `+0x24`, not one. `+0x28` lies *between* them
(file order is always `poolX(+0x20) → poolY(+0x28) → poolA(+0x24) → EOF`, 104/104):

| comp | field | pool | element | meaning |
|---|---|---|---|---|
| comp0 | `+0x24` | pool A | `f32` | key **TIME** |
| comp1 | `+0x24` | pool A | `f32` | **`1/(t[k+1] - t[k])`** — reciprocal of the interval to the next key |
| comp2 | `+0x20` | pool X | 16 bytes (`f32[4]`) | a position; semantics open (see below) |
| comp3 | `+0x28` | pool Y | 8 bytes (`4 × s16`) | unit **QUATERNION** |
| comp4 | `+0x28` | pool Y | 8 bytes (`4 × s16`) | present only when `COMPS == 5`; semantics open |

Pool X is **empty in 59 of 104** files — then `+0x20 == +0x28` and the two readings alias.

**comp1** is precomputed so the evaluator lerps with a multiply instead of a divide, and is
exactly `0.0` on the final key (no successor). That is why comp0 and comp1 share pool A:
times and rates, both floats, are interned together. `30.0` is `1/(1/30)`; `15.0` is a
2-frame gap; `2.7273` (= 30/11) an 11-frame gap. **Verified: 41633 non-final + 2074 final
keys across all 106 files, 100%, zero exceptions.**

**comp3** is a unit quaternion, `4 × s16 / 32768`, from pool Y. **Verified:
31844 / 31844** referenced elements on `jnt_*` tracks are unit-length.

> "Unit" only to **~1.3e-4** — `s16/32768` quantisation leaves the norm short (a real key
> is `0.416718, 0.908966` → norm `0.999873`). Harmless for the rotation, but the glTF spec
> requires genuinely unit quaternions, so `ae3 gltf` normalises on export
> (`ae3tools.i3dgltf._keys`), while `ae3tools.i3manim.Track.sample` deliberately does
> **not**, returning the file's raw key at an exact key time (`slerp` normalises only
> between keys). Comparing raw keys against a normalising consumer measures the norm
> deficit rather than the rotation.

Element sizes are pinned by *exact* division on the camera paths (where pool X is large):
`1456/91`, `6256/391`, `2416/151` are all exactly **16.00**, and the same files' comp3
pools give exactly **8.00**. `arijigoku1_path` has 91 positions but **one** quaternion — a
camera that moves along a path at fixed orientation.

**Open:** comp2 / comp4 semantics.

- comp2 on `jnt_*` tracks is **not** the bone's local translation — matched against the
  real skeleton it lands a mean **0.40** units from the nearest bone (refuted, not merely
  unconfirmed).
- comp2's element carries `w == -32768` (`0x8000`) on **89.4%** of references (comp4:
  90.2%), where comp3's elements never do. The sentinel reliably signals *"not a
  quaternion"*; what the other three `s16` mean is not established.
- **Nothing in the conversion depends on it.** Skeletal animation needs the rotations, and
  those are exact — `ape_nrmb_run_b` applied to `ape_nrmb_body_b` renders a coherent,
  correctly looping run cycle (`t=0` and `t=0.4` identical).

### The quaternion REPLACES the bone's local rotation

The key rotation is applied **wholesale**, not as a delta on the bind pose. Translation and
scale come from the skeleton. Because `ape_nrmb_body_b`'s bind local rotations are all
identity (and `q ∘ identity == q`), the two readings are indistinguishable on apes;
`npc_aki` discriminates — it has bones whose bind local rotation is genuinely non-identity:

| bone | bind rot. deviation | `\|q(0) − bind\|` | `\|q(0) − I\|` |
|---|---|---|---|
| `jnt_taleR1` | 2.0000 (180° flip) | **0.0000** | 2.0000 |
| `jnt_fingerL12` / `R12` / `L14` / `R14` | 0.7071 | **0.0000** | 0.7071 |

The first key reproduces the bind local rotation **exactly**, on all 5 bones, a 180° flip
included. A delta reading predicts identity there and is off by 2.0.

> Two bones (`jnt_footL1`, `jnt_legL1`) look like counterexamples and are not: their bind
> deviation is ~0.002 = float noise, so their bind rotation *is* identity and the test
> cannot discriminate on them.

### The bind skeleton is NOT pure translation

Only `ape_nrmb_body_b` has an all-identity, pure-translation bind skeleton. Across the
corpus:

- **Rotation:** `npc_aki jnt_taleR1` and `car jnt_frontR1` deviate by **2.0** (180° flips);
  `ape_lod_* jnt_fingerL21` by 0.74.
- **Scale:** **83 bones** are non-uniformly scaled, worst **45×** (`a_ara_a_doukutu doa_1`
  = 46 / 39 / 6.5), across 30 models.
- What *is* true corpus-wide, and is what the glTF export relies on: **no shear** (worst
  off-axis column dot `9.4e-08` over 2151 bones) and **no reflections** (zero negative
  determinants). So every bone local matrix is an exact `T·R·S`, and TRS decomposition for
  glTF is exact. A consumer that overwrites the whole 3×3 with a pure rotation silently
  resets the 83 scaled bones to unit scale.

### glTF animation export

`ae3 gltf --anims` bakes each I3M track into the `.glb` as a glTF animation, so a glTF
importer constructs the animation player itself and needs no I3M parser. One `rotation`
channel per track, `LINEAR` (= slerp per spec, matching `i3manim.Track.sample`).

- **Bone nodes are TRS, not `matrix`** — glTF forbids animating a node that carries a
  matrix. Exact here (no shear / no reflections, above); `ae3 gltf --verify` round-trips
  `T·R·S` against the stored world matrices: worst deviation **4.5e-07** over 154 models.
- **Verified against the independent Python evaluator:** seeking a glTF animation player to
  fixed times and comparing every posed bone rotation *by name* against
  `i3manim.Track.sample` gives **395 samples, worst error 0.000000 rad**. The test is
  mutation-checked — perturbing one bone at one time by 0.5° is caught and localised;
  swapping the quaternion component order is caught at 180°.
- **Which anim file belongs to which model is NOT established** — that association lives in
  the level scripts (`.plc` / `.asq`), not in the I3M file. `--anims` pairs by name
  resolution as a *testing* convenience only. Track→bone binding by name is proven;
  file→model association is a guess and is labelled as one.

### Which fields the runtime actually reads

`FUN_003a7a40` and `FUN_003a7ad8` — the natural accessors — have **zero callers**: they are
dead library code. The live consumers are `0x50ef50` / `0x50ef68` / `0x50f110` / `0x50f15c`,
and they read the key data **directly**. No I3M routine dereferences `+0x20` or `+0x28` at
all (the module relocates them but never reads them).

The `1/32768` dequantise scale appears **nowhere** in `.text` as a constant. This is
consistent with the dequantise happening on the **VU**: `ITOF15` converts int→float with 15
fractional bits, which *is* divide-by-32768.

---

## 3. `I3D_I3C` — collision

An **indexed triangle collision mesh + a BVH**, one AABB and one collision material per
file. Read out of `FUN_003a7d08` (fixup); `FUN_003a7c50` is its exact inverse and confirms
every field independently.

**A third, distinct header** — `+0x10` is a *byte* flag here:

```
+0x00  'I3D_I3C\0'
+0x08  u32  version == 0x00030000
+0x0c  u32  FILE SIZE                     (== len(file) on 50/50)
+0x10  u8   fixup flag (1 = offsets)      <- byte: *(char *)(iVar2 + 0x10)
+0x14  u16  COUNT_A  -> A records @+0x18, stride 0x0c
+0x16  u16  COUNT_C  -> u32 name array @+0x1c
+0x18  ptr  A records          +0x1c  ptr  name array
```

A record (12 B): `+0x00` ptr NAME (`'ROOTNODE'` on every file), `+0x04` u16 COUNT_B,
`+0x08` ptr → B records (stride `0x30`).

B record (48 B) — the fixup relocates exactly `+0x24`, `+0x28`, `+0x2c`:

```
+0x00  f32[4]  AABB CENTRE       (w = 1.0)
+0x10  f32[4]  AABB HALF-EXTENT  (w = 0.0)
+0x24  ptr     ?  NULL on 50/50 -- unused in this corpus, purpose not established
+0x28  ptr     BVH root      (passed to FUN_003a7bb8, the fixup's pointer relocator)
+0x2c  ptr     VERTEX ARRAY  f32[4], stride 0x10
```

BVH node (12 B): `+0x00` u16 COUNT, **bit15 set ⇒ leaf, do not recurse** (`*node & 0x8000`,
compiled as `lhu` then `andi ...,0x8000` at `0x003bfb04`); `+0x02` **s8[3] box centre**;
`+0x05` **u8[3] box half-extent**; `+0x08` ptr → children (stride `0x0c`) or, at a leaf,
one **triangle**: `u16 v0, v1, v2, TRI_ID`.

### The quantised BVH bounds

The 6 bytes are a **centre + half-extent pair** — the same shape as the B record's f32
centre/half-extent, quantised to s8/u8 — expressed in the **parent node's** box:

```
box.centre = parent.centre + (s8_centre / 127) * parent.half
box.half   =                 (u8_half   / 127) * parent.half
```

The root node's parent frame is the B record's own f32 AABB. **Nested frames** are the
reason an absolute s8 min/max reading fails: the bounds are neither min/max nor
root-relative.

This is proven instruction-by-instruction against the query, the recursive BVH descent at
**`0x003bf9a0`** (two callers: `0x003bfbc8` = itself, and `0x003bfd80` = the public entry):

```
0x003bf9a4  lui   t7,0x0072          \  t7 = 0x00721760
0x003bf9ac  addiu t7,t7,5984         /
0x003bf9ec  lwc1  f0,0(t7)           <- f0 = *(float*)0x721760 = 0x3C010204
                                          = 0.007874015718698502 ; 1/f0 = 127.0000005
0x003bf9f0  lb    t6,2(t0)   \
0x003bf9f4  lb    t7,3(t0)    >  SIGNED   -> s8 CENTRE   (t0 = the BVH node)
0x003bf9f8  lb    t5,4(t0)   /
0x003bfa14  lbu   t4,5(t0)   \
0x003bfa1c  lbu   t7,6(t0)    > UNSIGNED  -> u8 HALF-EXTENT
0x003bfa24  lbu   t6,7(t0)   /
      ... mtc1 / cvt.s.w (int->float), and a2 = parent CENTRE, a3 = parent HALF ...
0x003bfa18  mul.s f8,f8,f0    \
0x003bfa20  mul.s f2,f2,f0     >  parent.half * (1/127)
0x003bfa28  mul.s f3,f3,f0    /
0x003bfa44  mul.s f7,f2,f7    \
0x003bfa48  mul.s f9,f3,f9     >  offset = (s8/127) * parent.half
0x003bfa4c  mul.s f10,f8,f10  /
0x003bfa68  mul.s f2,f2,f4    \
0x003bfa6c  mul.s f3,f3,f5     >  box.half = (u8/127) * parent.half
0x003bfa74  mul.s f8,f8,f6    /
0x003bfa7c  add.s f0,f0,f7    \
0x003bfa80  add.s f1,f1,f9     >  box.centre = parent.centre + offset
0x003bfa84  add.s f11,f11,f10 /
0x003bfbc8  jal  0x003bf9a0   ; RECURSE -- the new box becomes the child's frame
```

The signedness split (`lb` on +2/+3/+4, `lbu` on +5/+6/+7), the divisor **127** as an
explicit `1/127` constant in `.rodata`, and the **parent-relative frame** (the freshly
computed box is passed down as the next level's `a2`/`a3`) are all read directly off the
instruction stream. Data and code agree exactly.

Args are `a2` = parent centre, `a3` = parent half, `t0` = node, `t1` = flags — the PS2
toolchain's **MIPS EABI passes args in `a0-a3` AND `t0-t3`**, which is why `t0`/`t1` are
live on entry with no prior def.

**At runtime** the query dequantises each node's box, then makes an indirect call through
`*(s3+0x0c)` with the box; `sltu s2,zero,v0` / `beq s2,zero,...` **culls the whole subtree**
when the callback returns 0. A second optional callback (`*(s5+0x14)`, `0x003bfaf4`) runs
when installed. So it is a **generic visitor**: the test is a caller-supplied predicate, not
a hard-coded ray or sweep. `FUN_003a7bb8` (two callers, both internal:
`0x003a7c24` = itself, `0x003a7e58` inside `FUN_003a7d08`) is **not** a query walker — it is
purely the fixup's recursive pointer relocator.

### BVH bounds — data evidence

- **Containment: 47434 / 47434 nodes** across all 50 files — every node's decoded box
  contains every triangle beneath it. Enforced by `ae3tools.i3c.I3c.check_bounds()`.
- **Tightness:** 97.3% of axis checks fit within **1–2 quantisation units** of the actual
  contents — the encoder rounds outward by a unit or two, exactly as a conservative
  quantiser must. (The 1.3% looser ones are all axes whose parent half-extent is ≈0.0001,
  where one unit is microscopic; worst *absolute* slack is 0.11% of the part's size.)
- **The divisor is uniquely 127:** `/128` breaks containment on 47346 nodes and `/126` on
  45458 — the scale moves the centre as well as the extent, so it is not monotonic and the
  constant is pinned, not fitted.

### Verification

- `+0x0c` == real size, version, and fixup flag == 1 on **50/50**.
- `nverts == (ptr_0x28 - ptr_0x2c)/16 == max_vertex_index + 1` on **50/50** — two
  independent derivations agreeing. (There is **no explicit vertex count** in the record;
  the vertex block simply runs up to the BVH root.)
- `TRI_ID` is exactly the permutation `0..ntris-1` on **50/50**.
- Every BVH node's decoded box contains its subtree's triangles — **47434/47434**.
- `centre ± half_extent` reproduces the vertex bbox **exactly** (`coll_arabian_a`:
  −654.60/−135.00/−1956.42 .. 499.18/283.97/1573.31) ⇒ a tight AABB, not a guess.
- **Ground truth:** `col_cube_1x1x1.i3d` → centre (0,0,0), half-extent (0.5,0.5,0.5) =
  a 1×1×1 cube; 8 vertices at every combination of ±0.5; 12 triangles lying on exactly
  **6 distinct axis-aligned planes, 2 per face**. The file's own name states the answer.
- Renders are coherent: `coll_arabian_a` (8137 tris) is a winding path linking circular
  arenas; `a_ara_a_arijigoku1` is a radially-triangulated disc with half-extent **y == 0
  exactly** — an antlion sand-pit's flat trigger zone (the funnel is in the visual mesh).

### Collision materials (the `+0x1c` name array)

`coll_wall` ×46, `g_a_suna_01` ×3 (*suna* = sand), one empty. **One per file** — the surface
type is per-object, not per-triangle.

**The names do not track the filenames.** `a_ara_a_col_suna_col_a_ara_a_suna.i3d` — the file
with *suna* in its name — carries `coll_wall`, while the cave door
`a_ara_a_doukutu_col_doa1.i3d` carries `g_a_suna_01`. Neither can be inferred from the
other; read the name array.

**The material array is vestigial — the shipped game never reads it.** The fixup relocates
the name pointers and then nothing ever dereferences them. The I3C header has four sibling
accessors, consecutive in `.text`:

| accessor | returns | callers | notes |
|---|---|---|---|
| `0x003a8140` | `lhu v0,0x14(a0)` → **COUNT_A** | 4 (the consumers) | live |
| `0x003a8148` | A-record name by index (`+0x18`, stride `0x0c`) | 2 | live |
| `0x003a8178` | `lhu v0,0x16(a0)` → **COUNT_C** (materials) | 0 | dead |
| `0x003a8180` | material name by index (`+0x1c`, stride 4) | 0 | dead |

Three independent lines confirm the material accessors are dead:

1. A direct-`jal`/`j` scan of *all* of `.text` finds zero callers of `0x003a8178`/
   `0x003a8180` — scanning **every address** in `0x3a8140..0x3a81c0`, so a mis-identified
   entry point cannot manufacture the zero — and an immediate/pointer-materialisation scan
   adds zero `lui`/`lo` sites and zero stored pointer words (covering a vtable or
   function-pointer dispatch). Both scans are needed together: one cannot see a direct
   `jal`, the other cannot see an indirect call.
2. All four I3C consumers (`0x2406cc`..`0x240c80`) contain **zero** loads at offset `0x16`
   or `0x1c`. They read only their own object's `0/8/12/16`, and they reach for the
   *sibling* accessors (`COUNT_A`, A-name) constantly — so it is not that this code
   prefers inline reads.
3. The names are not in the ELF to compare against. `g_a_suna_01`: **0** occurrences.
   `coll_wall`: exactly **1**, at `0x005eb658`, with **zero** references — an orphan
   literal among RTTI/debug strings (`CollisionDetectorImpl::resize`, `CollisionPrim`).
   `ROOTNODE` (the A-record name on all 50 files): **absent**.

The asymmetry makes this forced rather than merely negative: the A-record name accessor next
door **has 2 callers**, so the name machinery *is* used — just not for materials.

Whatever selects surface behaviour (if anything does) lives elsewhere — the level data, or
the attribute-looking strings `solid`, `touch`, `damage`, `drag`, `getyou`, `parent` at
`0x005eb538`..`0x005eb568`. Nothing links those to I3C yet.

### Triangle winding

Evaluated with the OpenGL/glTF face normal `n = (b−a)×(c−a)`, the stored winding yields
**inward**-facing normals: all 12 of `col_cube_1x1x1`'s faces point inward, and
`coll_arabian_a` reads 159 up-facing against 7177 down-facing. Both are artefacts of the
**sign**, not the data — the outward (Godot-style collision) normal is the **negation** of
the OpenGL one. `ae3tools.i3c.godot_normal` returns the negated normal.

Empirically pinned on the closed cube (whose name states what it is): loaded in **file
order**, a ray dropped on `col_cube_1x1x1` hits **y = +0.5** — the top face, front-on.
Reverse the winding and the same ray hits **y = −0.5** (it falls through the top and hits
the floor from inside). With the sign flipped, the cube faces out and `coll_arabian_a`
becomes 7177 floors to 159 ceilings.

This is not cosmetic: any floor test of the form `n.y >= threshold` inverts under the wrong
convention, making every floor read as a ceiling. An importer's reported hit normal matches
`i3c.godot_normal` on all 36 test drops (worst **0.000000**), including sloped faces like
`(0.187, 0.972, 0.139)`, so the sign is pinned by test — **do not flip the file-order
winding.**

### `.col` transport

`ae3 i3c --format col` writes the SDK's own compact `.col` file
(`ae3tools.i3c.write_col`), not a Sony format. It is not OBJ because OBJ round-trips every
float through decimal text, needs an import step, and cannot carry the material name.
Geometry is emitted in **raw game units with no axis conversion** — the same space
`ae3 gltf` puts the models in, so a 77.92 u/s run and a ~14.1 u jump are directly comparable
against the level.

- **All 50 files are one group / one part**, and vertex `w` is **0.0 on all 15000
  vertices** — no per-vertex payload. `write_col` raises rather than silently dropping
  geometry if that ever stops being true.
- **Verified against the Python parse**, not by eye: counts, AABB, material, **36 ray
  drops** onto the real surface (worst **5.2e-05 u**, `0.000000` on the cube) and **36 hit
  normals** (worst **0.000000**). Mutation-checked: reversing the cube's winding is caught
  at 1.000000 u (`got y=-0.5 want y=+0.5`), raising every vertex by 0.02 u at 0.020024.

### Open

- `B+0x24` is NULL on all 50 files — purpose not established.
- Whether the original treats collision as **one- or two-sided** is not established: the
  query is a generic visitor and its predicate has not been read. One-sided
  (backface-culled) is the safe default.
- The bounds are decoded and proven against data and code, but no ELF routine has been read
  that *consumes* the tree at runtime, so the sweep/ray/epsilon detail of a query is not
  established. Nothing in the conversion depends on it: a physics engine rebuilds its own
  broadphase from the triangle soup.

---

## 4. Node-type census and geometry

### Node types (census over 154 `I3D_BIN` files)

`0x2a` Bone ×2151 · `0x2d` CombinedMesh ×253 · `0x46` BindPose ×253 (1:1 with `0x2d`) ·
`0x59` MeshInstance ×458 · `0x4b`/`0x4c` Mesh ×197/×274 · `0x4d` Submesh ×638 ·
`0x56` SubmeshPiece ×1869 · `0x47` IndexList ×10812 · `0x31` VertexWeights ×576 (21 files
only) · `0x25` Material ×385.

### Geometry is a VIF DMA packet

The vertex/UV blocks are **`UNPACK` commands** that must be *executed* into VU1 memory, not
flat arrays. A block is raw instead when `buf[data+0x8] != 1`. `ae3 i3dmesh` runs the VIF
interpreter; `ae3 gltf` reuses it.

- **Child count is the full 24 bits** per `FUN_003a85e0` (`uVar1 & 0xffffff`), not a u16.
- **UVs are one-per-index** (`ucount == indcount` on **10788/10788** index lists), and they
  accumulate across a piece's `0x47` lists together with `ind`.
- 4 models (`tra_a_antena`, `tra_a_base01`, `tra_a_pc_unit`, `toy_c_bg01`) have index lists
  whose second child is **empty — no UV buffer at all**; the missing UVs must be padded to
  keep `vt` aligned with `ind`.

### Verification

- **154/154 parse**, 0 failures.
- `a_ara_a_col_suna` → 8 verts / 12 tris forming a **perfect axis-aligned box**: corners at
  exactly `{-5,5} × {-15,-5} × {-5,5}`. Every `w == 1.0`; no NaN/Inf.
- Material names are meaningful romaji matching their stage: `g_a_kabe_02` (*kabe* = wall),
  `g_a_tento_01` (tent), `g_a_yuka_03` (*yuka* = floor), `s_mizugiwa01_eff1` (water's edge),
  `s_suimen2` (water surface).
- Renders are correct: `ape_slm_body_c` is a **monkey in T-pose** (headless — eyes/head are
  separate files); `arabian_a` is a ground plane with a tent and a tower.

### Geometry provenance

The mesh payload layout is a port of Murugo's MIT-licensed `mdl2obj.py` for *Rule of Rose*
(PS2), the same I3D engine family:
<https://github.com/Murugo/Misc-Game-Research/blob/main/PS2/Rule%20of%20Rose/mdl2obj.py>.

- **Derived independently from the ELF/corpus:** the container (§1) — `file+0x10` base,
  16-byte node, type = bits 24..30, count = bits 0..23 — and the roles `0x25` Material,
  `0x2d` CombinedMesh, `0x59` MeshInstance, `0x2a` Bone. The reference agrees on all of
  these (it reads the type as `buf[offs+0x7] & 0x7f` and slices at `+0x10`), which is mutual
  confirmation.
- **Inherited, not yet re-derived from the decompilation:** the byte-level payload field
  offsets (transform table at `root.data+0x14`; the `0x59`/`0x4c`/`0x4d`/`0x56` layouts; the
  4-byte index record). They reproduce correct geometry — strong evidence, but not read out
  of the ELF. If something looks wrong later, suspect these first. (The transform table at
  root payload `+0x14` is separately re-derived from the ELF in §5.)

---

## 5. Skin and skeleton

`ae3 gltf` exports **154/154 → .glb** with skeleton and skin; e.g. `ape_slm_body_c` imports
as one skeleton with **58 bones, 1 root**, every mesh skinned.

### `0x31` VertexWeights — the payload

Its fixup handler (`0x003a8c60`) relocates exactly **one** pointer at `+0x00`, so the
payload holds nothing else indirect:

```
+0x00  ptr   -> records (self-rel; always 8, i.e. immediately inline)
+0x04  u16   JOINT  -- index into the owning 0x59 MeshInstance's bone list
+0x06  u16   COUNT  -- records, INCLUDING zero-weight padding
+0x08  record[COUNT]:  { u32 vertex byte-offset (index = /0x10), f32 weight }
```

**One `0x31` node per joint**, each listing that joint's influence over a subset of the
piece's vertices — so a vertex is named by several `0x31` nodes, and taking only `vw[0]`
collapses a blend into a single bone. *Zero-weight records are padding* — 1152 of 9981 in
the corpus, and **all 1152 point at offset 0**; keeping them double-counts vertex 0.

### The `0x46` table is the INVERSE BIND matrix

`world(bone_list[t]) * bind(t)` is **exactly identity on 326/336** joints — i.e. wherever
the file stores its skeleton at its bind pose. The other 10 are LOD/prop files posed *away*
from bind (`ape_lod_*`, `ape_sas_body_b`, `ape_tan_b_sack_b`), where the product is a plain
rigid transform — the correct rest pose, not an error. So the skinning matrix is
`world(joints[t]) * ibm[t]`, exactly what glTF computes.

### Bone hierarchy (`0x2a`)

Its fixup handler (`0x003a6d68`) is a bare `jr ra` — **no pointers** — and the payload's
first u16 is the **PARENT index** (`0xffff` = root). Payloads *overlap*: bones sharing a
parent share one slot in a pooled u16 array. Bones are flat siblings in the node tree, so
the hierarchy exists **only** here.

The root `0x52` handler (`0x003a6f60`) relocates `+0x10`, `+0x14`, `+0x18` — which
re-derives from the ELF the transform table at root payload `+0x14`. The table holds
**world** matrices; glTF wants parent-relative, so local = `inverse(world(parent)) *
world(self)`.

### Bone names

No bone *node* carries a name (all `0x2a` share one `extra` pointing at an empty string),
but the model has a separate **sorted name table** hanging off the ROOT payload:

```
root+0x14  ->  transform table   (nbones x 0x40)
root+0x18  ->  u32 array         (nbones+1)
root+0x1c  ->  u16 PERMUTATION   (nbones)   name order -> BONE INDEX
               name blob follows the permutation immediately:
               nbones NUL-terminated names, sorted ASCII-ascending
```

Each region ends *exactly* where the next begins. Sorted names + a permutation is a
**binary-search name→index map** — precisely what the runtime does: `FUN_003a78f8`
(find_track_by_name) strcmps the track name, and `FUN_0050f028` packs each track name into a
16-byte key (`FUN_0035ab88`, via `qfsrv`) to build its binding table. So **tracks bind by
NAME**, not by index. `ae3tools.i3manim.bone_names()` reads the table.

Nothing points *at* this table — a pointer-following parser will never find it, which is why
it was easy to miss. Offsets inside payloads are relative to the **payload base**, not to
`+0x10`.

**Verified:** on **154/154** models the permutation is a valid permutation of
`0..nbones-1` and the names are sorted and printable. **Semantically**, on
`ape_nrmb_body_b` all 57 parent links agree with what the names mean —
`jnt_armL2`→`jnt_armL1`, `jnt_fingerL11`→`jnt_handL`, `jnt_toeL`→`jnt_footL2`,
`jnt_eyeL`/`jnt_zura`→`jnt_head`. A wrong permutation cannot produce 57 coherent anatomical
links by chance. `ape_nrmb_body_b` imports as one skeleton with **58/58 bones carrying real
names**, and all **57** tracks of `ape_nrmb_run_b` bind by name with zero misses.

### Verification

- Weights **sum to 1.0** and cover **every** vertex on **88/88** skinned pieces (21 files);
  offsets are all 16-aligned and `max(offset)/16 == vcount-1`.
- **Max 4 influences per vertex** — the classic hardware-skinning limit, so `JOINTS_0` /
  `WEIGHTS_0` VEC4 is exact, never lossy.
- Bone parents form a **valid single-rooted tree, no cycles, parent index < child index**
  on **154/154** files.
- `ae3 gltf --verify` recomposes each bone's world matrix by walking the emitted hierarchy:
  **0.00000000** deviation over 154 files.
- **Round trip:** an independent re-implementation of the glTF skinning formula, reading the
  `.glb` bytes back, reproduces the OBJ rest pose on **154/154** models, worst delta **4e-8**.
- **Semantic proof** (the rest pose proves nothing — the skin matrices are identity at bind,
  so *any* weights reproduce it): rotating bone 9 of `ape_slm_body_c` swings exactly **one
  arm** as a connected chain while the torso, legs and other arm stay put; bone 7 (parent of
  both arms) swings **both arms and tilts the torso, legs planted**.

### Why rigid pieces are skinned too

A piece with no `0x31` is stored in its bone's LOCAL space. A glTF node is promoted to a
skeleton bone **only** if a skin names it as a joint, so parenting a rigid piece to its bone
node would leave a 58-bone model importing as 25 bones plus loose nodes. Binding each rigid
piece 1.0 to its bone under an **identity-IBM** skin (identity leaves the vertex in
bone-local space, which is exactly right) keeps the whole rig in one skeleton — what the I3M
tracks need to target.

### Open

- Two bones in the whole corpus (`npc_aki` 27 and 28, of 2151) are **degenerate** — zero X
  basis, singular world matrix. A singular parent destroys information, so their children are
  re-rooted with their world matrix intact. Neither is referenced by geometry.

---

## 6. `.tm2` textures

Standard Sony **TIM2**, decoded by `ae3 tm2`. The picture header is confirmed against
`TexDb::createTM2` (`0x0028ab1c`, named by the assert string
`static void* TexDb::createTM2(int, int, int, bool)` at `0x006ee6c0`), which **builds** a
TIM2 in RAM field by field, pinning every offset:

```
sb t7(4),  4(s2)   +0x04 version=4        sb zero,  32(s2)  +0x20 pict_format
sb zero,   5(s2)   +0x05 format           sb t4(1), 33(s2)  +0x21 mipmap_count
sh t4(1),  6(s2)   +0x06 picture count    sb zero,  34(s2)  +0x22 clut_type
sw t6,    16(s2)   +0x10 total_size       sb t7(3), 35(s2)  +0x23 image_type=3 (RGBA32)
sw zero,  20(s2)   +0x14 clut_size        sh s3,    36(s2)  +0x24 width
sw t5,    24(s2)   +0x18 image_size       sh s4,    38(s2)  +0x26 height
sh t7(48),28(s2)   +0x1c header_size=48   sd s1,    40(s2)  +0x28 GsTex0
sh zero,  30(s2)   +0x1e clut_colors      sd t7(96),48(s2)  +0x30 GsTex1
```

The tail assembles a GS `TEX0`: `sra s1,s1,6` (TBW = width/64) then `dsll s1,s1,14` (bits
14–19); `FUN_0028aaf4` is `log2ceil`, feeding TW/TH at bits 26–29 / 30–33; TCC = bit 34. No
code compares the `'TIM2'` magic (zero `lui 0x324d`/`0x4954` sites) — the loader trusts the
format. The ELF also *embeds* a TIM2 at `0x00650ca0`: a 512×512 IDTEX8 `fontcache`,
allocated blank (its whole CLUT is zero) and filled at runtime.

**Corpus:** 256 files / 258 pictures. **Everything is palettized** — 193 IDTEX8, 65 IDTEX4,
zero direct-colour — so only two decode paths are needed. Zero size mismatches across the
217 non-mipmapped pictures.

Two things the ELF does not settle, both proven from data:

- **Alpha is 0..128, not 0..255.** Across all **45072** RGBA32 CLUT entries, **zero** bytes
  exceed `0x80`, the max is exactly `0x80`, and 38978 (86%) are exactly `0x80`. A 0..255
  convention would be dominated by `0xff`. Alpha is rescaled `*255/128`; skipping it makes
  every texture half-transparent.
- **The 256-entry CLUT is CSM1-swizzled** — undo by exchanging bits 3 and 4 of the index
  (`(i & 0xE7) | ((i & 8) << 1) | ((i & 16) >> 1)`). The game never unswizzles: it DMAs the
  palette to the GS in CSM1 order, so no code path shows it. Confirmed **visually** — as
  stored, `a_tra_a_ayasiisyasin` is shot through with orange speckle; unswizzled it is
  clean. 16-entry CLUTs have no bit 4 and are unaffected.

**UV `v` is NOT flipped.** TIM2 rows and UV `v` both run top-down, so `v` passes straight
through to glTF (whose TEXCOORD origin is the image top-left). Proven by the colour-coded
variants: with `v` as-is, `ape_nrmc_pants_b` renders **cyan** and `ape_nrmy_pants_b` renders
**yellow**, matching the `c`/`y` naming (and `ape_nrmb` = blue shorts); flipped, both come
out brown/peach and nearly identical.

**A material name IS the `.tm2` basename** — 365/365 non-empty materials match across the
corpus (the only misses are empty names). But basenames **collide across stages**
(`ape_nrm01_b` exists under both `toyhouse_c` and `arabian_a`), so resolve within the
model's own stage; a bare recursive glob silently picks whichever it finds first.

> **Beware the flat-centroid shortcut.** Sampling one texel at a triangle's UV *centroid*
> and filling flat looks like texturing but is not: on these atlases — 64×64 grids of flat
> colour patches with **hard seams** — any triangle whose centroid lands across a seam takes
> the wrong patch. It also cannot show texture detail, so it can never confirm the UVs.
> Interpolate UV per pixel.

---

## CLI reference

All tools are subcommands of `ae3` (the `ae3tools` package):

```
ae3 i3d      <file>            # I3D_BIN tree dump / I3D_I3M header + tracks
ae3 i3d      --census <files>  # aggregate node-type census
ae3 i3dmesh  --out DIR <file>  # I3D_BIN geometry -> OBJ + MTL (rest pose); --stats parses only
ae3 gltf     --out DIR <file>  # geometry + skeleton + skin -> .glb; --verify; --anims
ae3 i3c      --out DIR --format obj|col|both <file>   # collision -> OBJ and/or .col
ae3 i3manim  <file>            # I3D_I3M evaluator + bone name table
ae3 render   <file>            # software-render a model/animation turntable (needs Pillow)
ae3 tm2      <file>            # TIM2 -> PNG (needs Pillow)
```

Python API highlights (all under `ae3tools`): `i3d.I3d` (BIN node tree: `.root`, `.walk()`,
`.name(n)`, `.payload(n, size)`) and `i3d.I3m` (`.tracks`, `.key_times(t)`,
`.key_indices(t)`, `.pool_a_floats()`); `i3manim.Track.sample` and `i3manim.bone_names()`;
`i3c.write_col`, `i3c.I3c.check_bounds()`, `i3c.godot_normal`; `i3dgltf._keys`.

Basenames are **not** unique across the corpus (`sw_yuka.i3d` exists under both
`toyhouse_c/bg` and `arabian_a/bg`), so outputs mirror the source directory tree rather than
flattening to basename — flattening silently overwrites collisions. The same trap applies to
material→`.tm2` resolution (§6) and any recursive glob.

---

## Provenance and methodology

Every structural fact above is read from the game's stripped R5900 ELF by disassembly, and
cross-checked against corpus-wide statistics over the shipped data (154 models, 106
animations, 50 collision files, 256 textures). The recurring division of labour: **the ELF
gives structure reliably; semantics need a second source or an empirical test on the whole
corpus.**

- **Fixup / unfixup pairs prove field roles.** For each container the fixup routine and its
  exact inverse independently name which words are pointers (`FUN_003a85e0`/`FUN_003a8490`
  for BIN, `FUN_003a7680`/`FUN_003a7760` for I3M, `FUN_003a7d08`/`FUN_003a7c50` for I3C).
- **Two disassembly scans are used together and neither alone is a proof:** a direct-`jal`
  scan (an indirect-call scan cannot see direct jumps) and an immediate/stored-pointer
  materialisation scan (which cannot see a direct `jal`). Together they establish that a
  routine is dead code or that a value is never dereferenced — e.g. the vestigial I3C
  material accessors (§3) and the dead I3M key accessors (§2).
- **The BVH query `FUN_003bf9a0` was located by its structural fingerprint**, not by a magic
  or a caller: byte loads at offsets +2..+7 with the exact signedness split
  `lb+2 lb+3 lb+4 lbu+5 lbu+6 lbu+7`. Of ~1012 such loads in `.text`, exactly one cluster in
  the 6.5 MB image matches — and the hypothesis (s8 centre + u8 half-extent) *was* the
  search key, so the match confirms the reading.
- **Tests are mutation-checked** — a passing test is trusted only after it has been seen to
  fail on a deliberate perturbation (a 0.5° bone rotation, a reversed cube winding, a raised
  vertex). Anim and collision cross-check an independent importer against the Python
  evaluator; the glTF skinning is round-tripped through a second re-implementation of the
  spec formula.

Related specs: `docs/formats/DATA_BIN.md` (the VFI/PCK container these files are extracted
from) and `docs/formats/EXTRACTION.md`.
