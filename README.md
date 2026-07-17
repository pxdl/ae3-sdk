# ae3-sdk

Data-free engine and toolkit for Ape Escape 3's (PS2) data formats — today the
music system end-to-end (the current milestone) plus extraction, database,
model/animation and FMV tooling:

- `core/` — a C11 synthesizer that reproduces the game's BGM playback
  (SPU2-accurate ADPCM voices, gaussian resampling, envelopes, the sound
  driver's sequencer and 48-slot voice pool, streaming SPU2 reverb).
  Bit-exact, engine-agnostic, no OS assumptions.
- `harness/` — native dev harnesses: `wavdump` (headless render/inspect) and
  `bgmplay` (SDL2 player/visualizer).
- `tools/` — Python toolkit (`ae3tools`): DATA.BIN (VFI) extraction, Exdb table
  dumping, I3D model/animation conversion, FMV and subtitle tools.
- `wasm/` — WebAssembly build of `core/` + JS binding (arrives in W2).
- `extract-web/` — TypeScript browser-side extraction package (arrives in W3).
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
