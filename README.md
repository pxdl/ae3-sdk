# ae3-sdk

Data-free engine and toolkit for Ape Escape 3's (PS2) data formats — today the
sequenced BGM and embedded sound-effect playback systems plus extraction,
database, model/animation, and FMV tooling:

- `core/` — a C11 synthesizer that reproduces the game's BGM and embedded-SE
  playback (SPU2-accurate ADPCM/noise voices, Gaussian resampling, envelopes,
  both sequencers, the shared 48-slot voice pool, and streaming SPU2 reverb).
  Bit-exact where gated, engine-agnostic, no OS assumptions.
- `harness/` — native dev harnesses: `wavdump`/`serender` (headless assembled
  renders), and `bgmplay` (SDL2 BGM player/visualizer).
- `tools/` — Python toolkit (`ae3tools`): DATA.BIN (VFI) extraction, Exdb
  tables, BGM/SE inspection and rendering, I3D model/animation conversion,
  FMV, and subtitles.
- `wasm/` — standalone WebAssembly build of `core/` + the `@ae3/synth` JS
  binding, render-identical to native (gated: synthetic vectors in CI, full
  corpus privately).
- `extract-web/` — TypeScript browser-side extraction (`@ae3/extract`):
  ISO9660 → DATA.BIN (VFI) → deflate/PCK → assets → OPFS cache, plus the Exdb
  (BgmDesc) parser. Byte-identical to the Python extractor (gated: synthetic
  fixtures in CI, full corpus privately).
- `docs/formats/` — canonical format specifications (DATA.BIN/VFI, BGM, Exdb…)
  that every implementation here cites.

**Scope and growth.** This is the single home for every reusable, data-free
implementation of the game's formats, kept next to the specs it implements.
The placement rule inside it: code that must run at *runtime inside a shipped
program* (the synth core, its WASM build, browser-side extraction — and later,
runtime loaders for further formats as consumers need to load them live) is a
library package; code a developer runs at a desk to convert or inspect files
is an `ae3tools` subcommand, and the Python side doubles as the offline oracle
the library implementations are verified against. Audio simply came first: the
player needs music loaded live, while the other formats have so far only
needed offline conversion. Larger downstream projects (a decompilation, an
engine port) get their own repositories and consume this one — they never
merge into it.

**This repository ships no game data.** Everything is read at runtime from the
user's own disc; see `NOTICE.md` for the full data policy and provenance notes.

License: MIT (see `LICENSE`, `NOTICE.md`).
