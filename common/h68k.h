/*
 * h68k.h: install the 68k write-fault hook (common/h68k.s).
 *
 * The hook sits in unused space of the emulator data page and replaces the
 * emulator's exception entry in the emulator context block, so a store
 * from 68k code to a protected page in the set range is made writable and
 * retried instead of becoming a 68k bus error. It stays installed after the
 * installing application quits; h68k_uninstall() removes it.
 *
 * Addresses are those of Mac OS 9.2.2 on QEMU mac99; h68k_install() checks
 * them and refuses to install when anything differs.
 */

#ifndef H68K_H
#define H68K_H

#include <stddef.h>
#include "nk.h"
#include "h68k_code.h"

enum {
    kH68kECB = 0x68FFF100,
    kH68kSlot = 0x68FFF14C,   /* ECB.ExceptionHandler */
    kH68kData = 0x68FFFB80,   /* = ECB + 0xA80, see h68k.s */
    kH68kCode = 0x68FFFD00,
    kH68kEnd = 0x68FFFF00,
    kH68kMagic = 'H68K'
};

typedef struct {
    unsigned long pc;   /* emulator instruction that faulted */
    unsigned long addr; /* fault address */
    unsigned long r24;  /* probably the 68k PC */
    unsigned long lr;
} H68kEvent;

/* Must match the D_ offsets in h68k.s. */
typedef struct {
    unsigned long magic;
    unsigned long count_all;     /* exceptions that entered the hook */
    unsigned long count_handled; /* writes made writable and retried */
    unsigned long lo, hi;        /* range to handle; hi = 0 for none */
    unsigned long area;
    unsigned long orig;          /* the emulator's own entry */
    unsigned long kstatus;       /* last MPSetAreaAccess status */
    unsigned long ring_idx;
    unsigned long spill[5];      /* CR, r0, r5, r6, r7 */
    unsigned long ecb[6];
    unsigned long count_passed;  /* write faults outside the range */
    unsigned long reserved[11];
    H68kEvent ring[16];
} H68kData;

_Static_assert(offsetof(H68kData, count_passed) == 0x50, "h68k.s D_COUNT_PASSED");
_Static_assert(offsetof(H68kData, ring) == 0x80, "h68k.s D_RING");
_Static_assert(kH68kData + sizeof(H68kData) <= kH68kCode, "data overlaps code");
_Static_assert(kH68kCode + sizeof h68k_code <= kH68kEnd, "code too long");

/* A pointer to a fixed address. The empty asm hides the constant from GCC,
   which otherwise warns that the address is outside any object. */
static inline volatile void *h68k_ptr(unsigned long addr)
{
    __asm__("" : "+r"(addr));
    return (volatile void *)addr;
}

static inline volatile H68kData *h68k_data(void)
{
    return h68k_ptr(kH68kData);
}

static inline int h68k_installed(void)
{
    volatile unsigned long *slot = h68k_ptr(kH68kSlot);
    return *slot == kH68kCode && h68k_data()->magic == kH68kMagic;
}

/* Write newly copied code back from the data cache and drop stale
   instructions (32-byte cache lines). */
static inline void h68k_flush(unsigned long start, unsigned long len)
{
    unsigned long a;
    for (a = start & ~31UL; a < start + len; a += 32)
        __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
    for (a = start & ~31UL; a < start + len; a += 32)
        __asm__ volatile("icbi 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync\n\tisync" : : : "memory");
}

/* Returns 0 when installed (or already installed), or:
   -1 the ECB's handler is not the emulator's own entry, -2 the space is in
   use, -3 the original entry is out of branch range, -4 the code is
   unexpected. */
static inline long h68k_install(unsigned long area)
{
    volatile unsigned long *slot = h68k_ptr(kH68kSlot);
    volatile H68kData *d = h68k_data();

    if (h68k_installed()) {
        d->area = area;
        return 0;
    }
    unsigned long orig = *slot;
    if (orig < 0x68060000 || orig >= 0x68080000) /* emulator code */
        return -1;
    for (unsigned long p = kH68kData; p < kH68kEnd; p += 4)
        if (*(volatile unsigned long *)h68k_ptr(p))
            return -2;

    unsigned long branch_at = kH68kCode + sizeof h68k_code - 4;
    long off = (long)orig - (long)branch_at;
    if (off < -0x2000000L || off >= 0x2000000L || (off & 3))
        return -3;

    volatile unsigned char *code = h68k_ptr(kH68kCode);
    for (unsigned long i = 0; i < sizeof h68k_code; i++)
        code[i] = h68k_code[i];
    volatile unsigned long *branch = h68k_ptr(branch_at);
    if (*branch != 0x48000000) {
        for (unsigned long i = 0; i < sizeof h68k_code; i++)
            code[i] = 0;
        return -4;
    }
    *branch = 0x48000000 | ((unsigned long)off & 0x03FFFFFC);
    h68k_flush(kH68kCode, sizeof h68k_code);

    d->lo = 0;
    d->hi = 0;
    d->area = area;
    d->orig = orig;
    d->magic = kH68kMagic;
    *slot = kH68kCode; /* one aligned store: the switch is atomic */
    return 0;
}

static inline void h68k_set_range(unsigned long lo, unsigned long hi)
{
    volatile H68kData *d = h68k_data();
    d->hi = 0;
    d->lo = lo;
    d->hi = hi;
}

static inline void h68k_uninstall(void)
{
    volatile unsigned long *slot = h68k_ptr(kH68kSlot);
    if (!h68k_installed())
        return;
    *slot = h68k_data()->orig;
    for (unsigned long p = kH68kData; p < kH68kEnd; p += 4)
        *(volatile unsigned long *)h68k_ptr(p) = 0;
}

#endif
