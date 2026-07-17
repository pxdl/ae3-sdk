#!/usr/bin/env python3
"""TIM2 (.tm2) -> PNG.

The picture header layout below is not taken from the public TIM2 spec on trust -- it is
read out of SCUS_975.01. `TexDb::createTM2` (0x0028ab1c, named by the assert string at
0x006ee6c0) BUILDS a TIM2 in RAM field by field, which pins every offset:

    sb  t7(4),  4(s2)     +0x04  version = 4
    sb  zero,   5(s2)     +0x05  format  = 0
    sh  t4(1),  6(s2)     +0x06  picture count = 1
    sw  t6,    16(s2)     +0x10  total_size
    sw  zero,  20(s2)     +0x14  clut_size = 0 (it builds a non-indexed picture)
    sw  t5,    24(s2)     +0x18  image_size
    sh  t7(48),28(s2)     +0x1c  header_size = 48
    sh  zero,  30(s2)     +0x1e  clut_colors
    sb  zero,  32(s2)     +0x20  pict_format
    sb  t4(1), 33(s2)     +0x21  mipmap_count
    sb  zero,  34(s2)     +0x22  clut_type
    sb  t7(3), 35(s2)     +0x23  image_type = 3 (RGBA32)
    sh  s3,    36(s2)     +0x24  width
    sh  s4,    38(s2)     +0x26  height
    sd  s1,    40(s2)     +0x28  GsTex0   <- assembled from TBW=w/64 (sra 6, dsll 14),
    sd  t7(96),48(s2)     +0x30  GsTex1      TW/TH = log2ceil(w/h) via FUN_0028aaf4
    sw  zero,  56(s2)     +0x38  GsRegs      (dsll 26 / dsll 30), TCC bit 34.
    sw  zero,  60(s2)     +0x3c  GsTexClut

No code compares the 'TIM2' magic (scanned .text for lui 0x324d/0x4954: zero hits) -- the
loader trusts the format.

ALPHA IS 0..128, NOT 0..255. Proven on the corpus, not assumed: across all 45072 RGBA32
CLUT entries in the 256 files, ZERO bytes exceed 0x80 and 38978 (86%) are exactly 0x80.
A 0..255 convention would be dominated by 0xff. So alpha is rescaled *255/128 here;
skipping that makes every texture half-transparent.
"""
import argparse
import os
import struct
import sys

from PIL import Image

from .i3d import dest_paths

MAGIC = b"TIM2"
BPP = {1: 16, 2: 24, 3: 32, 4: 4, 5: 8}
IDTEX4, IDTEX8, RGBA16, RGB24, RGBA32 = 4, 5, 1, 2, 3


def _a(v: int) -> int:
    """PS2 alpha 0..128 -> 0..255. 0x80 is fully opaque."""
    return 255 if v >= 0x80 else (v * 255) // 128


