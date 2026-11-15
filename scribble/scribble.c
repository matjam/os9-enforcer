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
 *   s  a store from this application's native code into a word in the
 *      middle of the system heap: SysLog and SysGuard log it and allow it
 *      (the system heap is log only);
 *   S  the same from 68k code.
 * A denied store stops this application; run it again for the next test.
 *
 *   t  time traps: for 2 seconds, rewrite one word on each of kBenchPages
 *      pages of the Finder's partition with BlockMoveData, with Enforcer's
 *      protection off and then on. With it on, the first write to each page
 *      after Enforcer protects it again (every 16 ms) is a trap. The extra
 *      time divided by the traps is what one trap costs, including putting
 *      the protection back. Needs the Enforcer Extension running.
 *
 *   m  allocate 64 KB of temporary memory filled with 'SCRB' and keep it,
 *      printing its handle and whether it is in one of Enforcer's chunks
 *      (README, step 4);
 *   M  allocate 1 MB of temporary memory and dispose of it: its chunk is
 *      freed;
 *   L  allocate 64 MB of temporary memory, write every page and keep it:
 *      for measuring what a switch costs with a large application;
 *   R  after a switch out and back, time one write into each page of L's
 *      block (each one faults: the engine protected them at the switch),
 *      then the same writes again;
 *   T  a native store into another application's temporary memory chunk
 *      (the Finder's if it has one): Enforcer denies it;
 *   P  write and read back up to 600 MB of temporary memory, then dispose
 *      of it: with virtual memory on, other applications' pages are paged
 *      out and in.
 *
 * Stores by code of an application that is not current, which the
 * patched NanoKernel drops in enforce mode (README, Known holes). Each one
 * stores the target word's value with its bits flipped, reads the word
 * back, and stores the old value again: the word read back tells whether
 * the store was dropped.
 *   Y  from an MP task, into the Finder's memory (this application is
 *      current);
 *   V  from a Time Manager task 5 seconds later, into the memory of a
 *      background-only application; make the Finder current in those 5
 *      seconds. The result is printed with the next command.
 *
 * A store from a deferred task, which runs above task level while this
 * application is current: Enforcer's fault handler skips it instead of
 * stopping this application there (README, step 10).
 *   D  a Time Manager task half a second later installs a deferred task,
 *      which stores into the Finder's memory.
 *
 *   X  send the Enforcer Extension the Apple event that makes it call
 *      ExitToShell without stopping the engine, as a crash ends it. The
 *      engine must stop itself; the Mac must keep running.
 *
 *   W  a native store into the Finder's memory right after a switch back
 *      to this application, while the engine still has another process as
 *      current (it sees switches on its tick): the patched kernel finds
 *      out who is current itself, so Enforcer stops this application.
 *
 *   K  native and 68k stores of flipped values into a word of Enforcer's
 *      control block and one of its owner table, each read back: the
 *      kernel drops all four and counts them, and this application goes
 *      on.
 *
 *   Z  run about 5 s in a loop of this application's own code that never
 *      calls WaitNextEvent, twice the same work. With preemption (kernel
 *      patch 6) the loop sees gaps while other applications run, and the
 *      two runs' sums are the same.
 *   C  the same loop for 2.5 s in a callback that Mixed Mode calls from
 *      native code (CallUniversalProc);
 *   U  and in one called from 68k code in this application's heap. A call
 *      is in progress below both: preemption must leave them alone.
 *   H  U for 20 s. Command-Option-Escape during it must bring up Force
 *      Quit, and its buttons must work before the 20 s are up: the
 *      Process Manager runs the dialog during the hang by itself (slowly
 *      under QEMU).
 *
 *   G  TempNewHandle in system mode (BeginSystemMode), as system software
 *      makes memory that outlives the current application: the handle
 *      must be the Process Manager's, not in one of this application's
 *      chunks (issue 1).
 *   A  draw an Appearance scroll bar with a thumb. The first thumb of a
 *      boot makes the theme keep its image in temporary memory, made in
 *      system mode; quit, launch again and A: the thumb must be clean.
 *   O  overflow: 8 MB of handles and pointers in this 1 MB partition;
 *      what the partition cannot hold must come from chunks.
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
#include <AppleEvents.h>

#include "engine.h"
#include "hyield.h"

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

/* Multiprocessing Services (MPLibrary), not in the Multiversal
   Interfaces. */
typedef struct OpaqueMPTaskID *MPTaskID;
typedef struct OpaqueMPQueueID *MPQueueID;
typedef OSStatus (*MPTaskProc)(void *parameter);
extern OSStatus MPCreateQueue(MPQueueID *queue);
extern OSStatus MPDeleteQueue(MPQueueID queue);
extern OSStatus MPCreateTask(MPTaskProc entry, void *parameter,
                             unsigned long stack_size, MPQueueID notify,
                             void *term1, void *term2, unsigned long options,
                             MPTaskID *task);
extern OSStatus MPWaitOnQueue(MPQueueID queue, void **p1, void **p2,
                              void **p3, long timeout);
enum { kMPForever = 0x7FFFFFFF };

/* The flip-and-restore store of the Y and V tests. */
typedef struct {
    volatile unsigned long *target;
    volatile unsigned long old, seen, done;
} FlipTest;

