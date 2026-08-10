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

TIM2 images and sprite sheets use the same `Vfi`. The scanner reads direct
`.tm2` files and `.pck` / `.pck.sz` containers with bounded concurrency.
`container` is the efficient persistence hook: it runs once for each container
that yielded an image, while the original bytes are already in memory.
Proven TIM2/PCK/SZ format violations after a successful VFI read are returned
as bounded `scan.issues`; source reads, aborts, resource failures, callbacks,
and unexpected exceptions still reject the scan.
`texture` remains available when a consumer needs individual TIM2 payloads:

```ts
const scan = await scanImageTextures(disc.vfi, {
    progress(done, total, path) {
        console.log(`${done}/${total}: ${path}`);
    },
    async container(entry, storedBytes) {
        await cache.write(`image-containers/${entry.entryOff}.bin`, storedBytes);
    },
});
const textures = scan.textures;
// scan.issues contains only bounded, sanitized TIM2/PCK/SZ format failures.
const source = await readImageTexture(disc.vfi, textures[0]);
const picture = decodeTim2(source, 0);
// picture.width, picture.height, picture.rgba
```

`ImageTexture.pictures` records every picture in a multi-picture TIM2. Callback
delivery remains bounded by the same scan concurrency, so persistence can make
progress without opening thousands of files at once.
`decodeTim2` supports the game's indexed 4-bit and 8-bit textures plus direct
RGBA16, RGB24, and RGBA32 pictures. It linearizes CSM1 palettes and expands the
PS2 alpha range from 0..128 to browser RGBA 0..255.
The returned dimensions always remain the declared TIM2 dimensions; decoding
does not crop transparent margins.
`ImageTexture.role` first uses exact UIS/I3D references in the texture's own
package, then unambiguous package context. Assets still unresolved are checked
against references in other packages. Only when no format reference exists
does a `ui_` member-name prefix provide a lower-confidence sprite fallback.
`roleEvidence` records which tier decided the result; filenames never override
UIS, I3D, or package evidence.

Stage previews live in the region-prefixed
`etc/warpgate/str/stages.bin` raw fixed-slot packfile. Other AE3 packfiles put a
nonzero stored-slot stride at header offset `0x18`; `parsePackfile` inflates each
bounded SZ slot before parsing the same member stream. IPC inspection identifies
the meaningful IPU frame separately from its qword padding:

```ts
const entry = disc.vfi.entries.find(candidate =>
    candidate.path.endsWith("/etc/warpgate/str/stages.bin"))!;
const source = await disc.vfi.read(entry);
const pack = await parsePackfile(source, entry.path);
const member = pack.slots[0].members.find(candidate => candidate.kind === "ipc")!;
const ipc = packfileMemberBytes(pack, member);
const info = inspectIpc(ipc, member.name);
const ipum = ipcToIpum(ipc, member.name);
// info.ipuFlags comes from IPC offset 0x0c; ipum contains one unpadded frame
```

AE3's preview frames store macroblocks in column-major order. `ipcToIpum`
preserves that order; decoders that assume raster-order IPU macroblocks will
produce a 16x16-tile-scrambled picture. A consumer must select a
column-major-aware IPU decoder rather than treating the bridge as conventional
raster-order MPEG video.

I3D assets are discovered and decoded from the same containers. Model catalog
entries include their exact bone-name table so consumers can match skeletal
animations without guessing from filenames:

```ts
const assets = await scanModelAssets(disc.vfi);
const modelAsset = assets.find(asset => asset.kind === "model")!;
const model = decodeI3dModel(
    await readModelAsset(disc.vfi, modelAsset),
    modelAsset.fileName,
);
// model.meshes: geometry, UVs, material names, skin joints, inverse bind matrices
// model.bones: exact names, parents, and world matrices

const animationAsset = assets.find(asset => asset.kind === "animation")!;
const animation = decodeI3dAnimation(
    await readModelAsset(disc.vfi, animationAsset),
    animationAsset.fileName,
);
// animation.tracks: key times and normalized xyzw quaternion rotations
```

`decodeI3dCollision` expands the I3C BVH leaves into indexed triangle geometry.
`readModelPackage` exposes sibling TIM2 textures and animations without a second
container parser. The scanner covers `I3D_BIN`, `I3D_I3M`, and `I3D_I3C`;
camera/non-skeletal tracks whose component 3 does not reference the proven
quaternion pool retain their time/name metadata with an empty rotation channel.

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

Lower-level pieces (`Iso9660`, `Vfi`, `inflateSz`, `unpackPck`, `inspectTim2`,
`decodeTim2`, `parseExdb`, `bgmSongTable`, …) are exported individually; each
is a direct port of its Python oracle in `tools/ae3tools/`, and
`docs/formats/` is the spec for both.

## Bounded file CLI

`bin/ae3-extract.mjs` exposes the same ISO/VFI parsers to local tooling without
loading a disc image or `DATA.BIN` into memory. It accepts a versioned,
data-free request and streams selected extents through bounded `FileHandle`
reads:

```json
{
  "schemaVersion": 1,
  "iso": [
    { "path": "SYSTEM.CNF", "output": "input/SYSTEM.CNF" }
  ],
  "vfi": [
    {
      "container": "DATA.BIN",
      "path": "irx/3.0/sg2iopm1.irx",
      "output": "input/sg2iopm1.irx"
    }
  ]
}
```

```sh
node bin/ae3-extract.mjs \
  --iso /path/to/disc.iso \
  --request request.json \
  --output build/extracted \
  --manifest build/locator.json
node bin/ae3-extract.mjs --schema
```

The JSON manifest records schema version, source hashes, ISO/VFI byte locators,
sizes, and SHA-1/SHA-256 for every extracted entry. Requests can name only ISO
root files and traversal-free output/VFI paths. The CLI contains no
title-specific hashes or paths; callers own target identity validation.


## Testing

- `npm run typecheck` / `npm test` — the public gate: synthetic fixtures
  (a miniature ISO/VFI/PCK/EDB/STR built at test time, no game data) exercise
  every parser and the end-to-end `openDisc` path under Node (which ships
  `DecompressionStream` and runs `.ts` natively).
- The real gate is private (needs a disc): differs extract the full asset and
  22-movie sets through this package, byte-compare MPEG/WAV/SRT outputs against
  the Python tools, and compare MPEG metadata with `ffprobe`.

Node ≥ 24 (or any current browser toolchain) — the package is consumed as
TypeScript source; there is no build step.