def _csm1_index(i: int) -> int:
    """CSM1 (GS-native) 256-entry CLUT order -> linear.

    The GS reads a 256-colour CLUT in 8x2 blocks, which swaps entry groups 8..15 and
    16..23 within every run of 32. Undo it by exchanging bits 3 and 4 of the index.
    Only 256-entry CLUTs are affected; a 16-entry CLUT has no bit 4 to swap.
    """
    return (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


class Picture:
    __slots__ = ("w", "h", "itype", "ctype", "ncol", "nmip", "pal", "idx", "rgba")

    def __repr__(self):
        return f"<Pic {self.w}x{self.h} itype={self.itype} ncol={self.ncol}>"


def _clut(b: bytes, off: int, ncol: int, ctype: int, swizzle: bool) -> list:
    """Decode a CLUT to a list of (r,g,b,a). ctype bit 7 set = CSM2 = already linear."""
    csm2 = bool(ctype & 0x80)
    kind = ctype & 0x3F
    pal = []
    for i in range(ncol):
        if kind == RGBA32:
            r, g, b_, a = b[off + i * 4: off + i * 4 + 4]
            pal.append((r, g, b_, _a(a)))
        elif kind == RGB24:
            r, g, b_ = b[off + i * 3: off + i * 3 + 3]
            pal.append((r, g, b_, 255))
        elif kind == RGBA16:
            v, = struct.unpack_from("<H", b, off + i * 2)
            pal.append(((v & 0x1F) << 3, ((v >> 5) & 0x1F) << 3,
                        ((v >> 10) & 0x1F) << 3, 255 if v & 0x8000 else 0))
        else:
            raise ValueError(f"unsupported clut_type 0x{ctype:02x}")
    # Only a 256-entry CSM1 palette is stored in GS order.
    if swizzle and not csm2 and ncol == 256:
        pal = [pal[_csm1_index(i)] for i in range(256)]
    return pal


def _indices(b: bytes, off: int, w: int, h: int, itype: int) -> list:
    if itype == IDTEX8:
        return list(b[off: off + w * h])
    if itype == IDTEX4:
        out = []
        for i in range(w * h // 2):
            v = b[off + i]
            out.append(v & 0x0F)          # low nibble is the left pixel
            out.append(v >> 4)
        return out
    raise ValueError(f"_indices called on non-indexed type {itype}")


def read(blob: bytes, path: str = "?", swizzle: bool = True) -> list:
    if blob[:4] != MAGIC:
        raise ValueError(f"{path}: not a TIM2 (magic {blob[:4]!r})")
    ver, fmt, npic = struct.unpack_from("<BBH", blob, 4)
    if ver != 4:
        raise ValueError(f"{path}: TIM2 version {ver} != 4")
    # format 0 -> pictures start right after the 16-byte header; 1 -> 128-byte aligned.
    off = 0x10 if fmt == 0 else 0x80
    pics = []
    for p in range(npic):
        (tot, clut_sz, img_sz, hdr_sz, ncol, pfmt, nmip, ctype,
         itype, w, h) = struct.unpack_from("<3I2H4B2H", blob, off)
        pic = Picture()
        pic.w, pic.h, pic.itype, pic.ctype, pic.ncol, pic.nmip = w, h, itype, ctype, ncol, nmip
        body = off + hdr_sz                    # level 0 image
        if itype in (IDTEX4, IDTEX8):
            if clut_sz == 0:
                raise ValueError(f"{path}: indexed picture {p} has no CLUT")
            pic.pal = _clut(blob, off + hdr_sz + img_sz, ncol, ctype, swizzle)
            pic.idx = _indices(blob, body, w, h, itype)
            if max(pic.idx) >= ncol:
                raise ValueError(f"{path}: pic {p} index {max(pic.idx)} >= ncol {ncol}")
            pic.rgba = [pic.pal[i] for i in pic.idx]
        elif itype == RGBA32:
            pic.rgba = [(blob[body + i * 4], blob[body + i * 4 + 1],
                         blob[body + i * 4 + 2], _a(blob[body + i * 4 + 3]))
                        for i in range(w * h)]
        elif itype == RGB24:
            pic.rgba = [(blob[body + i * 3], blob[body + i * 3 + 1],
                         blob[body + i * 3 + 2], 255) for i in range(w * h)]
        elif itype == RGBA16:
            pic.rgba = []
            for i in range(w * h):
                v, = struct.unpack_from("<H", blob, body + i * 2)
                pic.rgba.append(((v & 0x1F) << 3, ((v >> 5) & 0x1F) << 3,
                                 ((v >> 10) & 0x1F) << 3, 255 if v & 0x8000 else 0))
        else:
            raise ValueError(f"{path}: unsupported image_type {itype}")
        pics.append(pic)
        off += tot
    return pics


def to_image(pic: Picture) -> Image.Image:
    img = Image.new("RGBA", (pic.w, pic.h))
    img.putdata(pic.rgba)
    return img


def main() -> int:
    ap = argparse.ArgumentParser(description="Convert .tm2 (TIM2) textures to PNG")
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--out", default="extracted/png")
    ap.add_argument("--no-swizzle", action="store_true",
                    help="skip the CSM1 256-entry CLUT unswizzle (for A/B testing)")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    dests = dest_paths(args.files, args.out, ".png")
    ok = fail = 0
    for src, dst in zip(args.files, dests):
        try:
            pics = read(open(src, "rb").read(), src, swizzle=not args.no_swizzle)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            for i, pic in enumerate(pics):
                # Multi-picture files get a suffix; the common 1-picture case does not.
                d = dst if len(pics) == 1 else dst[:-4] + f"_{i}.png"
                to_image(pic).save(d)
            ok += 1
            if not args.quiet:
                print(f"OK   {src} -> {dst}  {pics[0].w}x{pics[0].h} "
                      f"itype={pics[0].itype}" + (f" x{len(pics)}" if len(pics) > 1 else ""))
        except Exception as e:
            fail += 1
            print(f"FAIL {src}: {e}")
    print(f"\n{ok}/{ok+fail} converted")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
