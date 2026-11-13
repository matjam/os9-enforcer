/*
 * LZSS (lzss.h), after Haruhiko Okumura's LZSS.C (1989). The encoder keeps
 * the last N positions in binary search trees, one per first byte, and
 * takes the longest match; on a tie, the one found first. Apple's
 * compressor made its choices the same way, so the output is identical.
 */

#include <string.h>

#include "lzss.h"

enum {
    N = 4096,      /* window */
    F = 18,        /* longest match */
    THRESHOLD = 2, /* a match must be longer than this */
    NIL = N        /* no node */
};

typedef struct {
    unsigned char text[N + F - 1];
    short lson[N + 1], rson[N + 257], dad[N + 1];
    int match_position, match_length;
} Work;

size_t lzss_work_size(void)
{
    return sizeof(Work);
}

/* Inserts the string at r into the tree for its first byte, and finds the
   longest match for it. A node that matches all F bytes is replaced. */
static void insert_node(Work *w, int r)
{
    const unsigned char *key = &w->text[r];
    int p = N + 1 + key[0];
    int cmp = 1;
    w->rson[r] = w->lson[r] = NIL;
    w->match_length = 0;
    for (;;) {
        int i;
        if (cmp >= 0) {
            if (w->rson[p] != NIL) {
                p = w->rson[p];
            } else {
                w->rson[p] = r;
                w->dad[r] = p;
                return;
            }
        } else {
            if (w->lson[p] != NIL) {
                p = w->lson[p];
            } else {
                w->lson[p] = r;
                w->dad[r] = p;
                return;
            }
        }
        for (i = 1; i < F; i++)
            if ((cmp = key[i] - w->text[p + i]) != 0)
                break;
        if (i > w->match_length) {
            w->match_position = p;
            if ((w->match_length = i) >= F)
                break;
        }
    }
    w->dad[r] = w->dad[p];
    w->lson[r] = w->lson[p];
    w->rson[r] = w->rson[p];
    w->dad[w->lson[p]] = r;
    w->dad[w->rson[p]] = r;
    if (w->rson[w->dad[p]] == p)
        w->rson[w->dad[p]] = r;
    else
        w->lson[w->dad[p]] = r;
    w->dad[p] = NIL;
}

static void delete_node(Work *w, int p)
{
    int q;
    if (w->dad[p] == NIL)
        return;
    if (w->rson[p] == NIL) {
        q = w->lson[p];
    } else if (w->lson[p] == NIL) {
        q = w->rson[p];
    } else {
        q = w->lson[p];
        if (w->rson[q] != NIL) {
            do
                q = w->rson[q];
            while (w->rson[q] != NIL);
            w->rson[w->dad[q]] = w->lson[q];
            w->dad[w->lson[q]] = w->dad[q];
            w->lson[q] = w->lson[p];
            w->dad[w->lson[p]] = q;
        }
        w->rson[q] = w->rson[p];
        w->dad[w->rson[p]] = q;
    }
    w->dad[q] = w->dad[p];
    if (w->rson[w->dad[p]] == p)
        w->rson[w->dad[p]] = q;
    else
        w->lson[w->dad[p]] = q;
    w->dad[p] = NIL;
}

size_t lzss_compress(const unsigned char *in, size_t n, unsigned char *out,
                     void *work, void (*progress)(size_t done, size_t n))
{
    Work *w = work;
    unsigned char code[17], mask;
    int i, len, r, s, last, code_ptr;
    size_t pos = 0, done = 0, outlen = 0, next_report = 0;

    for (i = N + 1; i <= N + 256; i++)
        w->rson[i] = NIL;
    for (i = 0; i < N; i++)
        w->dad[i] = NIL;
    code[0] = 0;
    code_ptr = 1;
    mask = 1;
    s = 0;
    r = N - F;
    memset(w->text, 0, sizeof w->text);
    memset(w->text, ' ', r);
    for (len = 0; len < F && pos < n; len++)
        w->text[r + len] = in[pos++];
    if (len == 0)
        return 0;
    for (i = 1; i <= F; i++)
        insert_node(w, r - i);
    insert_node(w, r);
    do {
        if (w->match_length > len)
            w->match_length = len;
        if (w->match_length <= THRESHOLD) {
            w->match_length = 1;
            code[0] |= mask;
            code[code_ptr++] = w->text[r];
        } else {
            code[code_ptr++] = (unsigned char)w->match_position;
            code[code_ptr++] = (unsigned char)(
                ((w->match_position >> 4) & 0xF0) |
                (w->match_length - (THRESHOLD + 1)));
        }
        if ((mask <<= 1) == 0) {
            memcpy(out + outlen, code, code_ptr);
            outlen += code_ptr;
            code[0] = 0;
            code_ptr = 1;
            mask = 1;
        }
        last = w->match_length;
        for (i = 0; i < last && pos < n; i++) {
            unsigned char c = in[pos++];
            delete_node(w, s);
            w->text[s] = c;
            if (s < F - 1)
                w->text[s + N] = c;
            s = (s + 1) & (N - 1);
            r = (r + 1) & (N - 1);
            insert_node(w, r);
        }
        done += i;
        while (i++ < last) {
            delete_node(w, s);
            s = (s + 1) & (N - 1);
            r = (r + 1) & (N - 1);
            if (--len)
                insert_node(w, r);
        }
        if (progress && done >= next_report) {
            progress(done, n);
            next_report = done + 0x10000;
        }
    } while (len > 0);
    if (code_ptr > 1) {
        memcpy(out + outlen, code, code_ptr);
        outlen += code_ptr;
    }
    return outlen;
}

size_t lzss_decompress(const unsigned char *in, size_t n, unsigned char *out,
                       size_t out_len)
{
    unsigned char ring[N];
    size_t i = 0, o = 0;
    int r = N - F;
    memset(ring, ' ', sizeof ring);
    while (i < n && o < out_len) {
        unsigned flags = in[i++];
        int bit;
        for (bit = 0; bit < 8 && i < n && o < out_len; bit++) {
            if (flags & (1u << bit)) {
                unsigned char c = in[i++];
                out[o++] = c;
                ring[r] = c;
                r = (r + 1) & (N - 1);
            } else {
                int k, at, count;
                if (i + 1 >= n)
                    return (size_t)-1;
                at = in[i] | ((in[i + 1] & 0xF0) << 4);
                count = (in[i + 1] & 0x0F) + THRESHOLD + 1;
                i += 2;
                for (k = 0; k < count && o < out_len; k++) {
                    unsigned char c = ring[(at + k) & (N - 1)];
                    out[o++] = c;
                    ring[r] = c;
                    r = (r + 1) & (N - 1);
                }
            }
        }
    }
    return o;
}