static void flip(FlipTest *f)
{
    unsigned long v = *f->target;
    f->old = v;
    *f->target = v ^ 0x5A5A5A5A;
    f->seen = *f->target;
    *f->target = v;
    f->done = 1;
}

static void flip_report(const char *who, FlipTest *f)
{
    printf("%s: %08lX at %08lX read back as %08lX: %s\n", who, f->old,
           (unsigned long)f->target, f->seen,
           f->seen == f->old ? "the store was dropped" : "the store landed");
}

static OSStatus mp_flip(void *parameter)
{
    flip((FlipTest *)parameter);
    return 0;
}

typedef struct {
    TMTask tm;
    FlipTest f;
} TimerFlip;

static void tm_flip(TMTask *tm)
{
    flip(&((TimerFlip *)tm)->f);
}

enum { kTimerInfo = 0x0000B802 }; /* register-based, A1 = task */

/* The Deferred Task Manager (InterfaceLib), not in the Multiversal
   Interfaces. */
typedef struct DeferredTask {
    struct DeferredTask *qLink;
    short qType;
    short dtFlags;
    UniversalProcPtr dtAddr;
    long dtParam;
    long dtReserved;
} DeferredTask;
extern pascal OSErr DTInstall(DeferredTask *dt);
enum {
    kDTQType = 7,
    kDTInfo = 0x0000B802 /* register-based, A1 = dtParam */
};

/* The D test: a Time Manager task installs a deferred task, which does
   the flip. */
typedef struct {
    TMTask tm;
    DeferredTask dt;
    FlipTest f;
} DeferFlip;

static void dt_flip(long param)
{
    flip((FlipTest *)param);
}

static void tm_defer(TMTask *tm)
{
    DTInstall(&((DeferFlip *)tm)->dt);
}

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
static State *engine_state(void);

/* Write faults taken so far: by the kernel's owner check when its block is
   registered (it decides first, and the hooks then see none), else by the
   native hook. */
static unsigned long hnat_count(void)
{
    State *s = engine_state();
    if (s && s->kcb)
        return s->kcb->allowed + s->kcb->logged + s->kcb->self;
    volatile HookData *d = hnat_data_ptr();
    return d ? d->count_all : 0;
}

/* A long at a 2-byte aligned address. */
static unsigned long long_at(unsigned long addr)
{
    volatile unsigned short *p = hook_ptr(addr);
    return ((unsigned long)p[0] << 16) | p[1];
}

/* Enforcer's state, or 0 when it is not running. */
static State *engine_state(void)
{
    long value = 0;
    State *s = 0;
    if (Gestalt(kEngineGestalt, &value) == noErr && value >= 0x1000)
        s = (State *)value;
    return s && s->magic == kEngineMagic && s->running ? s : 0;
}

/* A word in the middle of the partition of a background-only application
   that Enforcer manages (not this one, not the Finder), or 0. */
static volatile unsigned long *background_target(char *name)
{
    State *s = engine_state();
    if (!s)
        return 0;
    unsigned long me = long_at(0x2AA); /* ApplZone */
    for (int i = 0; i < kMaxDomains; i++) {
        const Domain *d = &s->domains[i];
        if (!d->live || d->dying || i == s->sys_dom ||
            (me >= d->location && me - d->location < d->size) ||
            !strcmp(d->name, "Finder") || d->hi - d->lo < 0x2000)
            continue;
        ProcessInfoRec info;
        memset(&info, 0, sizeof info);
        info.processInfoLength = sizeof info;
        ProcessSerialNumber psn = d->psn;
        if (GetProcessInformation(&psn, &info) ||
            !(info.processMode & modeOnlyBackground))
            continue;
        strcpy(name, d->name);
        return (volatile unsigned long *)((((d->lo + d->hi) / 2) &
                                           ~0xFFFUL) + 0x100);
    }
    return 0;
}

/* A word in the middle of the system heap: from SysZone to its bkLim. */
static volatile unsigned long *sys_target(void)
{
    unsigned long zone = long_at(0x2A6), end = long_at(zone);
    return (volatile unsigned long *)(((zone + end) / 2 & ~0xFFFUL) + 0x100);
}

static unsigned long long micros(void)
{
    MicroTime w;
    Microseconds(&w);
    return (unsigned long long)w.hi << 32 | w.lo;
}

static unsigned long tb(void)
{
    unsigned long t;
    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}

/* Z's loop: iters rounds of integer and floating-point work in registers,
   with no call, timing each round with the timebase. A round that took
   more than gap ticks is a time the loop did not run. */
typedef struct {
    unsigned long isum, gaps, max_gap, ticks;
    double fsum;
} Spin;

