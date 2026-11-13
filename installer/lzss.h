/*
 * LZSS as the Mac OS ROM file uses it for the ROM image: Haruhiko Okumura's
 * scheme (1989) with a 4096-byte window filled with spaces, matches of 3 to
 * 18 bytes, and a flag byte before each 8 items (1 a literal byte, 0 a
 * 2-byte match). lzss_compress makes the same bytes as Apple's compressor.
 *
 * Portable C, so tools/rompatch-test.sh can test it on the host.
 */

#ifndef LZSS_H
#define LZSS_H

#include <stddef.h>

/* The most lzss_compress can write for n bytes. */
#define LZSS_BOUND(n) ((n) + (n) / 8 + 16)

/* Compresses n bytes from in into out, which holds LZSS_BOUND(n) bytes, and
   returns the length. work is lzss_work_size() bytes of scratch memory.
   progress, if not NULL, is called now and then with the bytes done. */
size_t lzss_work_size(void);
size_t lzss_compress(const unsigned char *in, size_t n, unsigned char *out,
                     void *work, void (*progress)(size_t done, size_t n));

/* Decompresses n bytes from in into out, stopping at out_len bytes.
   Returns the bytes written, or (size_t)-1 if in ends inside a match. */
size_t lzss_decompress(const unsigned char *in, size_t n, unsigned char *out,
                       size_t out_len);

#endif
