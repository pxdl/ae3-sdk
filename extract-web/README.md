# @ae3/extract — client-side asset extraction

TypeScript port of the extraction chain, for the browser player: the user
points the app at their own Ape Escape 3 ISO and everything the synth needs
is pulled out client-side. Nothing is uploaded; this package contains no game
data.

```
ISO file  →  ISO9660  →  DATA.BIN  →  VFI  →  .sz (deflate) / .pck  →  assets  →  OPFS
```

The asset set is normative in [`../docs/formats/EXTRACTION.md`](../docs/formats/EXTRACTION.md):
the 62 bank pairs + 68 sequences, `bgm_desc.exdb` (song list, orphan→bank
pairing, authored song volumes — parsed live from the user's disc), and the
two IRX driver donors (pitch table, reverb preset). Paths are matched
region-tolerantly by suffix; v1 accepts plain 2048-byte-sector `.iso` only.

## Use

```ts
import { BlobSource, openDisc, OpfsCache } from "@ae3/extract";

const disc = await openDisc(new BlobSource(isoFile));   // throws with a clear
disc.songs;          // 68 songs: name, hd, bd, authored songvol   // reason if
disc.assets;         // located VfiEntries (banks, sequences, irx) // not AE3
await disc.vfi.read(disc.assets.mid[0]);                // stream one asset

const cache = await OpfsCache.open(disc.cacheKey);      // per-disc OPFS cache
```

Lower-level pieces (`Iso9660`, `Vfi`, `inflateSz`, `unpackPck`, `parseExdb`,
`bgmSongTable`, …) are exported individually; each is a direct port of its
Python oracle in `tools/ae3tools/` and `docs/formats/` is the spec for both.

## Testing

- `npm run typecheck` / `npm test` — the public gate: synthetic fixtures
  (a miniature ISO/VFI/PCK/EDB built at test time, no game data) exercise
  every parser and the end-to-end `openDisc` path under Node (which ships
  `DecompressionStream` and runs `.ts` natively).
- The real gate is private (needs a disc): a differ extracts the full asset
  set through this package and byte-compares every file against the Python
  extractor's output, and checks the song table against `bgmplay --songs`.

Node ≥ 23.6 (or any current browser toolchain) — the package is consumed as
TypeScript source; there is no build step.