static void __attribute__((noinline)) spin(unsigned long iters,
                                           unsigned long gap, Spin *r)
{
    unsigned long a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8;
    double x = 1.0, y = 0.5, z = 0.25, w = 2.0;
    unsigned long start = tb(), prev = start, gaps = 0, max_gap = 0;
    for (unsigned long i = 0; i < iters; i++) {
        for (int k = 0; k < 64; k++) {
            a = a * 1664525 + 1013904223;
            b ^= a >> 7;
            c += b;
            d = (d << 3) ^ c;
            e += d >> 5;
            f ^= e;
            g += f * 3;
            h ^= g >> 11;
            x = x * 1.0000001 + y;
            y = y * 0.9999999 - z * 1e-9;
            z += x * 1e-12;
            w = w * 0.5 + z;
        }
        unsigned long now = tb();
        if (now - prev > gap) {
            gaps++;
            if (now - prev > max_gap)
                max_gap = now - prev;
        }
        prev = now;
    }
    r->isum = a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
    r->fsum = x + y + z + w;
    r->gaps = gaps;
    r->max_gap = max_gap;
    r->ticks = prev - start;
}

/* Timebase ticks a millisecond, and spin's rounds for ms milliseconds. */
static unsigned long spin_rounds(unsigned long ms, unsigned long *per_ms)
{
    unsigned long long u0 = micros();
    unsigned long t0 = tb();
    while (micros() - u0 < 50000)
        ;
    *per_ms = (tb() - t0) / 50;
    Spin cal;
    spin(200, ~0UL, &cal);
    return (unsigned long)(200.0 * ms * *per_ms / (cal.ticks + 1));
}

/* C and U: spin called back from system code. */
static Spin g_cb;
static unsigned long g_cb_rounds, g_cb_gap;

static void cb_spin(void)
{
    spin(g_cb_rounds, g_cb_gap, &g_cb);
}

/* MOVEA.L 4(A7),A0; JSR (A0); RTS: calls the routine descriptor passed
   to it, from 68k code. */
static const unsigned short code68k_call[] = {0x206F, 0x0004, 0x4E90,
                                              0x4E75};

/* Process Manager calls that InterfaceLib does not export (MFPrivate), as
   68k code: _OSDispatch with the selector passed to it and no other
   parameter, and GetSystemClientProcess (selector $3F) with the address of
   a ProcessSerialNumber. Both return the OSErr. */
static const unsigned short code68k_osd[] = {
    0x4267,         /* CLR.W -(A7): room for the OSErr */
    0x3F2F, 0x0008, /* MOVE.W 8(A7),-(A7): the selector */
    0xA88F,         /* _OSDispatch */
    0x301F, 0x48C0, /* MOVE.W (A7)+,D0; EXT.L D0 */
    0x4E75};        /* RTS */
static const unsigned short code68k_client[] = {
    0x4267,         /* CLR.W -(A7) */
    0x2F2F, 0x0006, /* MOVE.L 6(A7),-(A7): the ProcessSerialNumber */
    0x3F3C, 0x003F, /* MOVE.W #$3F,-(A7) */
    0xA88F,         /* _OSDispatch */
    0x301F, 0x48C0, /* MOVE.W (A7)+,D0; EXT.L D0 */
    0x4E75};        /* RTS */
enum {
    kBeginSystemMode = 0x40,
    kEndSystemMode = 0x41,
    kCall68kInfo = 0xF1 /* long (long), C stack */
};

/* Call 68k code with one long parameter. */
static long call68k(const unsigned short *code, unsigned long size, long arg)
{
    Ptr p = NewPtr(size);
    if (!p)
        return memFullErr;
    BlockMove(code, p, size); /* flushes the code caches */
    UniversalProcPtr u =
        NewRoutineDescriptor((ProcPtr)p, kCall68kInfo, kM68kISA);
    long r = CallUniversalProc(u, kCall68kInfo, arg);
    DisposeRoutineDescriptor(u);
    DisposePtr(p);
    return r;
}

/* Whether handle h lives in one of Enforcer's temporary memory chunks:
   its zone follows the chunk signature (common/engine.h). */
static int in_chunk(Handle h)
{
    unsigned long z = (unsigned long)HandleZone(h);
    return z > 16 && long_at(z - 16) == kChunkSig && long_at(z - 12) == z;
}

static void pool_report(void)
{
    State *s = engine_state();
    if (!s) {
        printf("Enforcer is not running\n");
        return;
    }
    printf("chunks %d; taken %lu, passed %lu, passed in system mode %lu, "
           "disposed %lu, made %lu, freed %lu, dropped %lu, quits %lu, gone "
           "%lu\n",
           engine_live_chunks(s), s->pc.news, s->pc.passed, s->pc.system,
           s->pc.disposed, s->pc.chunks_made, s->pc.chunks_freed,
           s->pc.chunks_dropped, s->pc.quits, s->pc.gone);
}

/* A word in the middle of a temporary memory chunk of another
   application, the Finder's if it has one, or 0. */
