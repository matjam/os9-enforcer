/*
 * Patch the NanoKernel inside a Mac OS ROM file's data fork (rompatch.h).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rompatch.h"
#include "lzss.h"
#include "kpatch.h"

enum {
    kImageSize = 0x400000,
    kSumsLen = 40,         /* 8 lane sums and a 64-bit sum */
    kKernelOffsetField = 0x4C,
    kChecksumText = 14,    /* "\r\\ h# XXXXXXXX" */
    kNodeSize = 88,        /* link, type, hdr_size, flags, n_children,
                              child_size, a[32], b[32] */
    kChildSize = 60,       /* type, flags, compress, unpacked, crc, packed,
                              ptr, name[32] */
    kMaxNodes = 1024,
    kMaxChildren = 4096,
    kPadByte = 0x99        /* between parcel children, as in Apple's file */
};

static unsigned long be32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
           (unsigned long)p[2] << 8 | p[3];
}

static void put_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

unsigned long crc32_of(const unsigned char *p, size_t n)
{
    static unsigned long table[256];
    unsigned long c = 0xFFFFFFFFUL;
    size_t i;
    if (!table[1]) {
        for (i = 0; i < 256; i++) {
            unsigned long v = (unsigned long)i;
            int k;
            for (k = 0; k < 8; k++)
                v = v & 1 ? 0xEDB88320UL ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
    }
    for (i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return (c ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

unsigned long adler32_of(const unsigned char *p, size_t n)
{
    unsigned long a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552; /* keeps b below 2^32 */
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16 | a) & 0xFFFFFFFFUL;
}

static void checksum_text(unsigned char *out, unsigned long sum)
{
    char s[kChecksumText + 1];
    sprintf(s, "\r\\ h# %08lX", sum);
    memcpy(out, s, kChecksumText);
}

static int checksum_ok(const unsigned char *data, size_t at)
{
    unsigned char want[kChecksumText];
    checksum_text(want, adler32_of(data, at));
    return memcmp(data + at, want, kChecksumText) == 0;
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

/* Finds "h# HEX constant NAME" in the script; returns 0 if absent. */
static int find_constant(const unsigned char *s, size_t n, const char *name,
                         unsigned long *value, size_t *at, size_t *len)
{
    size_t namelen = strlen(name), i;
    for (i = 0; i + 2 < n; i++) {
        size_t j, start, end;
        unsigned long v = 0;
        if (s[i] != 'h' || s[i + 1] != '#')
            continue;
        j = i + 2;
        while (j < n && is_space(s[j]))
            j++;
        start = j;
        while (j < n && hex_value(s[j]) >= 0)
            v = v << 4 | (unsigned long)hex_value(s[j++]);
        end = j;
        if (end == start || j >= n || !is_space(s[j]))
            continue;
        while (j < n && is_space(s[j]))
            j++;
        if (j + 8 > n || memcmp(s + j, "constant", 8) != 0)
            continue;
        j += 8;
        if (j >= n || !is_space(s[j]))
            continue;
        while (j < n && is_space(s[j]))
            j++;
        if (j + namelen <= n && memcmp(s + j, name, namelen) == 0 &&
            (j + namelen == n || is_space(s[j + namelen]))) {
            *value = v;
            *at = start;
            *len = end - start;
            return 1;
        }
    }
    return 0;
}

static int put_constant(unsigned char *s, size_t at, size_t len,
                        unsigned long value)
{
    char hex[16];
    size_t n;
    sprintf(hex, "%lX", value);
    n = strlen(hex);
    if (n > len)
        return 0;
    memset(s + at, '0', len - n);
    memcpy(s + at + len - n, hex, n);
    return 1;
}

static const void *find_bytes(const void *hay, size_t n, const char *needle)
{
    size_t k = strlen(needle), i;
    const unsigned char *h = hay;
    for (i = 0; i + k <= n; i++)
        if (memcmp(h + i, needle, k) == 0)
            return h + i;
    return NULL;
}

/* The image's checksum: the sum of each byte lane of 8, and the sum of
   the image as big-endian 64-bit words, leaving out the 40 bytes at
   skip where they are kept. */
static void image_sums(const unsigned char *img, size_t skip,
                       unsigned char out[kSumsLen])
{
    unsigned long lane[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    unsigned long long sum64 = 0;
    size_t i;
    int k;
    for (i = 0; i < kImageSize; i += 8)
        for (k = 0; k < 8; k++)
            lane[k] += img[i + k];
    for (i = skip; i < skip + kSumsLen; i++)
        lane[i & 7] -= img[i];
    for (k = 0; k < 8; k++) {
        put_be32(out + 4 * k, lane[k]);
        sum64 += (unsigned long long)lane[k] << (8 * (7 - k));
    }
    put_be32(out + 32, (unsigned long)(sum64 >> 32));
    put_be32(out + 36, (unsigned long)sum64);
}

static int find_config(Rom *rom)
{
    const unsigned char *img = rom->image;
    unsigned long lane[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    size_t i, at;
    int k;
    for (i = 0; i < kImageSize; i += 8)
        for (k = 0; k < 8; k++)
            lane[k] += img[i + k];
    for (at = 0; at + 0x1000 <= kImageSize; at += 0x100) {
        unsigned long l[8];
        unsigned long long sum64 = 0;
        int ok = 1;
        memcpy(l, lane, sizeof l);
        for (i = at; i < at + kSumsLen; i++)
            l[i & 7] -= img[i];
        for (k = 0; k < 8 && ok; k++)
            ok = be32(img + at + 4 * k) == (l[k] & 0xFFFFFFFFUL);
        if (!ok)
            continue;
        for (k = 0; k < 8; k++)
            sum64 += (unsigned long long)l[k] << (8 * (7 - k));
        if (be32(img + at + 32) == (unsigned long)(sum64 >> 32) &&
            be32(img + at + 36) == (unsigned long)(sum64 & 0xFFFFFFFFUL)) {
            rom->config_offset = (unsigned long)at;
            return 1;
        }
    }
    return 0;
}

static int all_zero(const unsigned char *p, size_t n)
{
    while (n--)
        if (*p++)
            return 0;
    return 1;
}

/* Every release of Enforcer's kernel patch checks the control block's
   magic with `lis r17,'EN'` and `ori r17,r17,'FK'` (enf_register in
   kernel/nanokernel.s). Apple's kernels have no such pair. */
static int has_mark(const unsigned char *k, size_t n)
{
    static const unsigned char mark[8] = {0x3E, 0x20, 0x45, 0x4E,
                                          0x62, 0x31, 0x46, 0x4B};
    for (size_t i = 0; i + sizeof mark <= n; i += 4)
        if (!memcmp(k + i, mark, sizeof mark))
            return 1;
    return 0;
}

int rom_open(Rom *rom, const unsigned char *data, size_t len)
{
    const unsigned char *end_tag, *p, *child;
    unsigned long v, elf_off, link;
    size_t at, n, script_len, i;
    int nodes = 0;

    memset(rom, 0, sizeof *rom);
    rom->data = data;
    rom->len = len;
    rom->kernel = kKernelUnknown;

    if (len < 64 || memcmp(data, "<CHRP-BOOT>", 11) != 0)
        return kRomNotBootInfo;
    end_tag = find_bytes(data, len < 0x10000 ? len : 0x10000, "</CHRP-BOOT>");
    if (!end_tag)
        return kRomNotBootInfo;
    script_len = (size_t)(end_tag - data);
    if (!find_bytes(data, script_len, "adler32"))
        return kRomBadScript; /* only checksummed files (Mac OS 8.6 on) */
    if (!find_constant(data, script_len, "elf-offset", &elf_off, &at, &n) ||
        !find_constant(data, script_len, "parcels-offset", &v, &at, &n))
        return kRomBadScript;
    rom->parcels = v;
    if (!find_constant(data, script_len, "parcels-size", &v,
                       &rom->span_parcels_size.at, &rom->span_parcels_size.len))
        return kRomBadScript;
    rom->parcels_size = v;
    if (!find_constant(data, script_len, "info-size", &v,
                       &rom->span_info_size.at, &rom->span_info_size.len))
        return kRomBadScript;
    rom->info = v;

    /* The parcels end the checksummed part; the System Enabler follows. */
    if (rom->parcels < script_len || rom->parcels <= elf_off ||
        rom->parcels + rom->parcels_size != rom->info ||
        rom->info + kChecksumText > len)
        return kRomBadLayout;
    if (!checksum_ok(data, rom->info))
        return kRomBadChecksum;
    rom->enabler = (rom->info + kChecksumText + 15) & ~(size_t)15;
    rom->enabler_end = len;
    if (len > rom->info + kChecksumText) {
        if (len < rom->enabler + kChecksumText ||
            !all_zero(data + rom->info + kChecksumText,
                      rom->enabler - rom->info - kChecksumText))
            return kRomBadLayout;
        rom->enabler_end = len - kChecksumText;
        if (!checksum_ok(data, rom->enabler_end))
            return kRomBadChecksum;
    }

    /* The parcels: find the 'rom ' child. */
    p = data + rom->parcels;
    if (rom->parcels_size < 16 || memcmp(p, "prcl\1\0\0\0", 8) != 0)
        return kRomBadLayout;
    for (link = be32(p + 12); link; link = be32(p + link)) {
        unsigned long hdr, count, csize;
        if (++nodes > kMaxNodes || link + kNodeSize > rom->parcels_size)
            return kRomBadLayout;
        hdr = be32(p + link + 8);
        count = be32(p + link + 16);
        csize = be32(p + link + 20);
        if (csize < kChildSize || count > kMaxChildren ||
            link + hdr > rom->parcels_size ||
            kNodeSize + count * csize > hdr)
            return kRomBadLayout;
        if (memcmp(p + link + 4, "rom ", 4) != 0)
            continue;
        for (i = 0; i < count; i++) {
            child = p + link + kNodeSize + i * csize;
            if (memcmp(child + 8, "lzss", 4) == 0 &&
                be32(child + 12) == kImageSize)
                rom->rom_child = (size_t)(child - p);
        }
    }
    if (!rom->rom_child)
        return kRomNoImage;
    child = p + rom->rom_child;
    {
        unsigned long ptr = be32(child + 24), packed = be32(child + 20);
        size_t got;
        if (ptr + packed > rom->parcels_size)
            return kRomBadLayout;
        if ((be32(child + 4) & 4) && crc32_of(p + ptr, packed) != be32(child + 16))
            return kRomBadChecksum;
        rom->image = malloc(kImageSize);
        if (!rom->image)
            return kRomNoMemory;
        got = lzss_decompress(p + ptr, packed, rom->image, kImageSize);
        if (got != kImageSize)
            return kRomBadImage;
    }

    if (!find_config(rom))
        return kRomNoConfigInfo;
    {
        /* ConfigInfo's offsets are signed. */
        long off = (long)rom->config_offset +
                   (int32_t)be32(rom->image + rom->config_offset +
                                 kKernelOffsetField);
        if (off < 0 || off + KPATCH_SIZE > kImageSize)
            return kRomUnknownKernel;
        rom->kernel_offset = (unsigned long)off;
    }
    {
        const unsigned char *k = rom->image + rom->kernel_offset;
        if (crc32_of(k, KPATCH_STOCK_SIZE) == KPATCH_STOCK_CRC &&
            all_zero(k + KPATCH_STOCK_SIZE, KPATCH_SIZE - KPATCH_STOCK_SIZE))
            rom->kernel = kKernelStock;
        else if (crc32_of(k, KPATCH_SIZE) == KPATCH_CRC)
            rom->kernel = kKernelPatched;
        else if (has_mark(k, KPATCH_SIZE))
            rom->kernel = kKernelOther;
    }
    return kRomOK;
}

/* Where an offset in the old parcels ends up in the new ones. */
static unsigned long moved(unsigned long x, size_t from, long delta)
{
    return x >= from ? (unsigned long)((long)x + delta) : x;
}

int rom_patch(Rom *rom, unsigned char **out_p, size_t *out_len,
              long *enabler_delta, void (*progress)(size_t, size_t))
{
    const unsigned char *data = rom->data;
    const unsigned char *old = data + rom->parcels;
    unsigned char *packed, *work, *out, *np, *k;
    unsigned long link, ptr, old_packed, flags;
    size_t plen, padded, next, i, new_size, new_info, new_enabler, pos;
    long delta;

    *out_p = NULL;
    if (rom->kernel != kKernelStock)
        return kRomNotStock;

    /* The kernel, and the image's checksum. */
    k = rom->image + rom->kernel_offset;
    for (i = 0; i < sizeof kpatch_runs / sizeof kpatch_runs[0]; i++)
        memcpy(k + kpatch_runs[i].offset, kpatch_data + kpatch_runs[i].data,
               kpatch_runs[i].length);
    memset(rom->image + rom->config_offset, 0, kSumsLen);
    {
        unsigned char sums[kSumsLen];
        image_sums(rom->image, rom->config_offset, sums);
        memcpy(rom->image + rom->config_offset, sums, kSumsLen);
    }

    packed = malloc(LZSS_BOUND(kImageSize));
    work = malloc(lzss_work_size());
    if (!packed || !work) {
        free(packed);
        free(work);
        return kRomNoMemory;
    }
    plen = lzss_compress(rom->image, kImageSize, packed, work, progress);
    free(work);

    /* The new image replaces the old one up to whatever came next in the
       parcels (a node header or another child's data), which moves by
       delta. */
    ptr = be32(old + rom->rom_child + 24);
    old_packed = be32(old + rom->rom_child + 20);
    next = rom->parcels_size;
    for (link = be32(old + 12); link; link = be32(old + link)) {
        unsigned long count = be32(old + link + 16);
        unsigned long csize = be32(old + link + 20);
        if (link > ptr && link < next)
            next = link;
        for (i = 0; i < count; i++) {
            unsigned long cp = be32(old + link + kNodeSize + i * csize + 24);
            if (cp > ptr && cp < next)
                next = cp;
        }
    }
    if (next < ptr + old_packed) {
        free(packed);
        return kRomBadLayout;
    }
    padded = (plen + 3) & ~(size_t)3;
    delta = (long)(ptr + padded) - (long)next;
    new_size = rom->parcels_size + delta;
    new_info = rom->parcels + new_size;
    new_enabler = (new_info + kChecksumText + 15) & ~(size_t)15;
    *out_len = rom->enabler_end > rom->info + kChecksumText
                   ? new_enabler + (rom->enabler_end - rom->enabler) +
                         kChecksumText
                   : new_info + kChecksumText;
    out = malloc(*out_len);
    if (!out) {
        free(packed);
        return kRomNoMemory;
    }

    /* Script, trampoline and parcels, with the new image. */
    memcpy(out, data, rom->parcels);
    np = out + rom->parcels;
    memcpy(np, old, ptr);
    memcpy(np + ptr, packed, plen);
    memset(np + ptr + plen, kPadByte, padded - plen);
    memcpy(np + ptr + padded, old + next, rom->parcels_size - next);
    free(packed);

    /* Every link and child pointer past the image moves. */
    put_be32(np + 12, moved(be32(old + 12), next, delta));
    for (link = be32(old + 12); link; link = be32(old + link)) {
        unsigned long count = be32(old + link + 16);
        unsigned long csize = be32(old + link + 20);
        unsigned char *node = np + moved(link, next, delta);
        put_be32(node, moved(be32(old + link), next, delta));
        for (i = 0; i < count; i++) {
            unsigned char *c = node + kNodeSize + i * csize;
            put_be32(c + 24, moved(be32(c + 24), next, delta));
        }
    }
    {
        unsigned char *c = np + moved(rom->rom_child, next, delta);
        flags = be32(c + 4);
        put_be32(c + 20, (unsigned long)plen);
        if (flags & 4)
            put_be32(c + 16, crc32_of(np + ptr, plen));
    }

    /* The script's sizes, and the checksums. */
    if (!put_constant(out, rom->span_parcels_size.at,
                      rom->span_parcels_size.len, (unsigned long)new_size) ||
        !put_constant(out, rom->span_info_size.at, rom->span_info_size.len,
                      (unsigned long)new_info)) {
        free(out);
        return kRomBadScript;
    }
    checksum_text(out + new_info, adler32_of(out, new_info));
    pos = new_info + kChecksumText;
    if (rom->enabler_end > rom->info + kChecksumText) {
        memset(out + pos, 0, new_enabler - pos);
        memcpy(out + new_enabler, data + rom->enabler,
               rom->enabler_end - rom->enabler);
        pos = new_enabler + (rom->enabler_end - rom->enabler);
        checksum_text(out + pos, adler32_of(out, pos));
    }
    *enabler_delta = (long)new_enabler - (long)rom->enabler;
    *out_p = out;
    return kRomOK;
}

void rom_close(Rom *rom)
{
    free(rom->image);
    rom->image = NULL;
}

const char *rom_error_text(int err)
{
    switch (err) {
    case kRomOK:
        return "no error";
    case kRomNotBootInfo:
        return "it is not a NewWorld Mac OS ROM file";
    case kRomBadScript:
        return "its boot script is not one Enforcer knows";
    case kRomBadChecksum:
        return "it is damaged (a checksum does not match)";
    case kRomBadLayout:
        return "its parts are not laid out as Enforcer expects";
    case kRomNoImage:
        return "it has no compressed ROM image";
    case kRomBadImage:
        return "its ROM image does not decompress";
    case kRomNoConfigInfo:
        return "its ROM image has no valid ConfigInfo";
    case kRomUnknownKernel:
        return "it does not contain NanoKernel " KPATCH_VERSION;
    case kRomNotStock:
        return "its NanoKernel is already changed";
    case kRomNoMemory:
        return "there is not enough memory";
    }
    return "unknown error";
}

void cfrg_adjust(unsigned char *cfrg, size_t len, long delta)
{
    size_t at = 32;
    unsigned long count, i;
    if (len < 32)
        return;
    count = be32(cfrg + 28);
    for (i = 0; i < count; i++) {
        if (len < at + 43)
            break;
        if (cfrg[at + 23] == 1) /* kDataForkCFragLocator */
            put_be32(cfrg + at + 24,
                     (unsigned long)((long)be32(cfrg + at + 24) + delta));
        at += 43 + cfrg[at + 42];
        at = (at + 3) & ~(size_t)3;
    }
}
