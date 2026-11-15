/*
 * kcb.h: the control block of the kernel's owner check (kernel/nanokernel.s,
 * patch 4), and the calls that register it. Keep in step with the CB_, EV_
 * and EVF_ constants there.
 *
 * The engine allocates the block (16 KB, page aligned, physically
 * contiguous) and an owner table (one byte per 4 KB page of logical memory,
 * from 0 up to 2 GB), puts the physical address of each table page in
 * pages[], and registers the block's physical address with MP call 109. A
 * kernel without patch 4 has no call 109 and returns an error, and a kernel
 * with another version of the block returns paramErr.
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
    kKcbVersion = 6,          /* 5: no stacks, 488 events;
                                 4: no application VSIDs;
                                 3: writable by Mac OS, one denied page;
                                 2: no applzone; 1: 64 table pages and
                                 496 events */
    kKcbSize = 0x4000,
    kKcbRing = 480,
    kKcbStacks = 32,          /* stack ranges, one per application slot */
    kKcbTablePages = 128,     /* 128 pages of 4096 entries: 2 GB */
    kKcbFrameLen = 0x70,
    kMPEnforcer = 109
};

/* KernelCB.mode */
enum { kKcbPass, kKcbLog, kKcbEnforce };

/* Owner bytes. An application's is kOwnerApp + its domain's slot.
   kOwnerTables marks the control block's and the owner table's pages: the
   kernel drops every store into them. An application's byte with
   kOwnerCode marks a page where its code lies but which is not its memory
   (the partly used first page of its partition, code outside it): the
   kernel counts the code there as the application's, and treats the page
   as nobody's. */
enum {
    kOwnerNone = 0,
    kOwnerSystem = 1,
    kOwnerTables = 2,
    kOwnerApp = 3,
    kOwnerCode = 0x80
};

/* MP call 109 operations (r3) on the registered block, which Mac OS
   cannot write itself. Spaces, Switch and Flush are the kernel's
   application VSIDs (kernel/nanokernel.s, patch 5). */
enum {
    kKcbOpOwners = 1,
    kKcbOpMode = 2,
    kKcbOpCurrent = 3,
    kKcbOpSpaces = 4,  /* r4 on, r5 segment mask (bit n: segment n, 0-5) */
    kKcbOpSwitch = 5,  /* r4 a zone: its application's VSIDs */
    kKcbOpFlush = 6,   /* r4 first page, r5 pages: drop their PTEs */
    kKcbOpPatch = 7,   /* r4 address, r5 old word, r6 new: kcb_patch */
    /* Preemption (kernel patch 6). */
    kKcbOpPreempt = 8, /* r4 slice in timebase ticks (0 off), r5 the stub */
    kKcbOpYield = 9,   /* from the stub: returns the PC; next redirect in r4
                          ticks (0: one slice) */
    kKcbOpResume = 10, /* from the stub: r4 a register block (common/
                          hyield.s); needs no block registered */
    kKcbOpFresh = 11,  /* r4 a new application's owner: yielded[r4] = 0 */
    /* Sparse memory (kernel patch 7): between the two addresses of
       kKcbOpSparse, a page of an area (MPCreateArea) that has no physical
       page gets one from the kernel's pool, cleared, when it is first
       touched. A kernel without the patch returns paramErr for these. */
    kKcbOpSparse = 12,     /* r4, r5 the range (0, 0: none) */
    kKcbOpPoolPut = 13,    /* r4 a physical page the caller will not use */
    kKcbOpPoolGet = 14,    /* returns a physical page from the pool, or 0 */
    kKcbOpSparseFree = 15, /* r4 an address, r5 pages: back into the pool;
                              returns how many it took */
    kKcbOpSparseInfo = 16, /* r4 0: pages in the pool; 1: pages given; 2:
                              faults with the pool empty */
    kKcbOpPoolTake = 17,   /* r4, r5 a physical range: its pages out of the
                              pool, or with r6 not 0 only counted; returns
                              how many */
    kKcbOpSparseCount = 18, /* r4 an address, r5 pages: how many of them
                               have memory. paramErr from a kernel before
                               it. */
    kKcbOpStack = 19        /* r4 a slot, r5 and r6 the start and end of
                               that application's stack (0, 0: none) */
};

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
    /* Owner byte of the current application, or 0. With applzone set, the
       kernel writes it at each store it decides; without, the engine does
       on its tick. */
    volatile unsigned long current;
    unsigned long entries;              /* pages the owner table covers */
    volatile unsigned long denied_page; /* the last denied page, any kind */
    volatile unsigned long seq;         /* events recorded */
    unsigned long frame;                /* the emulator's frame code, or 0 */
    volatile unsigned long allowed, logged, denied, bail, self;
    volatile unsigned long skipped; /* 0 with a kernel that never skips */
    /* The physical address of ApplZone (low memory 0x2AA), or 0. The
       kernel reads it at each store to find the current application: the
       owner of the page ApplZone points into. */
    unsigned long applzone;
    /* The last denied native store and 68k store: the page, then the
       event's number. The hook that takes the fault denies a store to that
       page once for each number. */
    volatile unsigned long deny_native[2], deny_68k[2];
    volatile unsigned long tables; /* stores into the tables, dropped */
    /* Application VSIDs: on (kKcbOpSpaces), PTEs built again for another
       VSID, switches by kKcbOpSwitch, and switches the kernel made at a
       store because ApplZone had changed without one. */
    volatile unsigned long spaces, rebuilt, vswitch, lazy;
    volatile unsigned long lazy_pc; /* the PC of the last lazy switch */
    /* Preemption (kKcbOpPreempt): the slice in timebase ticks (0 off), the
       timebase (low word) before which no redirect is made, the yield
       stub, the PC a redirect replaced (0 when none is pending), redirects
       made, and ApplZone when the last slice started. */
    volatile unsigned long slice, next, stub, yield_pc, preempts, zone;
    unsigned long reserved;
    /* One byte per owner: 1 once ApplZone left that application, which
       then has called WaitNextEvent (or the like) itself. */
    volatile unsigned char yielded[128];
    unsigned long pages[kKcbTablePages]; /* physical */
    KEvent ring[kKcbRing];              /* event n at (n - 1) % kKcbRing */
    /* Each application's stack, start and end (kKcbOpStack). In enforce
       mode a store by another application's code into one is allowed and
       recorded as an offense: Mac OS calls one application's routine for
       another (a window's or menu's code, a callback) in the routine's
       own context, and the routine puts its results on the caller's
       stack. */
    volatile unsigned long stacks[2 * kKcbStacks];
} KernelCB;

