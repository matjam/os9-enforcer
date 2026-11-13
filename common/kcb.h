/*
 * kcb.h: the control block of the kernel's owner check (kernel/nanokernel.s,
 * patch 4), and the calls that register it. Keep in step with the CB_, EV_
 * and EVF_ constants there.
 *
 * The engine allocates the block (16 KB, page aligned, physically
 * contiguous) and an owner table (one byte per 4 KB page of logical memory,
 * from 0 to MemTop), puts the physical address of each table page in
 * pages[], and registers the block's physical address with MP call 109. A
 * kernel without patch 4 has no call 109 and returns an error, and the
 * engine runs on its hooks alone.
 *
 * With the block registered and mode kKcbLog or kKcbEnforce, the kernel
 * decides each store to a write-protected page whose owner byte is not 0,
 * in every task: it allows it (makes the page's PTE writable and runs the
 * store again) or denies it (passes the fault on). See the comment at
 * task_write in kernel/nanokernel.s for the rules.
 */

#ifndef KCB_H
#define KCB_H

#include <stddef.h>
#include <MacTypes.h>

#include "nk.h"

enum {
    kKcbMagic = 'ENFK',
    kKcbVersion = 1,
    kKcbSize = 0x4000,
    kKcbRing = 496,
    kKcbTablePages = 64,      /* 64 pages of 4096 entries: 1 GB */
    kKcbFrameLen = 0x70,
    kMPEnforcer = 109
};

/* KernelCB.mode */
enum { kKcbPass, kKcbLog, kKcbEnforce };

/* Owner bytes. An application's is kOwnerApp + its domain's slot. */
enum { kOwnerNone = 0, kOwnerSystem = 1, kOwnerApp = 3 };

/* KEvent.info, low byte. */
enum {
    kEvf68k = 1,     /* the 68k emulator's store */
    kEvfTask = 2,    /* not the blue task (an MP task) */
    kEvfOffense = 4, /* an application's code, not into its own page */
    kEvfDenied = 8,  /* and denied: the page stays protected */
    kEvfSkipped = 16 /* or dropped: the store was skipped, the page stays
                        protected, and the writer goes on */
};

/* A store the kernel decided. For 68k stores, r24 is the next 68k
   instruction + 2 and sp is the 68k A7; for native code, sp is r1. */
typedef struct {
    unsigned long seq;  /* KernelCB.seq after this event; 0 = empty */
    unsigned long pc;   /* the faulting instruction (the emulator's) */
    unsigned long addr;
    unsigned long r24;
    unsigned long lr;
    unsigned long sp;
    unsigned long info; /* page owner << 24 | code owner << 16 |
                           current << 8 | kEvf flags */
    unsigned long task; /* the task's ID */
} KEvent;

static inline unsigned kev_owner(unsigned long info) { return info >> 24; }
static inline unsigned kev_writer(unsigned long info) { return (info >> 16) & 0xFF; }
static inline unsigned kev_flags(unsigned long info) { return info & 0xFF; }

typedef struct {
    unsigned long magic, version;
    volatile unsigned long mode;
    volatile unsigned long current;     /* owner byte of the current app, or 0 */
    unsigned long entries;              /* pages the owner table covers */
    volatile unsigned long denied_page; /* the hooks clear it when they deny */
    volatile unsigned long seq;         /* events recorded */
    unsigned long frame;                /* the emulator's frame code, or 0 */
    volatile unsigned long allowed, logged, denied, bail, self;
    volatile unsigned long skipped; /* 0 with a kernel that never skips */
    unsigned long reserved[50];
    unsigned long pages[kKcbTablePages]; /* physical */
    KEvent ring[kKcbRing];              /* event n at (n - 1) % kKcbRing */
} KernelCB;

_Static_assert(offsetof(KernelCB, frame) == 0x1C, "CB_FRAME");
_Static_assert(offsetof(KernelCB, self) == 0x30, "CB_SELF");
_Static_assert(offsetof(KernelCB, skipped) == 0x34, "CB_SKIPPED");
_Static_assert(offsetof(KernelCB, pages) == 0x100, "CB_PAGES");
_Static_assert(offsetof(KernelCB, ring) == 0x200, "CB_RING");
_Static_assert(sizeof(KEvent) == 32, "event size");
_Static_assert(sizeof(KernelCB) == kKcbSize, "block size");

/* Memory Manager GetPhysical (InterfaceLib), which the Multiversal
   Interfaces do not declare. */
typedef struct {
    void *address;
    unsigned long count;
} KcbBlock;

typedef struct {
    KcbBlock logical;
    KcbBlock physical[8];
} KcbTranslation;

extern OSErr GetPhysical(KcbTranslation *addresses, unsigned long *count);

/* Virtual Memory Manager calls (InterfaceLib), which the Multiversal
   Interfaces do not declare. Hold keeps pages in physical memory; Lock
   also keeps them at the same physical addresses, and LockMemoryContiguous
   makes them one physical range first. */
extern OSErr HoldMemory(void *address, unsigned long count);
extern OSErr UnholdMemory(void *address, unsigned long count);
extern OSErr LockMemory(void *address, unsigned long count);
extern OSErr LockMemoryContiguous(void *address, unsigned long count);
extern OSErr UnlockMemory(void *address, unsigned long count);

/* The physical address of [addr, addr + len) when it is one physical
   range, or 0. */
static inline unsigned long kcb_physical(const volatile void *addr,
                                         unsigned long len)
{
    KcbTranslation t;
    unsigned long n = 8;
    t.logical.address = (void *)addr;
    t.logical.count = len;
    if (GetPhysical(&t, &n) || n != 1 || t.physical[0].count != len)
        return 0;
    return (unsigned long)t.physical[0].address;
}

/* Register the block at physical address phys, or unregister with 0.
   Returns 0 or the kernel's error (a kernel without patch 4: an MP error). */
static inline long kcb_register(unsigned long phys)
{
    KRegs k = {0};
    k.r3 = phys;
    return nk_call(kMPEnforcer, &k);
}

#endif
