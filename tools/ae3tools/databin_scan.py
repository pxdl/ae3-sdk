#!/usr/bin/env python3
"""Low-level scans over a raw DATA.BIN image (read-only).

Two scans, both over the whole file:
  1. count word-aligned MIPS `lw rX,0x7cf4(rY)` instruction encodings
     (word & 0xFC00FFFF == 0x8C007CF4) -- a code-presence probe over the
     executables embedded in DATA.BIN;
  2. list every sector-aligned (0x800) embedded ELF with its class, type,
     machine, entry point and flags, classifying EE-code images (entry >=
     0x00100000) vs IOP modules.
"""
import mmap
import struct
import sys


def scan(path):
    f = open(path, 'rb')
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    size = mm.size(); print(f'DATA.BIN {size/1e9:.2f} GB\n')

    # ---- 1. the decisive scan ----
    print('=== searching for `lw rX,0x7cf4(rY)` (the live-array load) ===')
    needle = b'\xf4\x7c'; pos = 0; cand = 0; hits = []
    while True:
        i = mm.find(needle, pos)
        if i < 0: break
        pos = i + 1
        if i % 4: continue                       # must be word-aligned within the file
        if i < 2: continue
        w = struct.unpack_from('<I', mm, i-2)[0] # low half at i => word starts at i-2
        cand += 1
        if (w & 0xFC00FFFF) == 0x8C007CF4:
            hits.append((i-2, w))
    print(f'  aligned 0x7cf4 half-words examined: {cand}')
    print(f'  actual `lw *,0x7cf4(*)` instructions: {len(hits)}')
    for off, w in hits[:10]:
        print(f'    file 0x{off:08x}  {w:08x}  lw r{(w>>16)&31},0x7cf4(r{(w>>21)&31})')
    if not hits: print('    ** NONE — no EE code in DATA.BIN loads the live param pointer **')

    # ---- 2. the embedded ELFs ----
    print('\n=== embedded ELFs (machine type) ===')
    MACH = {8:'MIPS'}
    pos = 0; n = 0
    while n < 40:
        i = mm.find(b'\x7fELF', pos)
        if i < 0: break
        pos = i + 4
        if i % 0x800: continue                  # sector-aligned only
        cls, dat = mm[i+4], mm[i+5]
        e_type, e_mach = struct.unpack_from('<HH', mm, i+16)
        entry, = struct.unpack_from('<I', mm, i+24)
        flags, = struct.unpack_from('<I', mm, i+36)
        # EE (R5900) images load at 0x0010xxxx+; IOP modules at 0x0000xxxx and are type ET_REL/IRX
        kind = 'EE?' if entry >= 0x00100000 else 'IOP?'
        print(f'  0x{i:08x} class={cls} type={e_type} machine={MACH.get(e_mach,e_mach)} '
              f'entry=0x{entry:08x} flags=0x{flags:08x}  -> {kind}')
        n += 1
    print(f'  total sector-aligned ELFs: {n}')
    mm.close()
    return 0


def main():
    if len(sys.argv) != 2 or sys.argv[1] in ("-h", "--help"):
        print("usage: ae3 databin-scan DATA_BIN\n\n" + __doc__.strip(),
              file=sys.stderr)
        return 0 if len(sys.argv) == 2 else 2
    return scan(sys.argv[1])


if __name__ == "__main__":
    sys.exit(main())