static volatile unsigned long *chunk_target(char *name)
{
    State *s = engine_state();
    if (!s)
        return 0;
    unsigned long me = long_at(0x2AA); /* ApplZone */
    const Chunk *pick = 0;
    for (int i = 0; i < kMaxChunks; i++) {
        const Chunk *c = &s->chunks[i];
        const Domain *d = &s->domains[c->domain];
        if (!c->live || (me >= d->location && me - d->location < d->size))
            continue;
        if (!pick || !strcmp(d->name, "Finder"))
            pick = c;
    }
    if (!pick)
        return 0;
    strcpy(name, s->domains[pick->domain].name);
    return (volatile unsigned long *)
        ((((pick->lo + pick->hi) / 2) & ~0xFFFUL) + 0x100);
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
    volatile unsigned long *sys = sys_target();
    printf("system heap target %08lX\n", (unsigned long)sys);
    static TimerFlip tf;
    Handle big = 0; /* L's block */
    int tf_armed = 0;
    for (;;) {
        printf("\nn native store, 6 68k store, b BlockMoveData, o own "
               "heap, s/S native/68k store into the system heap, t time "
               "traps, m temporary memory, M 1 MB of it, L keep 64 MB of "
               "it, R time writing it after a switch, F time reading the "
               "Finder after a switch, T store into "
               "another application's, P page 600 MB, Y from an MP task, "
               "V from a Time Manager task, D from a deferred task, W "
               "right after a switch, K into the control block and the "
               "owner table, X make the Enforcer Extension quit "
               "as in a crash, Z run 5 s without yielding, C/U the same "
               "in a native/68k callback, H 20 s in a 68k callback, G "
               "temporary memory in system mode, A draw a scroll bar, O 8 MB in this 1 MB partition, q "
               "quit; then Return\n");
        int ch = getchar();
        while (ch != '\n' && getchar() != '\n')
            ;
        if (tf_armed && tf.f.done) {
            flip_report("Time Manager task", &tf.f);
            RmvTime((QElemPtr)&tf.tm);
            tf_armed = 0;
        }
        unsigned long v = *target;
        switch (ch) {
        case 'Y': {
            static FlipTest f;
            MPQueueID q;
            MPTaskID task;
            memset((void *)&f, 0, sizeof f);
            f.target = target;
            if (MPCreateQueue(&q) ||
                MPCreateTask(mp_flip, &f, 0, q, 0, 0, 0, &task)) {
                printf("could not start an MP task\n");
                continue;
            }
            MPWaitOnQueue(q, 0, 0, 0, kMPForever);
            MPDeleteQueue(q);
            flip_report("MP task, into the Finder", &f);
            continue;
        }
        case 'V': {
            char name[16];
            if (tf_armed) {
                printf("the Time Manager task is still waiting\n");
                continue;
            }
            volatile unsigned long *t = background_target(name);
            if (!t) {
                printf("no background-only application found\n");
                continue;
            }
            memset(&tf, 0, sizeof tf);
            tf.f.target = t;
            tf.tm.tmAddr = (ProcPtr)NewRoutineDescriptor(
                (ProcPtr)tm_flip, kTimerInfo, kPowerPCISA);
            InsTime((QElemPtr)&tf.tm);
            PrimeTime((QElemPtr)&tf.tm, 5000);
            tf_armed = 1;
            printf("in 5 seconds, a store into %s at %08lX: make the "
                   "Finder current now\n", name, (unsigned long)t);
            continue;
        }
        case 'W': {
            State *s = engine_state();
            if (!s) {
                printf("Enforcer is not running\n");
                continue;
            }
            unsigned long mine = (unsigned long)ApplicationZone();
            int tries = 0;
            while (tries < 1000) {
                tries++;
                EventRecord ev;
                WaitNextEvent(0, &ev, 1, NULL); /* let others run */
                unsigned long behind = s->cur_zone;
                if (behind != mine) {
                    unsigned long w = *target;
                    *target = w;
                    printf("stored %08lX to %08lX after a switch, try %d; "
                           "the engine had zone %08lX current\n", w,
                           (unsigned long)target, tries, behind);
                    break;
                }
            }
            if (tries == 1000)
                printf("the engine was never behind\n");
            continue;
        }
        case 'K': {
            State *s = engine_state();
            if (!s || !s->kcb) {
                printf("no control block registered\n");
                continue;
            }
            /* Words the kernel never reads: one in the block's reserved
               space, and the owner table's last, for pages at the top of
               logical memory that nobody owns. */
            volatile unsigned long *words[2] = {
                (volatile unsigned long *)&s->kcb->reserved,
                (volatile unsigned long *)(s->owners + s->owners_size - 4)};
            const char *names[2] = {"control block", "owner table"};
            unsigned long before = s->kcb->tables;
            for (int i = 0; i < 2; i++) {
                volatile unsigned long *w = words[i];
                unsigned long old = *w;
                *w = ~old;
                unsigned long native = *w;
                CallUniversalProc(upp, pi, (long)w, (long)~old);
                unsigned long m68k = *w;
                *w = old;
                printf("%s word %08lX: %08lX; after a native store of "
                       "%08lX: %08lX, after a 68k one: %08lX: %s\n",
                       names[i], (unsigned long)w, old, ~old, native, m68k,
                       native == old && m68k == old ? "dropped" : "WRITTEN");
            }
            printf("stores dropped into the tables: %lu (6 expected)\n",
                   s->kcb->tables - before);
            continue;
        }
        case 'X': {
            ProcessSerialNumber psn = {0, kNoProcess};
            ProcessInfoRec info;
            int found = 0;
            while (!found && GetNextProcess(&psn) == noErr) {
                memset(&info, 0, sizeof info);
                info.processInfoLength = sizeof info;
                found = GetProcessInformation(&psn, &info) == noErr &&
                        info.processSignature == 'EnfX';
            }
            if (!found) {
                printf("the Enforcer Extension is not running\n");
                continue;
            }
            AEAddressDesc to;
            AppleEvent ev, reply;
            OSErr err = AECreateDesc(typeProcessSerialNumber, &psn,
                                     sizeof psn, &to);
            if (!err) {
                err = AECreateAppleEvent('EnfX', 'exit', &to,
                                         kAutoGenerateReturnID,
                                         kAnyTransactionID, &ev);
                AEDisposeDesc(&to);
            }
            if (!err) {
                err = AESend(&ev, &reply, kAENoReply, kAENormalPriority,
                             kAEDefaultTimeout, NULL, NULL);
                AEDisposeDesc(&ev);
            }
            printf("exit event sent to the Enforcer Extension: error %d\n",
                   (int)err);
            continue;
        }
        case 'D': {
            static DeferFlip df;
            memset(&df, 0, sizeof df);
            df.f.target = target;
            df.dt.qType = kDTQType;
            df.dt.dtAddr = NewRoutineDescriptor((ProcPtr)dt_flip, kDTInfo,
                                                kPowerPCISA);
            df.dt.dtParam = (long)&df.f;
            df.tm.tmAddr = (ProcPtr)NewRoutineDescriptor(
                (ProcPtr)tm_defer, kTimerInfo, kPowerPCISA);
            InsTime((QElemPtr)&df.tm);
            PrimeTime((QElemPtr)&df.tm, 500);
            /* Wait without calling WaitNextEvent, so this application
               stays current when the deferred task runs. */
            unsigned long start = TickCount();
            while (!df.f.done && TickCount() - start < 180)
                ;
            RmvTime((QElemPtr)&df.tm);
            if (df.f.done)
                flip_report("Deferred task, into the Finder", &df.f);
            else
                printf("the deferred task did not run\n");
            continue;
        }
        case 's':
            v = *sys;
            printf("native store of %08lX to %08lX\n", v, (unsigned long)sys);
            *sys = v;
            break;
        case 'S':
            v = *sys;
            printf("68k store of %08lX to %08lX\n", v, (unsigned long)sys);
            CallUniversalProc(upp, pi, (long)sys, (long)v);
            break;
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
        case 'L': {
            /* 64 MB more temporary memory, every page written, kept: the
               engine then re-protects that many more pages each time this
               application is switched out. */
            OSErr err = 0;
            Handle h = TempNewHandle(64UL << 20, &err);
            if (!h) {
                printf("TempNewHandle of 64 MB failed: %d\n", err);
                continue;
            }
            HLock(h);
            for (unsigned long o = 0; o < (64UL << 20); o += 4096)
                (*h)[o] = 1;
            printf("64 MB of temporary memory at %08lX, kept%s\n",
                   (unsigned long)*h, in_chunk(h) ? ", in a chunk" : "");
            big = h;
            continue;
        }
        case 'R': {
            /* After a switch out and back, the engine has protected every
               page again: time one write into each page of L's block
               (each faults), then the same writes again (none do). */
            State *s = engine_state();
            if (!s || !big) {
                printf("needs Enforcer running and L first\n");
                continue;
            }
            unsigned long n = s->c.switches, mine =
                (unsigned long)ApplicationZone();
            EventRecord ev;
            int tries = 0;
            while ((s->c.switches == n || s->cur_zone != mine) &&
                   tries++ < 1000)
                WaitNextEvent(0, &ev, 1, NULL);
            volatile char *p = (volatile char *)*big;
            unsigned long pages = (64UL << 20) / 4096;
            unsigned long long t0 = micros();
            for (unsigned long i = 0; i < pages; i++)
                p[i * 4096] = 2;
            unsigned long long t1 = micros();
            for (unsigned long i = 0; i < pages; i++)
                p[i * 4096] = 3;
            unsigned long long t2 = micros();
            printf("%lu pages after a switch: first writes %lu us, again "
                   "%lu us: %.1f us a page in faults\n", pages,
                   (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
                   (double)((t1 - t0) - (t2 - t1)) / pages);
            continue;
        }
        case 'F': {
            /* After a switch out and back, read one word of each page of
               the Finder's partition, then again. With application VSIDs,
               the first read of a page whose PTE was built for the
               Finder's VSIDs builds it again for Scribble's (kernel patch
               5, CB_REBUILT): the difference is what a rebuild costs. */
            State *s = engine_state();
            if (!s) {
                printf("needs Enforcer running\n");
                continue;
            }
            unsigned long n = s->c.switches, mine =
                (unsigned long)ApplicationZone();
            EventRecord ev;
            int tries = 0;
            while ((s->c.switches == n || s->cur_zone != mine) &&
                   tries++ < 1000)
                WaitNextEvent(0, &ev, 1, NULL);
            volatile unsigned long *p =
                (volatile unsigned long *)((loc + 4095) & ~4095UL);
            unsigned long pages = (loc + size - (unsigned long)p) / 4096;
            unsigned long sum = 0;
            unsigned long r0 = s->kcb ? s->kcb->rebuilt : 0;
            unsigned long long t0 = micros();
            for (unsigned long i = 0; i < pages; i++)
                sum += p[i * 1024];
            unsigned long long t1 = micros();
            unsigned long r1 = s->kcb ? s->kcb->rebuilt : 0;
            for (unsigned long i = 0; i < pages; i++)
                sum += p[i * 1024];
            unsigned long long t2 = micros();
            unsigned long rebuilt = r1 - r0;
            printf("%lu Finder pages read after a switch: first %lu us, "
                   "again %lu us; %lu PTEs built again, %.1f us each "
                   "(sum %08lX)\n", pages, (unsigned long)(t1 - t0),
                   (unsigned long)(t2 - t1), rebuilt,
                   rebuilt ? (double)((t1 - t0) - (t2 - t1)) / rebuilt : 0.0,
                   sum);
            continue;
        }
        case 'Z': {
            /* Run about 5 s without calling WaitNextEvent: the same loop
               twice, 2.5 s each. With preemption (kernel patch 6), the
               yield stub calls WaitNextEvent for Scribble every slice:
               the loop sees gaps while other applications run, and the
               two runs' sums must be the same (every register came back
               as it was). */
            unsigned long per_ms;
            unsigned long iters = spin_rounds(2500, &per_ms);
            Spin r1, r2;
            State *s = engine_state();
            unsigned long p0 = s && s->kcb ? s->kcb->preempts : 0;
            volatile YieldData *y = s ? s->yield : 0;
            unsigned long y0 = y ? y->yielded : 0, e0 = y ? y->entered : 0;
            unsigned long w0 = s ? s->c.pm_switches : 0;
            spin(iters, per_ms, &r1);
            spin(iters, per_ms, &r2);
            printf("%lu rounds twice: %s; %lu ms and %lu ms\n", iters,
                   r1.isum == r2.isum && r1.fsum == r2.fsum
                       ? "sums equal" : "SUMS DIFFER",
                   r1.ticks / per_ms, r2.ticks / per_ms);
            printf("gaps over 1 ms: %lu and %lu, the longest %lu us and "
                   "%lu us\n", r1.gaps, r2.gaps,
                   r1.max_gap * 1000 / per_ms, r2.max_gap * 1000 / per_ms);
            if (s) {
                s = engine_state();
                printf("redirects %lu, stub entries %lu, WaitNextEvent "
                       "calls %lu, Process Manager switches %lu\n",
                       s && s->kcb ? s->kcb->preempts - p0 : 0,
                       y ? y->entered - e0 : 0, y ? y->yielded - y0 : 0,
                       s ? s->c.pm_switches - w0 : 0);
                if (y)
                    printf("checks: starting %lu, not task %lu, in a call "
                           "%lu, other stack %lu, low stack %lu, safe %lu\n",
                           y->starting, y->not_task, y->in_call,
                           y->other_stack, y->low_stack, y->safe);
            }
            continue;
        }
        case 'C':
        case 'U':
        case 'H': {
            /* Spin 2.5 s in a callback: called by Mixed Mode from native
               code (C), or from 68k code in this application's heap (U).
               A Toolbox call is in progress below it, so preemption must
               not call WaitNextEvent: every check says "in a call". H is
               U for 20 s, to try Force Quit (Command-Option-Escape). */
            unsigned long per_ms;
            g_cb_rounds = spin_rounds(ch == 'H' ? 20000 : 2500, &per_ms);
            g_cb_gap = per_ms;
            State *s = engine_state();
            volatile YieldData *y = s ? s->yield : 0;
            if (!y) {
                printf("needs Enforcer with preemption\n");
                continue;
            }
            unsigned long safe0 = y->safe, call0 = y->in_call,
                          w0 = y->yielded, e0 = y->entered,
                          b0 = y->broken;
            UniversalProcPtr cbu = NewRoutineDescriptor(
                (ProcPtr)cb_spin, kCStackBased, kPowerPCISA);
            if (ch == 'C') {
                CallUniversalProc(cbu, kCStackBased);
            } else {
                Ptr c68 = NewPtr(sizeof code68k_call);
                ProcInfoType pi1 = kCStackBased |
                                   STACK_ROUTINE_PARAMETER(1, kFourByteCode);
                BlockMove(code68k_call, c68, sizeof code68k_call);
                UniversalProcPtr u68 =
                    NewRoutineDescriptor((ProcPtr)c68, pi1, kM68kISA);
                CallUniversalProc(u68, pi1, cbu);
                DisposeRoutineDescriptor(u68);
                DisposePtr(c68);
            }
            DisposeRoutineDescriptor(cbu);
            printf("%s callback: %lu ms, gaps over 1 ms %lu, the longest "
                   "%lu us; stub entries %lu, in a call %lu, chain not "
                   "ending at the base %lu, safe %lu, WaitNextEvent calls "
                   "%lu\n",
                   ch == 'C' ? "native" : "68k", g_cb.ticks / per_ms,
                   g_cb.gaps, g_cb.max_gap * 1000 / per_ms, y->entered - e0,
                   y->in_call - call0, y->broken - b0, y->safe - safe0,
                   y->yielded - w0);
            printf("the last chain (sp %08lX, base %08lX, %lu frames):",
                   y->last_sp, y->last_base, y->last_n);
            for (unsigned long i = 0; i <= y->last_n && i < 11; i++)
                printf(" %08lX", y->chain[i]);
            printf("\n");
            continue;
        }
        case 'm': {
            /* For finding how the Process Manager records an
               application's temporary memory: a 64 KB block, kept, filled
               with 'SCRB', at a handle printed here. */
            OSErr err = 0;
            Handle h = TempNewHandle(0x10000, &err);
            if (!h) {
                printf("TempNewHandle failed: %d\n", err);
                continue;
            }
            for (unsigned long i = 0; i < 0x10000 / 4; i++)
                ((unsigned long *)*h)[i] = 'SCRB';
            printf("temporary memory: handle %08lX, block %08lX, %s\n",
                   (unsigned long)h, (unsigned long)*h,
                   in_chunk(h) ? "in a chunk" : "not in a chunk");
            pool_report();
            continue;
        }
        case 'G': {
            /* Temporary memory made in system mode (BeginSystemMode, as
               system software makes memory that must outlive the current
               application) must stay the Process Manager's, on its
               system list: not in one of this application's chunks,
               which are freed when it quits (issue 1). */
            OSErr err = 0, err2 = 0;
            ProcessSerialNumber in = {0, 0}, out = {0, 0};
            Handle own = TempNewHandle(0x1000, &err);
            long b = call68k(code68k_osd, sizeof code68k_osd,
                             kBeginSystemMode);
            long c = call68k(code68k_client, sizeof code68k_client,
                             (long)&in);
            Handle sys = TempNewHandle(0x1000, &err2);
            long e = call68k(code68k_osd, sizeof code68k_osd,
                             kEndSystemMode);
            long c2 = call68k(code68k_client, sizeof code68k_client,
                              (long)&out);
            printf("BeginSystemMode %ld, client %ld (%lu.%lu); "
                   "EndSystemMode %ld, client %ld (%lu.%lu)\n",
                   b, c, (unsigned long)in.highLongOfPSN,
                   (unsigned long)in.lowLongOfPSN, e, c2,
                   (unsigned long)out.highLongOfPSN,
                   (unsigned long)out.lowLongOfPSN);
            printf("own handle %08lX (%d): %s; system-mode handle %08lX "
                   "(%d): %s\n",
                   (unsigned long)own, err,
                   own && in_chunk(own) ? "in a chunk" : "not in a chunk",
                   (unsigned long)sys, err2,
                   sys && in_chunk(sys) ? "in a chunk" : "not in a chunk");
            int system = in.highLongOfPSN == 0 &&
                         in.lowLongOfPSN == kSystemProcess;
            if (!system)
                printf("SKIP: the Process Manager did not enter system "
                       "mode\n");
            else if (sys && !in_chunk(sys))
                printf("PASS: the system-mode handle is the Process "
                       "Manager's\n");
            else
                printf("FAIL: the system-mode handle is in a chunk, "
                       "freed when Scribble quits\n");
            if (own)
                DisposeHandle(own);
            if (sys)
                DisposeHandle(sys);
            pool_report();
            continue;
        }
        case 'A': {
            /* An Appearance scroll bar with a thumb, in a window of this
               application's, shown for 3 s. The first thumb drawn in a
               boot makes the theme keep its image in temporary memory,
               for every application after (issue 1). Prints the
               TempNewHandle calls taken into chunks, and passed on in
               system mode, while it draws. */
            static const unsigned char title[] = {8, 'S', 'c', 'r', 'i',
                                                  'b', 'b', 'l', 'e'};
            static const unsigned char none[] = {0};
            State *s = engine_state();
            unsigned long n0 = s ? s->pc.news : 0;
            unsigned long y0 = s ? s->pc.system : 0;
            Rect wr = {80, 80, 280, 200};
            WindowPtr w = NewCWindow(0, &wr, title, true, documentProc,
                                     (WindowPtr)-1, false, 0);
            if (!w) {
                printf("NewCWindow failed\n");
                continue;
            }
            GrafPtr old;
            GetPort(&old);
            SetPort(w);
            Rect cr = {-1, 105, 186, 121};
            ControlHandle c = NewControl(w, &cr, none, true, 30, 0, 100,
                                         scrollBarProc, 0);
            if (c)
                Draw1Control(c);
            s = engine_state();
            printf("scroll bar %s; TempNewHandle into chunks %lu, passed "
                   "on in system mode %lu\n",
                   c ? "drawn" : "not made", s ? s->pc.news - n0 : 0,
                   s ? s->pc.system - y0 : 0);
            unsigned long t = TickCount();
            while (TickCount() - t < 180)
                ;
            DisposeWindow(w);
            SetPort(old);
            continue;
        }
        case 'M': {
            /* A 1 MB block, which needs a chunk of its own, disposed at
               once: the chunk is freed. */
            OSErr err = 0;
            Handle h = TempNewHandle(0x100000, &err);
            if (!h) {
                printf("TempNewHandle failed: %d\n", err);
                continue;
            }
            printf("1 MB temporary memory: block %08lX, %s\n",
                   (unsigned long)*h,
                   in_chunk(h) ? "in a chunk" : "not in a chunk");
            pool_report();
            DisposeHandle(h);
            printf("disposed\n");
            pool_report();
            continue;
        }
        case 'P': {
            /* Paging: up to 600 MB of temporary memory in 8 MB blocks, a
               word written on every page, then all of it disposed. With
               virtual memory on and less physical memory than that, other
               applications' pages (protected ones too) go out to disk. */
            enum { kBlocks = 75, kBlockSize = 0x800000 };
            Handle hs[kBlocks];
            int n = 0;
            unsigned long t0 = TickCount();
            for (; n < kBlocks; n++) {
                OSErr err = 0;
                hs[n] = TempNewHandle(kBlockSize, &err);
                if (!hs[n])
                    break;
                HLock(hs[n]);
                volatile unsigned long *p = (volatile unsigned long *)*hs[n];
                for (unsigned long i = 0; i < kBlockSize / 4; i += 1024)
                    p[i] = i;
            }
            unsigned long bad = 0;
            for (int j = 0; j < n; j++) {
                volatile unsigned long *p = (volatile unsigned long *)*hs[j];
                for (unsigned long i = 0; i < kBlockSize / 4; i += 1024)
                    if (p[i] != i)
                        bad++;
            }
            printf("%d MB written and read back in %lu ticks, %lu wrong\n",
                   n * (kBlockSize >> 20), TickCount() - t0, bad);
            pool_report();
            for (int j = 0; j < n; j++)
                DisposeHandle(hs[j]);
            printf("disposed\n");
            pool_report();
            continue;
        }
        case 'T': {
            char name[16];
            volatile unsigned long *t = chunk_target(name);
            if (!t) {
                printf("no other application's chunk\n");
                continue;
            }
            v = *t;
            printf("native store of %08lX to %08lX, in %s's temporary "
                   "memory\n", v, (unsigned long)t, name);
            *t = v;
            break;
        }
        case 'O': {
            /* Overflow: ask for 6 MB of handles and 2 MB of pointers in
               256 KB blocks, far more than this 1 MB partition holds.
               Each request the partition cannot take must come from a
               chunk; every block is written, read back and disposed, and
               the chunks are freed with their last block. */
            enum { kN = 32, kBlock = 0x40000 };
            static Handle hs[kN];
            static Ptr ps[kN];
            State *s = engine_state();
            int chunks0 = s ? engine_live_chunks(s) : 0;
            int got = 0, over = 0, bad = 0, dirty = 0;
            for (int i = 0; i < kN; i++) {
                int ptr = i >= 24, clear = i & 1;
                unsigned long *b;
                if (ptr) {
                    ps[i] = clear ? NewPtrClear(kBlock) : NewPtr(kBlock);
                    b = (unsigned long *)ps[i];
                } else {
                    hs[i] = clear ? NewHandleClear(kBlock) : NewHandle(kBlock);
                    b = hs[i] ? (unsigned long *)*hs[i] : 0;
                }
                if (!b) {
                    printf("block %d: error %d\n", i, MemError());
                    continue;
                }
                got++;
                unsigned long z = ptr ? (unsigned long)PtrZone(ps[i])
                                      : (unsigned long)HandleZone(hs[i]);
                if (z > 16 && long_at(z - 16) == kChunkSig &&
                    long_at(z - 12) == z)
                    over++;
                if (!ptr)
                    HLock(hs[i]);
                for (unsigned long k = 0; k < kBlock / 4; k++) {
                    if (clear && b[k])
                        dirty++;
                    b[k] = 0x4F000000UL | (unsigned long)i << 16 | (k & 0xFFFF);
                }
            }
            for (int i = 0; i < kN; i++) {
                unsigned long *b = i >= 24 ? (unsigned long *)ps[i]
                                   : hs[i] ? (unsigned long *)*hs[i] : 0;
                if (!b)
                    continue;
                for (unsigned long k = 0; k < kBlock / 4; k++)
                    if (b[k] !=
                        (0x4F000000UL | (unsigned long)i << 16 | (k & 0xFFFF)))
                        bad++;
            }
            int chunks1 = s ? engine_live_chunks(s) : 0;
            for (int i = 0; i < kN; i++) {
                if (i >= 24 && ps[i])
                    DisposePtr(ps[i]);
                else if (i < 24 && hs[i])
                    DisposeHandle(hs[i]);
                hs[i] = 0;
                ps[i] = 0;
            }
            int chunks2 = s ? engine_live_chunks(s) : 0;
            Handle after = NewHandle(0x8000);
            int home = after && !in_chunk(after);
            if (after)
                DisposeHandle(after);
            printf("%d of %d blocks, %d in chunks; %d words wrong, %d not "
                   "cleared; chunks %d, then %d, then %d after disposing; a "
                   "32 KB handle afterwards is %s\n",
                   got, kN, over, bad, dirty, chunks0, chunks1, chunks2,
                   home ? "in the partition" : "NOT in the partition");
            if (s)
                printf("overflow: handles %lu, pointers %lu, not served "
                       "%lu\n", s->pc.over_handles, s->pc.over_ptrs,
                       s->pc.over_none);
            printf("%s\n", got == kN && over > 0 && !bad && !dirty && home &&
                               chunks2 <= chunks0 + 1
                               ? "PASS" : "FAIL");
            continue;
        }
        case 'q':
            return 0;
        default:
            continue;
        }
        printf("the store landed (not denied)\n");
    }
}
