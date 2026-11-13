/*
 * Test installer/rompatch.c and installer/lzss.c on the host.
 *
 * Usage: rompatch-test STOCK OUT [EXPECTED] [CFRG CFRG_OUT]
 *   STOCK     a stock Mac OS ROM data fork
 *   OUT       where to write the patched data fork
 *   EXPECTED  a patched data fork to compare OUT with
 *   CFRG      the stock file's 'cfrg' resource; CFRG_OUT gets it adjusted
 *
 * Checks that lzss_compress gives back Apple's compressed image, patches
 * the file, and opens the result to check that it reads as patched.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lzss.h"
#include "rompatch.h"

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(*len);
    if (fread(p, 1, *len, f) != *len) {
        perror(path);
        exit(1);
    }
    fclose(f);
    return p;
}

static void spit(const char *path, const unsigned char *p, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, len, f) != len) {
        perror(path);
        exit(1);
    }
    fclose(f);
}

static unsigned long be32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
           (unsigned long)p[2] << 8 | p[3];
}

int main(int argc, char **argv)
{
    size_t len, out_len, exp_len;
    unsigned char *data, *out, *expected;
    Rom rom, again;
    long delta;
    int err;

    if (argc < 3) {
        fprintf(stderr, "usage: %s STOCK OUT [EXPECTED] [CFRG CFRG_OUT]\n",
                argv[0]);
        return 2;
    }
    data = slurp(argv[1], &len);
    err = rom_open(&rom, data, len);
    if (err) {
        printf("rom_open: %s\n", rom_error_text(err));
        return 1;
    }
    printf("kernel %s at image offset 0x%lX, ConfigInfo at 0x%lX\n",
           rom.kernel == kKernelStock ? "stock" :
           rom.kernel == kKernelPatched ? "patched" : "unknown",
           rom.kernel_offset, rom.config_offset);

    {
        const unsigned char *child = data + rom.parcels + rom.rom_child;
        const unsigned char *packed = data + rom.parcels + be32(child + 24);
        size_t plen = be32(child + 20), n;
        unsigned char *buf = malloc(LZSS_BOUND(0x400000));
        void *work = malloc(lzss_work_size());
        n = lzss_compress(rom.image, 0x400000, buf, work, NULL);
        printf("lzss_compress of the stock image: %zu bytes, %s Apple's\n",
               n, n == plen && memcmp(buf, packed, n) == 0 ? "same as"
                                                            : "DIFFERENT from");
        free(buf);
        free(work);
    }

    err = rom_patch(&rom, &out, &out_len, &delta, NULL);
    if (err) {
        printf("rom_patch: %s\n", rom_error_text(err));
        return 1;
    }
    spit(argv[2], out, out_len);
    printf("patched: %zu bytes (was %zu), System Enabler moved %ld\n",
           out_len, len, delta);

    err = rom_open(&again, out, out_len);
    printf("reopened: %s, kernel %s\n", rom_error_text(err),
           again.kernel == kKernelPatched ? "patched" : "NOT PATCHED");
    rom_close(&again);

    if (argc > 3) {
        size_t i;
        expected = slurp(argv[3], &exp_len);
        for (i = 0; i < out_len && i < exp_len && out[i] == expected[i]; i++)
            ;
        if (i == out_len && out_len == exp_len)
            printf("identical to %s\n", argv[3]);
        else
            printf("DIFFERS from %s at 0x%zX (lengths %zu, %zu)\n", argv[3],
                   i, out_len, exp_len);
    }
    if (argc > 5) {
        size_t clen;
        unsigned char *cfrg = slurp(argv[4], &clen);
        cfrg_adjust(cfrg, clen, delta);
        spit(argv[5], cfrg, clen);
    }
    rom_close(&rom);
    return 0;
}
