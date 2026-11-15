/*
 * hyield.h: the yield stub of the kernel's preemption (common/hyield.s,
 * kernel patch 6). Its data, YieldData, is the first 128 bytes of the
 * stub's block; the code follows. The engine copies hyield_code (from
 * hyield_code.h, which hooks_embed generates) into a system-heap block,
 * fills check and wne, and registers the entry with kKcbOpPreempt.
 */

#ifndef HYIELD_H
#define HYIELD_H

#include <stddef.h>

enum { kYieldMagic = 'YLD1' };

/* The stub calls check(sp, pc, lr, data) with the preempted code's r1, PC
   and LR, at its level; a result other than 0 makes it call
   WaitNextEvent. */
typedef struct YieldData YieldData;
typedef long (*YieldCheck)(unsigned long sp, unsigned long pc,
                           unsigned long lr, volatile YieldData *y);

struct YieldData {
    unsigned long magic;     /* kYieldMagic */
    unsigned long entry;     /* the entry, from the block's start */
    unsigned long vecbr;     /* the branch over the AltiVec saves */
    YieldCheck check;        /* the engine's check (a TVector) */
    void *wne;               /* WaitNextEvent (a TVector) */
    /* The stub's counts: entries, WaitNextEvent calls, and redirects whose
       PC the kernel no longer had. */
    volatile unsigned long entered, yielded, lost;
    /* The check's: an application that has not given up the processor
       yet; above task level; a return address on the stack not the
       application's code below one that is (a Toolbox call in progress);
       r1 outside the application's stack, or too close to its limit; a
       chain that does not end at the stack's base; safe. */
    volatile unsigned long starting, not_task, in_call, other_stack,
        low_stack, broken, safe;
    /* The last check: r1, PC, LR, the current application's owner byte,
       the stack's base, frames followed, and the first return addresses
       on the chain, then the link it ended on. */
    volatile unsigned long last_sp, last_pc, last_lr, last_owner, last_base,
        last_n;
    volatile unsigned long chain[11];
};

_Static_assert(offsetof(YieldData, check) == 0x0C, "D_CHECK");
_Static_assert(offsetof(YieldData, wne) == 0x10, "D_WNE");
_Static_assert(offsetof(YieldData, entered) == 0x14, "D_ENTERED");
_Static_assert(offsetof(YieldData, lost) == 0x1C, "D_LOST");
_Static_assert(sizeof(YieldData) == 0x80, "D_SIZE");

#endif
