/* exstdump -- headless EXST (.x) stream harness: header info, or decode to WAV.
 *
 * usage: exstdump [--decode] [--trim-pad] [--pcm] [-o OUT] FILE.x...
 *   default      one info line per file (the `ae3 exst` inspection, natively)
 *   --decode     decode the whole payload to WAV at the header's channels/rate.
 *                Decodes the ACTUAL whole-sector payload, not the header length
 *                field -- 16 shipped files overstate it (docs/formats/EXST.md
 *                §4); the discrepancy is warned on stderr.
 *   --trim-pad   drop the trailing silent-pad run (min across channels), the
 *                same trim `ae3 exst --trim-pad` applies
 *   --pcm        raw interleaved s16 instead of WAV framing (corpus gate food)
 *   -o OUT       output path (single input only; default FILE.wav / FILE.pcm)
 *
 * WAV framing is the canonical 44-byte header, byte-identical to what
 * `ae3 exst --decode` writes, so the private corpus gate can diff whole files.
 * Public API only (ae3synth.h) -- the reference consumer of ae3_exst_*. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ae3synth.h"

static void wav_header(FILE *f, uint32_t data_bytes, uint16_t ch, uint32_t rate)
{
    uint8_t h[44];
    uint32_t byte_rate = rate * ch * 2;
    uint16_t block = ch * 2;
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + data_bytes;
    memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;             memcpy(h + 16, &v, 4);   /* fmt chunk length */
    uint16_t u = 1;     memcpy(h + 20, &u, 2);   /* PCM */
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block, 2);
    u = 16;             memcpy(h + 34, &u, 2);   /* bits */
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fwrite(h, 1, 44, f);
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: cannot open\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "%s: read failed\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)sz;
    return buf;
}

static const char *base_name(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static int info(const char *path)
{
    size_t len;
    uint8_t *data = read_file(path, &len);
    if (!data)
        return 1;
    ae3_exst_header h;
    if (ae3_exst_parse(data, len, &h) != 0) {
        fprintf(stderr, "%s: not an EXST file\n", path);
        free(data);
        return 1;
    }
    uint32_t actual = (uint32_t)((len - AE3_EXST_HDR) / AE3_EXST_SECTOR);
    uint32_t spc = actual * (AE3_EXST_SECTOR / h.channels / 16) * 28;
    uint32_t pad = ae3_exst_trailing_pad(data + AE3_EXST_HDR, len - AE3_EXST_HDR,
                                         h.channels);
    printf("%s: %uch %u Hz, %u sectors, %u samples/ch (%.2fs), pad %u frames",
           base_name(path), h.channels, h.rate, actual, spc,
           h.rate ? (double)spc / h.rate : 0.0, pad);
    if (h.loop)
        printf(", LOOP from sector %u", h.loop_start);
    if (h.length != actual)
        printf("  [WARN header says %u sectors]", h.length);
    printf("\n");
    free(data);
    return 0;
}

static int decode(const char *path, const char *out_path, int trim, int raw_pcm)
{
    size_t len;
    uint8_t *data = read_file(path, &len);
    if (!data)
        return 1;
    ae3_exst_header h;
    if (ae3_exst_parse(data, len, &h) != 0) {
        fprintf(stderr, "%s: not an EXST file\n", path);
        free(data);
        return 1;
    }
    const uint8_t *payload = data + AE3_EXST_HDR;
    size_t pay_len = len - AE3_EXST_HDR;
    uint32_t sectors = (uint32_t)(pay_len / AE3_EXST_SECTOR);
    if (h.length != sectors)
        fprintf(stderr, "%s: header length %u sectors != actual payload %u; "
                        "decoding actual\n", base_name(path), h.length, sectors);

    ae3_exst st;
    if (ae3_exst_reset(&st, h.channels) != 0) {
        fprintf(stderr, "%s: %u channels unsupported for decode\n",
                base_name(path), h.channels);
        free(data);
        return 1;
    }
    uint32_t spc = sectors * (AE3_EXST_SECTOR / h.channels / 16) * 28;
    int16_t *pcm = malloc((size_t)sectors * 3584 * sizeof(int16_t) + 2);
    if (!pcm) {
        fprintf(stderr, "out of memory\n");
        free(data);
        return 1;
    }
    for (uint32_t s = 0; s < sectors; s++)
        ae3_exst_decode(&st, payload + (size_t)s * AE3_EXST_SECTOR,
                        pcm + (size_t)s * 3584);
    if (trim)
        spc -= ae3_exst_trailing_pad(payload, pay_len, h.channels) * 28;

    char def_path[4096];
    if (!out_path) {
        snprintf(def_path, sizeof def_path, "%.*s.%s",
                 (int)(strlen(path) - (strlen(path) > 2 &&
                       !strcmp(path + strlen(path) - 2, ".x") ? 2 : 0)),
                 path, raw_pcm ? "pcm" : "wav");
        out_path = def_path;
    }
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "%s: cannot write\n", out_path);
        free(pcm);
        free(data);
        return 1;
    }
    uint32_t data_bytes = spc * h.channels * 2;
    if (!raw_pcm)
        wav_header(f, data_bytes, h.channels, h.rate);
    fwrite(pcm, 1, data_bytes, f);
    fclose(f);
    printf("%s -> %s (%uch %u Hz, %u samples/ch)\n",
           base_name(path), out_path, h.channels, h.rate, spc);
    free(pcm);
    free(data);
    return 0;
}

int main(int argc, char **argv)
{
    int do_decode = 0, trim = 0, raw_pcm = 0;
    const char *out = NULL;
    const char *files[4096];
    int nfiles = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--decode"))
            do_decode = 1;
        else if (!strcmp(argv[i], "--trim-pad"))
            trim = 1;
        else if (!strcmp(argv[i], "--pcm"))
            do_decode = raw_pcm = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            out = argv[++i];
        else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        } else if (nfiles < (int)(sizeof files / sizeof *files))
            files[nfiles++] = argv[i];
    }
    if (!nfiles) {
        fprintf(stderr, "usage: exstdump [--decode] [--trim-pad] [--pcm] "
                        "[-o OUT] FILE.x...\n");
        return 2;
    }
    if (out && nfiles > 1) {
        fprintf(stderr, "-o takes a single input file\n");
        return 2;
    }
    int rc = 0;
    for (int i = 0; i < nfiles; i++)
        rc |= do_decode ? decode(files[i], out, trim, raw_pcm)
                        : info(files[i]);
    return rc;
}
