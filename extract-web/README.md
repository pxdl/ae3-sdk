# @ae3/extract — client-side asset extraction

TypeScript port of the extraction chain for browser players and asset viewers:
the user points an app at their own Ape Escape 3 ISO and supported assets are
read client-side. Nothing is uploaded; this package contains no game data.

```
ISO file  →  ISO9660  →  DATA.BIN  →  VFI  →  assets / FMV demux  →  OPFS
```

The audio asset set is normative in
[`../docs/formats/EXTRACTION.md`](../docs/formats/EXTRACTION.md): the 62 bank
pairs + 68 sequences, `bgm_desc.exdb` (song list, orphan→bank pairing, authored
song volumes — parsed live from the user's disc), and the two IRX driver donors
(pitch table, reverb preset). FMV/container behavior is normative in
[`../docs/formats/FMV.md`](../docs/formats/FMV.md). Paths are matched
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

FMVs use the same parsed `Vfi`; catalog discovery reads no movie payloads.
Call `vfi.read(asset.movie)` only when a selected movie is prepared or exported:

```ts
const movies = locateFmvAssets(disc.vfi);
const movie = movies[0];
const metadata = await inspectFmvAsset(disc.vfi, movie.movie);
const { header, video, wav, videoInfo } =
    demuxFmv(await disc.vfi.read(movie.movie), movie.movie.path);
const cues = movie.subtitleBin && movie.subtitleSbt
    ? parseFmvSubtitles(await disc.vfi.read(movie.subtitleBin),
        await disc.vfi.read(movie.subtitleSbt), movie.name)
    : [];
const seekIndex = indexMpeg2SeekPoints(video);
const captions = subtitlesToVtt(cues);
```

`video` is the untouched MPEG-2 elementary stream. `wav` is decoded PCM.
`videoInfo.sampleAspect` is the proven game presentation SAR 7:6 layered over
the unchanged stream. Parsing validates the complete STR structure and strict
UTF-8 subtitle sidecars; malformed or truncated inputs throw with an offset.
`inspectFmvAsset` reads only the header and first complete video chunk, so a
catalog can expose dimensions, field order, frame rate, and display aspect
without loading whole movies. `indexMpeg2SeekPoints` returns picture count and
GOP/I-picture byte offsets for bounded playback or packet-preserving export.

Lower-level pieces (`Iso9660`, `Vfi`, `inflateSz`, `unpackPck`, `parseExdb`,
`bgmSongTable`, …) are exported individually; each is a direct port of its
Python oracle in `tools/ae3tools/` and `docs/formats/` is the spec for both.

## Testing

- `npm run typecheck` / `npm test` — the public gate: synthetic fixtures
  (a miniature ISO/VFI/PCK/EDB/STR built at test time, no game data) exercise
  every parser and the end-to-end `openDisc` path under Node (which ships
  `DecompressionStream` and runs `.ts` natively).
- The real gate is private (needs a disc): differs extract the full asset and
  22-movie sets through this package, byte-compare MPEG/WAV/SRT outputs against
  the Python tools, and compare MPEG metadata with `ffprobe`.

Node ≥ 23.6 (or any current browser toolchain) — the package is consumed as
TypeScript source; there is no build step.
