"""The `ae3` command: one CLI, one subcommand per tool.

Each subcommand dispatches to a module's main() with argv rewritten so the
tool's own argparse help reads `ae3 <sub> ...`. Modules import lazily, so
optional dependencies (Pillow for render/tm2, ffmpeg for the FMV tools) only
matter for the subcommands that use them.
"""
import importlib
import sys

# sub -> (module, one-line help)
COMMANDS = {
    "vfiparse":     ("vfiparse",     "inspect a DATA.BIN VFI archive: header + entry table"),
    "extract":      ("vfiextract",   "extract assets from DATA.BIN (VFI -> .sz/.pck expansion)"),
    "databin-scan": ("databin_scan", "scan a raw DATA.BIN for embedded ELFs"),
    "exdb":         ("exdb",         "dump the self-describing Exdb design-data tables"),
    "songvol":      ("bgm_songvol",  "per-song BGM volumes from bgm_desc.exdb"),
    "i3d":          ("i3d",          "parse I3D_BIN/I3M containers: node tree + payloads"),
    "i3c":          ("i3c",          "I3D_I3C collision: triangle soup + BVH"),
    "i3dmesh":      ("i3dmesh",      "I3D_BIN mesh geometry -> OBJ"),
    "gltf":         ("i3dgltf",      "models + skins + animations -> glTF (.glb)"),
    "i3manim":      ("i3manim",      "I3D_I3M animation evaluator + bone name table"),
    "render":       ("i3drender",    "software-render a model/animation turntable (needs Pillow)"),
    "tm2":          ("tm2",          "TIM2 textures -> PNG (needs Pillow)"),
    "strextract":   ("strextract",   "demux FMV .str streams from DATA.BIN (.m2v + .wav)"),
    "sbt2srt":      ("sbt2srt",      "FMV subtitles (.sbt) -> .srt"),
    "fmv2mp4":      ("fmv2mp4",      "extracted .m2v+.wav pairs -> playable .mp4 (needs ffmpeg)"),
}


def usage(file=sys.stdout):
    print("usage: ae3 <command> [args...]\n\ncommands:", file=file)
    for name, (_, help_) in COMMANDS.items():
        print(f"  {name:<13} {help_}", file=file)
    print("\n`ae3 <command> --help` for each command's options.", file=file)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if not argv or argv[0] in ("-h", "--help"):
        usage(file=sys.stdout if argv else sys.stderr)
        return 0 if argv else 2
    cmd, rest = argv[0], argv[1:]
    if cmd not in COMMANDS:
        print(f"ae3: unknown command '{cmd}'\n", file=sys.stderr)
        usage(file=sys.stderr)
        return 2
    mod = importlib.import_module(f"ae3tools.{COMMANDS[cmd][0]}")
    sys.argv = [f"ae3 {cmd}"] + rest
    return mod.main() or 0


if __name__ == "__main__":
    sys.exit(main())
