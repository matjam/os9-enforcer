/*
 * h68k.h: install the 68k write hook (common/h68k.s).
 *
 * The hook sits in unused space of the emulator data page and replaces the
 * emulator's exception entry in the emulator context block, so a store
 * from 68k code to a protected page it manages is made writable and retried
 * instead of becoming a 68k bus error or hanging the machine. Its ring is a
 * system-heap block. It stays installed after the installing application
 * quits; h68k_uninstall() removes it.
 *
 * Addresses are those of Mac OS 9.2.2 on QEMU mac99; h68k_install() checks
 * them and refuses to install when anything differs.
 */

#ifndef H68K_H
#define H68K_H

#include <MacMemory.h>
#include "nk.h"
#include "hook.h"
#include "h68k_code.h"

enum {
    kH68kECB = 0x68FFF100,
    kH68kSlot = 0x68FFF14C,   /* ECB.ExceptionHandler */
    kH68kData = 0x68FFFB80,   /* = ECB + 0xA80, see h68k.s */
    kH68kCode = 0x68FFFC40,
    kH68kEnd = 0x68FFFF00,
    kH68kMagic = 'H684'       /* changes with the data layout */
};

_Static_assert(kH68kData + sizeof(HookData) <= kH68kCode, "data overlaps code");
_Static_assert(kH68kCode + sizeof h68k_code <= kH68kEnd, "code too long");

static inline volatile HookData *h68k_data(void)
{
    return hook_ptr(kH68kData);
}

static inline int h68k_installed(void)
{
    volatile unsigned long *slot = hook_ptr(kH68kSlot);
    return *slot == kH68kCode && h68k_data()->magic == kH68kMagic;
}

/* Returns 0 when installed (or already installed), or:
   -1 the ECB's handler is not the emulator's own entry, -2 the space is in
   use, -3 the original entry is out of branch range, -4 the code is
   unexpected, -5 no memory for the ring. */
static inline long h68k_install(unsigned long area)
{
    volatile unsigned long *slot = hook_ptr(kH68kSlot);
    volatile HookData *d = h68k_data();

    if (h68k_installed()) {
        d->area = area;
        return 0;
    }
    unsigned long orig = *slot;
    if (orig < 0x68060000 || orig >= 0x68080000) /* emulator code */
        return -1;
    for (unsigned long p = kH68kData; p < kH68kEnd; p += 4)
        if (*(volatile unsigned long *)hook_ptr(p))
            return -2;

    unsigned long branch_at = kH68kCode + sizeof h68k_code - 4;
    long off = (long)orig - (long)branch_at;
    if (off < -0x2000000L || off >= 0x2000000L || (off & 3))
        return -3;
    HookEvent *ring = (HookEvent *)NewPtrSysClear(kHookRing * sizeof(HookEvent));
    if (!ring)
        return -5;

    volatile unsigned char *code = hook_ptr(kH68kCode);
    for (unsigned long i = 0; i < sizeof h68k_code; i++)
        code[i] = h68k_code[i];
    volatile unsigned long *branch = hook_ptr(branch_at);
    if (*branch != 0x48000000) {
        for (unsigned long i = 0; i < sizeof h68k_code; i++)
            code[i] = 0;
        DisposePtr((Ptr)ring);
        return -4;
    }
    *branch = 0x48000000 | ((unsigned long)off & 0x03FFFFFC);
    hook_flush(kH68kCode, sizeof h68k_code);

    d->lo = 0;
    d->hi = 0;
    d->area = area;
    d->orig = orig;
    d->ring = ring;
    d->magic = kH68kMagic;
    *slot = kH68kCode; /* one aligned store: the switch is atomic */
    return 0;
}

static inline void h68k_set_range(unsigned long lo, unsigned long hi)
{
    hook_set_range(h68k_data(), lo, hi);
}

static inline void h68k_uninstall(void)
{
    volatile unsigned long *slot = hook_ptr(kH68kSlot);
    if (!h68k_installed())
        return;
    volatile HookData *d = h68k_data();
    HookEvent *ring = d->ring;
    *slot = d->orig;
    for (unsigned long p = kH68kData; p < kH68kEnd; p += 4)
        *(volatile unsigned long *)hook_ptr(p) = 0;
    if (ring)
        DisposePtr((Ptr)ring);
}

#endif
