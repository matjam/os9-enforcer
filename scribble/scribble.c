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
static unsigned long hnat_count(void)
{
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
    printf("chunks %d; taken %lu, passed %lu, disposed %lu, made %lu, "
           "freed %lu, dropped %lu, quits %lu, gone %lu\n",
           engine_live_chunks(s), s->pc.news, s->pc.passed, s->pc.disposed,
           s->pc.chunks_made, s->pc.chunks_freed, s->pc.chunks_dropped,
           s->pc.quits, s->pc.gone);
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
    int tf_armed = 0;
    for (;;) {
        printf("\nn native store, 6 68k store, b BlockMoveData, o own "
               "heap, s/S native/68k store into the system heap, t time "
               "traps, m temporary memory, M 1 MB of it, T store into "
               "another application's, P page 600 MB, Y from an MP task, "
               "V from a Time Manager task, D from a deferred task, W "
               "right after a switch, X make the Enforcer Extension quit "
               "as in a crash, q quit; then Return\n");
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
        case 'q':
            return 0;
        default:
            continue;
        }
        printf("the store landed (not denied)\n");
    }
}
