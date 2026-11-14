/*
 * hook.h: the data block shared by the 68k write hook (h68k.s) and the
 * native write hook (hnat.s), and helpers for both.
 *
 * Each hook handles a write fault on a page it manages: inside [lo, hi)
 * and, when bitmap is set, with the page's bit set. It makes the page
 * writable and retries the store. A write inside [cur_lo, cur_hi) is only
 * counted (count_self); any other is recorded in the ring (count_handled is
 * its sequence number). Faults it does not handle go on to the original
 * handler and are counted in count_passed, with the last one in passed.
 *
 * The engine moves [cur_lo, cur_hi) on its tick, up to 16 ms after a
 * switch. While ApplZone is outside it, the range is the previous
 * application's: no write is counted as self, and no code is the current
 * application's, so only the kernel's denial (kdeny) denies.
 *
 * Deny rule: a recorded store whose code lies inside [cur_lo, cur_hi) is
 * the current application's own code writing outside its memory. The hook
 * sets kHookAppCode in the event's pc. When the fault address is at or
 * above deny_from, it also leaves the page protected and passes the fault
 * on, which stops the application; such faults are counted in count_denied,
 * with the last one in denied. deny_from is ~0 to deny nothing and 0 to
 * deny everywhere. The code's address is the faulting PC for native code
 * and the next 68k instruction (r24 - 2) for 68k code; for 68k code, a
 * store by the emulator's own call-frame code (frame, kHookFrameLen bytes)
 * is not the application's, although r24 then points at its trap. When
 * stop is set, a denial also stores ApplZone (the application to stop) in
 * *stop.
 */

#ifndef HOOK_H
#define HOOK_H

#include <stddef.h>

enum { kHookRing = 256 }; /* ring entries; RING_N in the .s files */

/* FRAME_LEN in h68k.s: the emulator code that fills a 68k-to-native call
   frame (9.2.2: 0x6806E300). */
enum { kHookFrameLen = 0x70 };

/* Set in HookEvent.pc (PowerPC instructions are 4-byte aligned, so the
   low bits are free): the store's code is the current application's. */
enum { kHookAppCode = 1 };

/* Set in a native HookEvent.pc by the engine's fault handler: the store was
   denied, but it came from code running above task level (a deferred task,
   a secondary interrupt handler), where stopping the application would
   leave the interrupted code unfinished. The handler skipped the store
   instead. */
enum { kHookSkipped = 2 };

/* A store a hook handled. Must match the R_ offsets in h68k.s and hnat.s.
   For h68k, r24 is the next 68k instruction + 2 and sp is the 68k A7; for
   hnat, r24 holds the interrupted r3 and sp is r1. */
typedef struct {
    unsigned long seq;  /* count_handled after this event; 0 = empty */
    unsigned long pc;   /* the instruction that faulted, | kHookAppCode */
    unsigned long addr; /* fault address */
    unsigned long r24;
    unsigned long lr;
    unsigned long zone; /* ApplZone at the fault: the current application */
    unsigned long r4;   /* h68k: usually the value stored */
    unsigned long sp;
} HookEvent;

/* The last write fault a hook passed on (P_ offsets). */
typedef struct {
    unsigned long pc, addr, r24, zone, sp;
} HookPassed;

/* Must match the D_ offsets in h68k.s and hnat.s. */
typedef struct {
    unsigned long magic;
    unsigned long count_all;     /* exceptions that entered the hook */
    unsigned long count_handled; /* recorded in the ring */
    unsigned long lo, hi;        /* pages to manage; hi = 0 for none */
    unsigned long area;
    unsigned long orig;          /* the handler the hook replaced */
    unsigned long kstatus;       /* last MPSetAreaAccess status */
    unsigned long ring_idx;      /* next slot to write */
    unsigned long spill[5];      /* CR, r0, r5, r6, r7 */
    unsigned long ctx[6];        /* context block words around the call */
    unsigned long count_passed;  /* write faults not handled */
    HookPassed passed;
    /* Page bitmap over [lo, hi), one bit per 4 KB page, most significant
       bit of each byte first; 0 means every page in the range. */
    const unsigned char *bitmap;
    unsigned long owner;         /* for host tools: the installer's state */
    unsigned long cur_lo, cur_hi;
    HookEvent *ring;             /* kHookRing entries, or 0 */
    unsigned long count_self;    /* handled inside [cur_lo, cur_hi) */
    unsigned long saved_ctx;     /* hnat: the context block, during the call */
    unsigned long block;         /* hnat: the system-heap block it lives in */
    unsigned long deny_from;     /* deny app-code stores at or above this */
    unsigned long count_denied;
    HookPassed denied;           /* the last store denied */
    volatile unsigned long *stop; /* gets ApplZone on a denial, or 0 */
    unsigned long frame;         /* h68k: the emulator's frame code, or 0 */
    /* The page the kernel's owner check denied last (common/kcb.h), or 0.
       A recorded store to that page is denied whatever its address, and
       the word cleared. */
    volatile unsigned long *kdeny;
    unsigned long zone;          /* h68k: ApplZone at the fault it handles */
    unsigned long reserved[3];
} HookData;

