#!/usr/bin/env python3
"""Parse the DATA.BIN (VFI) container.

Layout established TWICE, independently:

  1. Read out of SCUS_975.01 — FUN_001008e0 (magic check), FUN_0010070c (lookup by name),
     FUN_00100958 (entry accessor). See docs/formats/DATA_BIN.md.
  2. Cross-checked against aluigi's `ape_escape_vfi.bms` QuickBMS script, which is an
     independent reimplementation by someone who never saw our work.

The two agree on the structure. The BMS supplied field *semantics* our first pass had
mislabelled -- see the header table below. Where they differ we went with the BMS and
then verified empirically (e.g. .sz = raw deflate: 400/400 exact-size decompressions).

Header:
  +0x00  u32  magic 'VFI\\0' == 0x00494656
  +0x04  u32  version (1)
  +0x08  u32  DATA_OFF, in SECTORS -> data region starts at 63*0x800 = 0x1f800.
              (We first mislabelled this "count=63". It is not a count. 0x1f800 is
              exactly where an earlier brute-force magic scan found the first ELF.)
  +0x0c  u32  zero
  +0x10  u16  FILES   (3994)
  +0x12  u16  FOLDERS (266)
  +0x14  u32  INFO_OFF  -> file entry table, self-relative
  +0x18  u32  dummy
  +0x1c  u32  INFO_END

File entry (variable length, walked sequentially from INFO_OFF):
  +0x00  u16  ENTRY_SIZE   (record length; advance by this)
  +0x02  u16  PARENT_OFF   (0 = root, else offset into the folder region)
  +0x04  u32  OFFSET       in SECTORS -> byte offset = *0x800
  +0x08  u32  SIZE         in bytes
  +0x0c  str  NAME         (ENTRY_SIZE - 12 bytes)

Folder record (at FOLDERS_OFF + PARENT_OFF; FOLDERS_OFF = end of the file entry table):
  +0x00  u16  ENTRY_SIZE
  +0x02  u16  NEXT_OFF
  +0x04  u16  PARENT_OFF   (walk up until 0)
  +0x06  u16  dummy
  +0x08  str  PATH         (ENTRY_SIZE - 8 bytes)

Read-only. Never writes to DATA.BIN.
"""
import argparse
import struct
import sys

MAGIC = 0x00494656
SECTOR = 0x800


def _cstr(b: bytes) -> str:
    z = b.find(b"\0")
    return b[: z if z >= 0 else len(b)].decode("ascii", "replace")


class Vfi:
    def __init__(self, f, base: int = 0):
        self.f = f
        self.base = base
        hdr = self._read(0, 0x20)
        self.magic, self.version, self.data_off, self.zero = struct.unpack_from("<4I", hdr, 0)
        if self.magic != MAGIC:
            raise ValueError(f"bad magic 0x{self.magic:08x} at base 0x{base:x}")
        self.files, self.folders = struct.unpack_from("<2H", hdr, 0x10)
        self.info_off, self.dummy, self.info_end = struct.unpack_from("<3I", hdr, 0x14)
        self._entries = None
        self._folders_off = None
        self._folder_cache: dict[int, str] = {}

    def _read(self, off: int, n: int) -> bytes:
        self.f.seek(self.base + off)
        return self.f.read(n)

    # --- the file entry table -------------------------------------------------
    def entries(self):
        """Walk FILES variable-length records from INFO_OFF. Returns list of dicts."""
        if self._entries is not None:
            return self._entries
        out = []
        off = self.info_off
        for _ in range(self.files):
            head = self._read(off, 12)
            if len(head) < 12:
                break
            entry_size, parent_off = struct.unpack_from("<2H", head, 0)
            offset, size = struct.unpack_from("<2I", head, 4)
            if entry_size < 12:
                break
            name = _cstr(self._read(off + 12, entry_size - 12))
            out.append(dict(entry_off=off, entry_size=entry_size, parent_off=parent_off,
                            sector=offset, size=size, name=name))
            off += entry_size
        self._entries = out
        self._folders_off = off  # savepos after the loop -- the folder region
        return out

    @property
    def folders_off(self) -> int:
        if self._folders_off is None:
            self.entries()
        return self._folders_off

    def _folder(self, parent_off: int):
        """Read one folder record -> (path, its own parent_off)."""
        if parent_off in self._folder_cache:
            return self._folder_cache[parent_off]
        off = self.folders_off + parent_off
        head = self._read(off, 8)
        if len(head) < 8:
            return ("", 0)
        entry_size, _next_off, up, _dummy = struct.unpack_from("<4H", head, 0)
        if entry_size < 8:
            return ("", 0)
        path = _cstr(self._read(off + 8, entry_size - 8))
        res = (path, up)
        self._folder_cache[parent_off] = res
        return res

    def full_path(self, e) -> str:
        """Prepend every folder up the parent chain, exactly as the BMS does."""
        name = e["name"]
        p = e["parent_off"]
        guard = 0
        while p != 0 and guard < 64:
            path, up = self._folder(p)
            if not path:
                break
            name = f"{path}/{name}"
            p = up
            guard += 1
        return name

    def byte_offset(self, e) -> int:
        return e["sector"] * SECTOR

    def header_report(self) -> str:
        return "\n".join([
            f"  magic      'VFI\\0' (0x{self.magic:08x})   version {self.version}",
            f"  DATA_OFF   {self.data_off} sectors -> byte 0x{self.data_off*SECTOR:x}   (+0x08)",
            f"  FILES      {self.files}                        (+0x10, u16)",
            f"  FOLDERS    {self.folders}                       (+0x12, u16)",
            f"  INFO_OFF   0x{self.info_off:x}                     (+0x14)",
            f"  INFO_END   0x{self.info_end:x}                    (+0x1c)",
            f"  FOLDERS_OFF 0x{self.folders_off:x}  (= end of the file entry table)",
        ])


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Inspect a DATA.BIN VFI archive: header summary and entry table.")
    ap.add_argument("path", help="path to DATA.BIN (on the game disc)")
    ap.add_argument("-n", "--num", type=int, default=25,
                    help="number of entries to list (default 25)")
    args = ap.parse_args()

    with open(args.path, "rb") as f:
        v = Vfi(f)
        total = f.seek(0, 2)
        print(f"=== {args.path} ===")
        ents = v.entries()
        print(v.header_report())

        print(f"\n  CHECK entries walked: {len(ents)} / FILES {v.files} -> "
              f"{'MATCH' if len(ents) == v.files else 'MISMATCH'}")
        oor = sum(1 for e in ents if v.byte_offset(e) + e["size"] > total)
        print(f"  CHECK sector*0x800 + size <= EOF: {len(ents)-oor}/{len(ents)} in range")
        named = sum(1 for e in ents if e["name"] and all(32 <= ord(c) < 127 for c in e["name"]))
        print(f"  CHECK printable names: {named}/{len(ents)}")

        print(f"\n=== first {args.num} entries (full paths) ===")
        print(f"{'sector':>9} {'size':>10}  path")
        for e in ents[:args.num]:
            print(f"{e['sector']:>9} {e['size']:>10}  {v.full_path(e)}")

        dirs = {}
        for e in ents:
            p = v.full_path(e)
            d = p.rsplit("/", 1)[0] if "/" in p else "(root)"
            dirs[d] = dirs.get(d, 0) + 1
        print(f"\n=== {len(dirs)} distinct directories; top 20 by file count ===")
        for d, c in sorted(dirs.items(), key=lambda kv: -kv[1])[:20]:
            print(f"  {c:>5}  {d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
