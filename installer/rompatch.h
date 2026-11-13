/*
 * Patch the NanoKernel inside a Mac OS ROM file's data fork.
 *
 * The data fork of a NewWorld Mac OS ROM file (file type 'tbxi') is:
 *   - a CHRP boot script for Open Firmware, whose "h# ... constant" lines
 *     give the offsets and sizes of the parts below;
 *   - the trampoline (MacOS.elf);
 *   - the parcels ('prcl'): a list of nodes, each with children. The 'rom '
 *     node's child is the 4 MB ROM image, LZSS-compressed, with a CRC-32.
 *     The image has a checksum of its own in its ConfigInfo page, and the
 *     NanoKernel at ConfigInfo's KernelCodeOffset;
 *   - an Adler-32 checksum of all the above (info-size bytes), as text;
 *   - the System Enabler's code fragments, at 16-byte alignment, which the
 *     resource fork's 'cfrg' finds by data fork offset;
 *   - an Adler-32 checksum of the whole file before it, as text.
 *
 * rom_patch rebuilds the data fork with the kernel patch applied (kpatch.h)
 * and every size, offset and checksum that depends on it. The System
 * Enabler moves by the change in the compressed image's size: adjust the
 * 'cfrg' resource with cfrg_adjust.
 *
 * Portable C, so tools/rompatch-test.sh can test it on the host.
 */

#ifndef ROMPATCH_H
#define ROMPATCH_H

#include <stddef.h>

typedef struct {
    unsigned long offset, length, data;
} KPatchRun;

enum {
    kRomOK,
    kRomNotBootInfo,   /* no CHRP boot script */
    kRomBadScript,     /* a constant is missing or does not fit */
    kRomBadChecksum,   /* an Adler-32 or CRC-32 does not match */
    kRomBadLayout,     /* the parts are not where expected */
    kRomNoImage,       /* no compressed ROM image in the parcels */
    kRomBadImage,      /* the image does not decompress to 4 MB */
    kRomNoConfigInfo,  /* no ConfigInfo page with a valid checksum */
    kRomUnknownKernel, /* the kernel is neither stock 2.28 nor patched */
    kRomNotStock,      /* rom_patch on an image that is not stock */
    kRomNoMemory
};

enum {
    kKernelStock,      /* NanoKernel 2.28 as Apple made it */
    kKernelPatched,    /* this release's patch is applied */
    kKernelUnknown
};

typedef struct {
    const unsigned char *data;  /* the data fork */
    size_t len;
    int kernel;                 /* kKernel... */
    unsigned long kernel_offset;   /* in the image */
    unsigned long config_offset;   /* the ConfigInfo page, in the image */
    unsigned char *image;          /* the decompressed ROM image */
    /* internal */
    size_t parcels, parcels_size, info, enabler, enabler_end;
    size_t rom_child;              /* the 'rom ' child entry, in the parcels */
    struct { size_t at, len; } span_parcels_size, span_info_size;
} Rom;

/* Reads and checks a data fork, and decompresses its ROM image. data must
   stay valid until rom_close. */
int rom_open(Rom *rom, const unsigned char *data, size_t len);

/* Applies the patch and writes the new data fork to *out (malloc'd; the
   caller frees it). *enabler_delta is how far the System Enabler moved.
   progress, if not NULL, is called during the compression. */
int rom_patch(Rom *rom, unsigned char **out, size_t *out_len,
              long *enabler_delta, void (*progress)(size_t done, size_t n));

void rom_close(Rom *rom);

const char *rom_error_text(int err);

/* Moves the data fork offsets in a 'cfrg' resource by delta. */
void cfrg_adjust(unsigned char *cfrg, size_t len, long delta);

unsigned long crc32_of(const unsigned char *p, size_t n);
unsigned long adler32_of(const unsigned char *p, size_t n);

#endif
