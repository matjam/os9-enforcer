/*
 * Scribble: write into the Finder's memory, to test Guard (xlog/).
 *
 * Each test stores into a word in the middle of the Finder's partition the
 * value that is already there, so nothing changes if the store is allowed.
 *   n  a store from this application's native code: Guard denies it;
 *   6  a store from 68k code in this application's heap: Guard denies it;
 *   b  BlockMoveData, system code, called by this application: allowed
 *      (Guard does not look at who called the code doing the store);
 *   o  a store into this application's own heap: allowed.
 * A denied store stops this application; run it again for the next test.
 *
 *   t  time traps: for 2 seconds, rewrite one word on each of kBenchPages
 *      pages of the Finder's partition with BlockMoveData, with Enforcer's
 *      protection off and then on. With it on, the first write to each page
 *      after Enforcer protects it again (every 16 ms) is a trap. The extra
 *      time divided by the traps is what one trap costs, including putting
 *      the protection back. Needs the Enforcer Extension running.
 */

#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <MixedMode.h>
#include <Processes.h>
#include <Events.h>
#include <Gestalt.h>
#include <Timer.h>

#include "engine.h"

/* In InterfaceLib, not in the Multiversal Interfaces. */
typedef struct {
    unsigned long hi, lo;
} MicroTime;
extern pascal void Microseconds(MicroTime *t);

enum {
    kBenchPages = 32,     /* under the native hook's ring of 64 per tick */
    kBenchMicros = 2000000,
    kHnatSlot = 0x68FFF44C /* common/hnat.h */
};

/* store(addr, value), 68k, C calling convention. */
static const unsigned short code68k[] = {
    0x206F, 0x0004, /* movea.l 4(sp),a0 */
    0x20AF, 0x0008, /* move.l 8(sp),(a0) */
    0x4E75          /* rts */
};

static int find_finder(unsigned long *loc, unsigned long *size)
{
    ProcessSerialNumber psn = {0, kNoProcess};
    ProcessInfoRec info;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof info);
        info.processInfoLength = sizeof info;
        if (GetProcessInformation(&psn, &info) == noErr &&
            info.processSignature == 'MACS') {
            *loc = (unsigned long)info.processLocation;
            *size = info.processSize;
            return 1;
        }
    }
    return 0;
}

/* The native hook's data block: the 0xC0 bytes before the code that the
   native context block's handler word points at (as common/hnat.h). */
static volatile HookData *hnat_data_ptr(void)
{
    volatile unsigned long *slot = hook_ptr(kHnatSlot);
    unsigned long code = *slot;
    if (code >= 0x40000000 || code < sizeof(HookData))
        return 0;
    volatile HookData *d = (volatile HookData *)(code - sizeof(HookData));
    return (d->magic & 0xFFFFFF00) == ('HNA0' & 0xFFFFFF00) ? d : 0;
}

/* The native hook's count of exceptions it has seen (HookData.count_all),
   or 0 if it is not installed. */
static unsigned long hnat_count(void)
{
    volatile HookData *d = hnat_data_ptr();
    return d ? d->count_all : 0;
}

static unsigned long long micros(void)
{
    MicroTime w;
    Microseconds(&w);
    return (unsigned long long)w.hi << 32 | w.lo;
}

/* Ask the engine for a mode and wait for it to take effect. */
static int set_mode(State *s, unsigned long mode)
{
    s->want_mode = mode;
    unsigned long start = TickCount();
    while (s->mode != mode) {
        if (TickCount() - start > 300)
            return 0;
        EventRecord ev;
        WaitNextEvent(0, &ev, 5, NULL); /* let the extension run */
    }
    return 1;
}

typedef struct {
    unsigned long sweeps, traps;
    unsigned long long us;
} BenchResult;

static void sweep_for(volatile unsigned long **pages, BenchResult *r)
{
    unsigned long c0 = hnat_count(), sweeps = 0;
    unsigned long long t0 = micros(), t;
    do {
        for (int i = 0; i < kBenchPages; i++) {
            unsigned long v = *pages[i];
            BlockMoveData(&v, (Ptr)pages[i], sizeof v);
        }
        sweeps++;
        t = micros();
    } while (t - t0 < kBenchMicros);
    r->sweeps = sweeps;
    r->us = t - t0;
    r->traps = hnat_count() - c0;
}