_Static_assert(sizeof(HookEvent) == 32, "ring entry size");
_Static_assert(offsetof(HookData, count_passed) == 0x50, "D_COUNT_PASSED");
_Static_assert(offsetof(HookData, passed) == 0x54, "D_PASSED");
_Static_assert(offsetof(HookData, bitmap) == 0x68, "D_BITMAP");
_Static_assert(offsetof(HookData, cur_lo) == 0x70, "D_CUR_LO");
_Static_assert(offsetof(HookData, ring) == 0x78, "D_RING");
_Static_assert(offsetof(HookData, count_self) == 0x7C, "D_COUNT_SELF");
_Static_assert(offsetof(HookData, saved_ctx) == 0x80, "hnat D_CTX");
_Static_assert(offsetof(HookData, deny_from) == 0x88, "D_DENY_FROM");
_Static_assert(offsetof(HookData, count_denied) == 0x8C, "D_COUNT_DENIED");
_Static_assert(offsetof(HookData, denied) == 0x90, "D_DENIED");
_Static_assert(offsetof(HookData, stop) == 0xA4, "D_STOP");
_Static_assert(offsetof(HookData, frame) == 0xA8, "D_FRAME");
_Static_assert(offsetof(HookData, kdeny) == 0xAC, "D_KDENY");
_Static_assert(offsetof(HookData, zone) == 0xB0, "D_ZONE");
_Static_assert(sizeof(HookData) == 0xC0, "data block size");

/* A pointer to a fixed address. The empty asm hides the constant from GCC,
   which otherwise warns that the address is outside any object. */
static inline volatile void *hook_ptr(unsigned long addr)
{
    __asm__("" : "+r"(addr));
    return (volatile void *)addr;
}

/* Write newly copied code back from the data cache and drop stale
   instructions (32-byte cache lines). */
static inline void hook_flush(unsigned long start, unsigned long len)
{
    unsigned long a;
    for (a = start & ~31UL; a < start + len; a += 32)
        __asm__ volatile("dcbst 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync" : : : "memory");
    for (a = start & ~31UL; a < start + len; a += 32)
        __asm__ volatile("icbi 0,%0" : : "r"(a) : "memory");
    __asm__ volatile("sync\n\tisync" : : : "memory");
}

static inline void hook_set_range(volatile HookData *d, unsigned long lo,
                                  unsigned long hi)
{
    d->hi = 0;
    d->lo = lo;
    d->hi = hi;
}

/* Manage only pages whose bit is set in bitmap, which covers [lo, hi) and
   must stay allocated while installed. */
static inline void hook_set_bitmap(volatile HookData *d, unsigned long lo,
                                   unsigned long hi,
                                   const unsigned char *bitmap)
{
    d->hi = 0;
    d->lo = lo;
    d->bitmap = bitmap;
    d->hi = hi;
}

static inline void hook_set_current(volatile HookData *d, unsigned long lo,
                                    unsigned long hi)
{
    d->cur_hi = 0;
    d->cur_lo = lo;
    d->cur_hi = hi;
}

static inline int hook_bit(const unsigned char *bitmap, unsigned long lo,
                           unsigned long addr)
{
    unsigned long page = (addr - lo) >> 12;
    return (bitmap[page >> 3] >> (7 - (page & 7))) & 1;
}

/* For h68k events: the address of the 68k instruction after the faulting
   store. The store ends there; its start depends on its length. */
static inline unsigned long hook_next_pc(unsigned long r24)
{
    return r24 - 2;
}

#endif
