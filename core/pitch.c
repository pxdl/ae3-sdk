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
    p = (p * 0x1000) >> 12;                       /* EE 4.12 multiplier: unity ALWAYS --
                                                     even under LFO. M9 pinned the real
                                                     hook: vibrato arrives as note/fine
                                                     offsets (ae3__lfo_tick below) */
    return (uint16_t)p;                           /* lhu truncation, as the IRX stores */
}

/* ---- M9: the driver LFO --------------------------------------------------------
 *
 * Ground truth: the EE-side voice flush FUN_003ffc70 (60 Hz dispatcher slot 3) and
 * the rate/depth setters FUN_003fedf8/FUN_003feea8, raw-disassembly-verified;
 * full spec in the private repo's decomp/functions_bgm/lfo/NOTES.md, condensed in
 * docs/formats/BGM.md "LFO". There is NO LFO code in the IOP module: the flush
 * replaces the bend-wheel MSB with the waveform sample, converts bend to note+fine
 * on the EE (BGM pitch mode), and sends the pitch command with bendMSB=0x40,
 * range=1, multiplier unity. So vibrato inherits the pitch table's 16-steps-per-
 * semitone quantization and the 60 Hz tick grid -- both audible hardware facts.
 *
 * The waveform is a 60-byte unsigned table, one entry per 4 phase units. Banks
 * with an LFO chunk supply their own (s_20_park: a sine); every other voice uses
 * the driver's default triangle. The default is COMPUTED here -- it is the
 * canonical triangle (peak 0xF0 at [14], trough 0x00 at [44], step 8), an
 * arithmetic fact like the ET pitch fallback, and unlike that fallback it is
 * byte-identical to the game ELF's table at 0x0069e1e0 (the private corpus gate
 * diffs the two), so no ELF donor is needed. */

void ae3__lfo_triangle(uint8_t tbl[60])
{
    for (int i = 0; i < 60; i++)
        tbl[i] = (uint8_t)(i <= 14 ? 128 + 8 * i
                         : i <= 44 ? 352 - 8 * i
                                   : 8 * i - 352);
}

/* FUN_003fedf8: rate = 240/(60 - p*58/127), integer ops; p==0 disarms. Always
 * zeroes the phase AND the 6-of-7 countdown (so the first tick after a rate set
 * is a reload tick that does not advance). Armed only if rate AND depth. */
void ae3__lfo_set_rate(ae3_voice *v, int p)
{
    uint32_t r = 0;
    if (p != 0)
        r = 240u / (uint32_t)(60 - (p * 58) / 127);
    v->lfo_rate = r;
    v->lfo_count = 0;
    v->lfo_phase = 0;
    v->lfo_on = r != 0 && v->lfo_depth != 0;
}

/* FUN_003feea8: depth, DOUBLED for BGM voices (pitch mode cmd[1]==1 -- every BGM
 * voice; the doubling makes BGM depth always even). Armed only if depth AND rate. */
void ae3__lfo_set_depth(ae3_voice *v, int p)
{
    v->lfo_depth = p != 0 ? (uint32_t)p << 1 : 0;
    v->lfo_on = v->lfo_depth != 0 && v->lfo_rate != 0;
}

/* One 60 Hz tick of the flush's 0x400 block for an armed voice: advance the phase
 * (6 of every 7 ticks), sample the waveform, and recompute the pitch register the
 * way the flush's pitch send does. Runs every tick while armed -- when the LFO
 * disarms nothing re-sends, so the voice keeps its last modulated pitch (the
 * driver's freeze quirk; authored CC1 ramps end near zero so it is inaudible). */
void ae3__lfo_tick(ae3_synth *s, ae3_voice *v)
{
    if (v->lfo_count == 0) {
        v->lfo_count = 6;                     /* reload tick: no phase advance */
    } else {
        v->lfo_count--;
        v->lfo_phase += v->lfo_rate;
    }
    uint32_t ph = v->lfo_phase;
    if (ph >= 240)
        ph = v->lfo_phase = v->lfo_rate >> 1; /* wrap lands at rate/2, not 0 */

    const uint8_t *tbl = v->lfo_tbl ? v->lfo_tbl : s->lfo_tri;
    /* NULL = chunk-present bank + out-of-range index, where the real driver reads
     * a garbage pointer; no armed voice in the corpus has one (lfo/NOTES.md). */

    int d = (int)v->lfo_depth;
    int out = (int)(((uint32_t)tbl[ph >> 2] * (uint32_t)d) / 255u)
            - ((d + 1) >> 1) + 0x40;          /* replaces the bend-wheel MSB */

    /* the flush's BGM bend->note/fine conversion (raw asm 0x400330 / 0x4003f0):
     * m>>6 semitones, the >>2 remainder in 1/16-semitone steps */
    int range = v->tone->bend, semis, fine;
    if (out >= 0x40) {
        int m = (out - 0x40) * range;
        semis = m >> 6;
        fine  = (m >> 2) - (semis << 4);
    } else {
        int m = (0x40 - out) * range;
        int mag = m >> 6;
        semis = -mag;
        fine  = (mag << 4) - (m >> 2);
    }
    /* sent as ev_set_pitch(root, note+semis, fine+that, 0x40, range=1, mult unity):
     * bend term zero, so the wheel is ignored while armed */
    v->pitch = ae3__pitch_reg(s, v->key + semis, v->tone->root,
                              v->tone->tune + fine, 64, 1);
    if (v->pitch > 0x3FFF)
        s->st.pitch_step_clamped++;
}