static void bench(unsigned long loc, unsigned long size)
{
    long value = 0;
    State *s = 0;
    if (Gestalt(kEngineGestalt, &value) == noErr && value >= 0x1000)
        s = (State *)value;
    if (!s || s->magic != kEngineMagic || !s->running) {
        printf("Enforcer is not running\n");
        return;
    }
    /* Pages in the middle of the Finder's protected pages. */
    unsigned long lo = (loc + 0xFFF) & ~0xFFFUL;
    unsigned long hi = (loc + size) & ~0xFFFUL;
    if (hi - lo < (kBenchPages + 32) * 0x1000UL) {
        printf("the Finder's partition is too small\n");
        return;
    }
    volatile unsigned long *pages[kBenchPages];
    for (int i = 0; i < kBenchPages; i++)
        pages[i] = (volatile unsigned long *)(lo + (16 + i) * 0x1000UL + 0x200);

    unsigned long was = s->mode;
    BenchResult off, on;
    if (!set_mode(s, kModeOff)) {
        printf("Enforcer did not turn protection off\n");
        return;
    }
    sweep_for(pages, &off);
    if (!set_mode(s, kModeEnforce)) {
        printf("Enforcer did not turn protection on\n");
        set_mode(s, was);
        return;
    }
    sweep_for(pages, &on);
    set_mode(s, was);

    double per_off = (double)off.us / off.sweeps;
    double extra = on.us - on.sweeps * per_off;
    /* Retro68's printf has no %llu; two seconds fit in an unsigned long. */
    printf("off: %lu sweeps of %d pages in %lu us (%.2f us a sweep), "
           "%lu traps\n", off.sweeps, kBenchPages, (unsigned long)off.us,
           per_off, off.traps);
    printf("on:  %lu sweeps in %lu us (%.2f us a sweep), %lu traps\n",
           on.sweeps, (unsigned long)on.us, (double)on.us / on.sweeps,
           on.traps);
    if (on.traps)
        printf("extra time %.0f us, %.1f us a trap; %.0f traps a second\n",
               extra, extra / on.traps, on.traps * 1e6 / on.us);
}

int main(void)
{
    unsigned long loc, size;
    printf("Scribble: write into the Finder's memory\n");
    if (!find_finder(&loc, &size)) {
        printf("Finder not found\n");
        getchar();
        return 1;
    }
    volatile unsigned long *target =
        (volatile unsigned long *)(((loc + size / 2) & ~0xFFFUL) + 0x100);
    Ptr own = NewPtrClear(16);
    Ptr c = NewPtr(sizeof code68k);
    if (!own || !c) {
        printf("out of memory\n");
        getchar();
        return 1;
    }
    BlockMoveData(code68k, c, sizeof code68k);
    ProcInfoType pi = kCStackBased |
                      STACK_ROUTINE_PARAMETER(1, kFourByteCode) |
                      STACK_ROUTINE_PARAMETER(2, kFourByteCode);
    UniversalProcPtr upp = NewRoutineDescriptor((ProcPtr)c, pi, kM68kISA);

    printf("Finder partition %08lX+%08lX, target %08lX\n", loc, size,
           (unsigned long)target);
    /* A PowerPC function pointer points at a transition vector, whose
       first word is the code address. */
    printf("main's code %08lX, 68k code %08lX\n",
           *(unsigned long *)(void *)&main, (unsigned long)c);
    for (;;) {
        printf("\nn native store, 6 68k store, b BlockMoveData, o own "
               "heap, t time traps, q quit; then Return\n");
        int ch = getchar();
        while (ch != '\n' && getchar() != '\n')
            ;
        unsigned long v = *target;
        switch (ch) {
        case 'n':
            printf("native store of %08lX to %08lX\n", v, (unsigned long)target);
            *target = v;
            break;
        case '6':
            printf("68k store of %08lX to %08lX\n", v, (unsigned long)target);
            CallUniversalProc(upp, pi, (long)target, (long)v);
            break;
        case 'b':
            printf("BlockMoveData of %08lX to %08lX\n", v, (unsigned long)target);
            BlockMoveData(&v, (Ptr)target, sizeof v);
            break;
        case 'o':
            printf("store into own heap at %08lX\n", (unsigned long)own);
            *(volatile unsigned long *)own = v;
            break;
        case 't':
            bench(loc, size);
            continue;
        case 'q':
            return 0;
        default:
            continue;
        }
        printf("the store landed (not denied)\n");
    }
}
