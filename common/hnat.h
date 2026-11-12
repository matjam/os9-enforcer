/*
 * hnat.h: install the native write hook (common/hnat.s).
 *
 * The hook replaces the ROM Exception Manager's entry in the native context
 * block, so a store from native code to a protected page it manages is made
 * writable and retried before the Exception Manager sees it (the Exception
 * Manager saves registers on the faulting stack, so it cannot survive a
 * fault on a protected stack page). Its code, data block and ring are one
 * system-heap block. It stays installed after the installing application
 * quits; hnat_uninstall() removes it.
 */

#ifndef HNAT_H
#define HNAT_H

#include <string.h>
#include <MacMemory.h>
#include "nk.h"
#include "hook.h"
#include "hnat_code.h"

enum {
    kHnatNCB = 0x68FFF400,
    kHnatSlot = 0x68FFF44C,   /* NCB.ExceptionHandler */
    kHnatMagic = 'HNAT'
};

/* The block: data (0xC0 bytes), then code (32-byte aligned), then ring. */
static inline volatile HookData *hnat_data(void)
{
    volatile unsigned long *slot = hook_ptr(kHnatSlot);
    unsigned long code = *slot;
    if (code >= 0x40000000 || code < sizeof(HookData)) /* ROM or nothing */
        return 0;
    volatile HookData *d = (volatile HookData *)(code - sizeof(HookData));
    return d->magic == kHnatMagic ? d : 0;
}

static inline int hnat_installed(void)
{
    return hnat_data() != 0;
}

/* Returns 0 when installed (or already installed), or:
   -1 the NCB's handler is not in ROM or not reachable by an absolute
   branch, -4 the code is unexpected, -5 no memory. */
static inline long hnat_install(unsigned long area)
{
    volatile unsigned long *slot = hook_ptr(kHnatSlot);
    volatile HookData *d = hnat_data();
    if (d) {
        d->area = area;
        return 0;
    }
    unsigned long orig = *slot;
    if (orig < 0xFFC00000 || (orig & 3)) /* ROM, and in reach of `ba` */
        return -1;

    /* + 64: up to 31 bytes each to align the code and the ring. */
    unsigned long size = sizeof(HookData) + 64 + sizeof hnat_code +
                         kHookRing * sizeof(HookEvent);
    Ptr block = NewPtrSysClear(size);
    if (!block)
        return -5;
    unsigned long code = ((unsigned long)block + sizeof(HookData) + 31) & ~31UL;
    d = (volatile HookData *)(code - sizeof(HookData));
    unsigned long data = (unsigned long)d;

    unsigned long *c = (unsigned long *)code;
    unsigned long n = sizeof hnat_code / 4, pairs = 0, branches = 0;
    memcpy(c, hnat_code, sizeof hnat_code);
    for (unsigned long i = 0; i < n; i++) {
        if (c[i] == 0x3C800000 && i + 1 < n && c[i + 1] == 0x38840000) {
            c[i] |= ((data + 0x8000) >> 16) & 0xFFFF; /* lis r4,ha */
            c[i + 1] |= data & 0xFFFF;                /* addi r4,r4,lo */
            pairs++;
        } else if (c[i] == 0x48000002) {
            c[i] = 0x48000002 | (orig & 0x03FFFFFC); /* ba orig */
            branches++;
        }
    }
    if (pairs != 2 || branches != 1) {
        DisposePtr(block);
        return -4;
    }
    hook_flush(code, sizeof hnat_code);

    d->ring = (HookEvent *)(code + ((sizeof hnat_code + 31) & ~31UL));
    d->area = area;
    d->orig = orig;
    d->block = (unsigned long)block;
    d->magic = kHnatMagic;
    *slot = code; /* one aligned store: the switch is atomic */
    return 0;
}

static inline void hnat_uninstall(void)
{
    volatile unsigned long *slot = hook_ptr(kHnatSlot);
    volatile HookData *d = hnat_data();
    if (!d)
        return;
    *slot = d->orig;
    d->magic = 0;
    DisposePtr((Ptr)d->block);
}

#endif
