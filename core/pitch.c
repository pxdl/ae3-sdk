/* pitch.c -- the SPU2 pitch register, exactly as the driver's IOP module computes it.
 *
 * Ground truth: ev_set_pitch in irx/3.0/sg2iopm1.irx (not stripped; disassembled and
 * re-verified). The math below mirrors that code:
 *   d = |note - root|; q = d / 12 (truncating); R = d % 12
 *   idx = (note >= root ? R*16 : (12-R)*16) + 208 + fine + ((bendMSB - 64) * range >> 2)
 *   p   = note >= root ? pitch_tbl[idx] << q : pitch_tbl[idx] >> (q + 1)
 *   p   = (p * 441) / 480                      (floor; reciprocal-multiply in the IRX)
 *   p   = (p * mult) >> 12                     (4.12 multiplier from the EE command --
 *                                               unity 0x1000 for BGM; the LFO's hook)
 *   register = p & 0xFFFF                      (the IRX stores with lhu)
 * Notes pinned from the disassembly that the docs did not have: the bend term uses an
 * ARITHMETIC shift (floors for down-bends), `fine` is sign-extended (lb), and there is
 * NO index clamp and NO 0x3FFF clamp in the driver -- the hardware clamps the counter
 * step (voice.c). We clamp idx defensively (the driver would read past the table; game
 * data never does -- counted in stats so a violation is visible).
 *
 * pitch_tbl itself is Sony's data: 608 u16 entries in sg2iopm1.irx's .rodata. It is
 * loaded from the user's extracted IRX at runtime, NEVER embedded (project stance, same
 * as bgm.load_reverb with libsd). Without the IRX we fall back to nearest equal
 * temperament, which matches the real table within +-1 unit (+-0.42 cents) everywhere
 * except the table's two corrupt entries (idx 64: 0x0938 for 0x0983, idx 172: 0x0d0c
 * for 0x0e0d -- Sony's typos, reachable only under bend; BGM.md section 4a). */
#include <math.h>
#include <string.h>

#include "internal.h"

/* Nearest-ET fallback: tbl[i] = round(0x1000 * 2^((i-208)/192)). */
void ae3_pitch_tbl_et(uint16_t *tbl)
{
    for (int i = 0; i < AE3_PITCH_TBL_N; i++)
        tbl[i] = (uint16_t)llround(4096.0 * pow(2.0, (i - 208) / 192.0));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Find `pitch_tbl` in the IRX (ELF32 LE, relocatable, with .symtab -- sg2iopm1.irx is
 * not stripped) and copy its 608 entries out. */
int ae3__load_pitch_irx(ae3_synth *s, const uint8_t *d, size_t len)
{
    if (len < 0x34 || memcmp(d, "\x7f""ELF", 4) != 0)
        return ae3__fail(s, "not an ELF (irx)");
    uint32_t shoff = rd32(d + 0x20);
    uint16_t shentsize = rd16(d + 0x2e), shnum = rd16(d + 0x30);
    if ((size_t)shoff + (size_t)shnum * shentsize > len)
        return ae3__fail(s, "irx: section headers out of range");

    /* locate .symtab (type 2) and its string table (sh_link) */
    const uint8_t *symtab = NULL, *strtab = NULL;
    uint32_t symsize = 0, strsize = 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = d + shoff + (size_t)i * shentsize;
        if (rd32(sh + 4) != 2)                    /* SHT_SYMTAB */
            continue;
        uint32_t off = rd32(sh + 16), size = rd32(sh + 20), link = rd32(sh + 24);
        const uint8_t *lh = d + shoff + (size_t)link * shentsize;
        symtab = d + off; symsize = size;
        strtab = d + rd32(lh + 16); strsize = rd32(lh + 20);
        if ((size_t)(off + size) > len || (size_t)(rd32(lh + 16) + strsize) > len)
            return ae3__fail(s, "irx: symtab out of range");
        break;
    }
    if (!symtab)
        return ae3__fail(s, "irx: no symbol table (stripped?)");

    for (uint32_t o = 0; o + 16 <= symsize; o += 16) {
        uint32_t nameoff = rd32(symtab + o), value = rd32(symtab + o + 4);
        uint16_t shndx = rd16(symtab + o + 14);
        if (nameoff >= strsize ||
            strncmp((const char *)strtab + nameoff, "pitch_tbl", 10) != 0)
            continue;
        /* value is a section-relative-turned-virtual address; map through the
         * symbol's section (sh_addr -> sh_offset) */
        if (shndx == 0 || shndx >= shnum)
            return ae3__fail(s, "irx: pitch_tbl has bad section %u", shndx);
        const uint8_t *sh = d + shoff + (size_t)shndx * shentsize;
        uint32_t addr = rd32(sh + 12), off = rd32(sh + 16), size = rd32(sh + 20);
        if (value < addr || value - addr + 2 * AE3_PITCH_TBL_N > size ||
            (size_t)(off + (value - addr) + 2 * AE3_PITCH_TBL_N) > len)
            return ae3__fail(s, "irx: pitch_tbl out of section range");
        const uint8_t *p = d + off + (value - addr);
        for (int i = 0; i < AE3_PITCH_TBL_N; i++)
            s->pitch_tbl[i] = rd16(p + 2 * i);
        /* sanity: the two proven landmarks (BGM.md section 4a) */
        if (s->pitch_tbl[16] != 0x800 || s->pitch_tbl[208] != 0x1000)
            return ae3__fail(s, "irx: pitch_tbl landmarks wrong (%#x/%#x)",
                             s->pitch_tbl[16], s->pitch_tbl[208]);
        s->pitch_verbatim = true;
        return 0;
    }
    return ae3__fail(s, "irx: no pitch_tbl symbol");
}

uint16_t ae3__pitch_reg(ae3_synth *s, int note, int root, int fine, int bend_msb,
                        int range)
{
    int base, q;
    if (note >= root) {
        int diff = note - root;
        q = diff / 12;
        base = (diff % 12) * 16;
    } else {
        int diff = root - note;
        q = diff / 12;
        base = (12 - diff % 12) * 16;
    }
    int idx = base + 208 + fine + (((bend_msb - 64) * range) >> 2);
    if (idx < 0 || idx >= AE3_PITCH_TBL_N) {      /* driver reads OOB here; count it */
        s->st.pitch_idx_clamped++;
        idx = idx < 0 ? 0 : AE3_PITCH_TBL_N - 1;
    }
    uint32_t p = s->pitch_tbl[idx];
    p = note >= root ? p << q : p >> (q + 1);
    p = p * 441 / 480;
    p = (p * 0x1000) >> 12;                       /* EE 4.12 multiplier: unity for BGM;
                                                     LFO vibrato plugs in here (M9) */
    return (uint16_t)p;                           /* lhu truncation, as the IRX stores */
}
