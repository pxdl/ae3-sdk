# ae3tools — desktop toolkit for Ape Escape 3 data

Importable Python package + one `ae3` CLI with a subcommand per tool. Reads
everything from the user's own game disc; ships no game data.

```bash
pip install ./tools[images]     # [images] adds Pillow for render/tm2
ae3 --help
```

| command | does |
|---|---|
| `ae3 vfiparse DATA.BIN` | inspect the VFI archive: header + entry table |
| `ae3 extract --data DATA.BIN [--glob PAT] [--all] [--manifest]` | extract assets (expands `.sz`/`.pck`) |
| `ae3 databin-scan DATA.BIN` | scan for embedded ELFs |
| `ae3 exdb FILES [--schema] [--tsv]` | dump the self-describing Exdb design tables |
| `ae3 songvol --exdb bgm_desc.exdb.exdb` | authored per-song BGM volumes |
| `ae3 i3d FILES` | I3D container node trees |
| `ae3 i3dmesh FILES --out DIR` | meshes → OBJ |
| `ae3 gltf FILES --out DIR` | models + skins + animations → `.glb` |
| `ae3 i3manim FILES` | animation tracks + bone name tables |
| `ae3 i3c FILES --out DIR` | collision → OBJ + BVH stats |
| `ae3 render MODEL.i3d OUT.png` | software turntable render |
| `ae3 tm2 FILES -o DIR` | TIM2 textures → PNG |
| `ae3 strextract --data DATA.BIN` | demux FMVs (`.m2v` + `.wav`) |
| `ae3 sbt2srt --data DATA.BIN` | FMV subtitles → `.srt` |
| `ae3 fmv2mp4 DIR [OUT]` | `.m2v`+`.wav` pairs → playable `.mp4` (ffmpeg) |

Format documentation: `../docs/formats/`. The library is importable
(`from ae3tools import exdb, vfiparse, …`) for scripting pipelines.
