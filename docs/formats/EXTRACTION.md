# EXTRACTION.md — normative asset manifest for the BGM player

The exact set of assets the browser player (and any other consumer) must pull
from the user's own disc, with their VFI paths inside `DATA.BIN`. Pinned from a
`vfiextract --manifest` run against the US disc (`SCUS_975.01`); DATA.BIN holds
3,994 VFI entries total.

Chain: **ISO file → ISO9660 filesystem → `DATA.BIN` → VFI archive → assets.**
`.sz` entries are raw deflate (browser: `DecompressionStream('deflate-raw')`);
`.pck` entries are the PCK sub-container (see DATA_BIN.md). Both expansions are
loss-free and verified (400/400 `.sz` inflate to their exact declared size).

## 1. Instrument banks + sequences (played directly)

    debug/us/sound/bgm/*.hd     62 files   bank headers (programs/tones)
    debug/us/sound/bgm/*.bd     62 files   bank bodies (ADPCM waveforms)
    debug/us/sound/bgm/*.mid    68 files   sequences

192 plain (uncompressed) VFI entries, ~50 MB total. Stems pair `.hd`/`.bd`
directly; the 6 orphan sequences pair to banks via `bgm_desc` (below).

## 2. Mastering / cue database

    debug/us/static/exdb_sound.pck.sz     -> PCK -> bgm_desc.exdb.exdb

The `ExdbBgmDesc` table: cue name → (midi, vh, vb, volume_scale, reverb
class). Source of the song list, orphan→bank pairing, and authored song
volumes (`songvol = trunc(127 * volume_scale * slider * dolby)`). The same PCK
carries the per-area SE descriptor tables (125 files; not needed for v1).

## 3. Sound config (cue layer, later milestone)

    debug/us/startup/exdb_common.pck.sz   -> PCK -> sound_config.exdb.exdb

Ducking spec + master volumes. NOT needed for v1 playback; documented here
because the cue layer (M8+) will want it. Note it is in `startup/exdb_common`,
not `static/exdb_sound`.

## 4. Driver donors (pitch table + reverb preset)

    irx/3.0/sg2iopm1.irx    (37,191 B)   IOP driver module: the PITCH table
    irx/3.0/libsd.irx       (30,085 B)   SPU2 reverb preset (STUDIO_C, depth 30)

Both are plain VFI entries inside DATA.BIN — the whole player feeds from the
single `DATA.BIN` file; nothing else on the disc is required.

## 5. Region note

All paths above are the US disc (`SCUS_975.01`). Other regions likely move
`debug/us/` to another prefix; consumers should glob-tolerate the prefix and
report what they found rather than hard-fail.

## Verification

A consumer's extraction output must byte-match `vfiextract`'s for every asset
above (the reference differ used to validate the TS extractor runs exactly
this comparison). Re-pin this manifest with `ae3 extract --data DATA.BIN
--manifest` whenever DATA_BIN.md changes.