_Static_assert(offsetof(KernelCB, frame) == 0x1C, "CB_FRAME");
_Static_assert(offsetof(KernelCB, self) == 0x30, "CB_SELF");
_Static_assert(offsetof(KernelCB, skipped) == 0x34, "CB_SKIPPED");
_Static_assert(offsetof(KernelCB, applzone) == 0x38, "CB_APPLZONE");
_Static_assert(offsetof(KernelCB, deny_native) == 0x3C, "CB_DENY_NATIVE");
_Static_assert(offsetof(KernelCB, deny_68k) == 0x44, "CB_DENY_68K");
_Static_assert(offsetof(KernelCB, tables) == 0x4C, "CB_TABLES");
_Static_assert(offsetof(KernelCB, spaces) == 0x50, "CB_SPACES");
_Static_assert(offsetof(KernelCB, lazy) == 0x5C, "CB_LAZY");
_Static_assert(offsetof(KernelCB, lazy_pc) == 0x60, "CB_LAZY_PC");
_Static_assert(offsetof(KernelCB, slice) == 0x64, "CB_SLICE");
_Static_assert(offsetof(KernelCB, zone) == 0x78, "CB_ZONE");
_Static_assert(offsetof(KernelCB, yielded) == 0x80, "CB_YIELDED");
_Static_assert(offsetof(KernelCB, pages) == 0x100, "CB_PAGES");
_Static_assert(offsetof(KernelCB, ring) == 0x300, "CB_RING");
_Static_assert(offsetof(KernelCB, stacks) == 0x3F00, "CB_STACKS");
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

/* Set the owner byte of count pages from page first to owner, where the
   byte is replace (or every byte, with replace -1). The kernel leaves out
   pages past the table's end. */
static inline long kcb_owners(unsigned long first, unsigned long count,
                              unsigned owner, long replace)
{
    KRegs k = {0};
    k.r3 = kKcbOpOwners;
    k.r4 = first;
    k.r5 = count;
    k.r6 = owner;
    k.r7 = (unsigned long)replace;
    return nk_call(kMPEnforcer, &k);
}

/* kKcbOpMode, kKcbOpCurrent or kKcbOpSwitch; or one with a second
   argument (kKcbOpSpaces, kKcbOpFlush). */
static inline long kcb_set(unsigned long op, unsigned long value)
{
    KRegs k = {0};
    k.r3 = op;
    k.r4 = value;
    return nk_call(kMPEnforcer, &k);
}

static inline long kcb_set2(unsigned long op, unsigned long a,
                            unsigned long b)
{
    KRegs k = {0};
    k.r3 = op;
    k.r4 = a;
    k.r5 = b;
    return nk_call(kMPEnforcer, &k);
}

/* Replace the word old at addr with new, through its physical address,
   and make it the code the CPU runs. The page must have a PTE (read the
   word first), and should be held: the change does not mark the page
   modified, so a file-mapped page read in again loses it. Returns 0, or
   paramErr when the word is not old or the page has no PTE. */
static inline long kcb_patch(unsigned long addr, unsigned long old,
                             unsigned long new_word)
{
    KRegs k = {0};
    k.r3 = kKcbOpPatch;
    k.r4 = addr;
    k.r5 = old;
    k.r6 = new_word;
    return nk_call(kMPEnforcer, &k);
}

#endif
