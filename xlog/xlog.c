/*
 * XLog: log writes from one application into another's memory, across all
 * running applications. Log only: every write is allowed.
 *
 * Guard: the same program built with XLOG_ENFORCE=1. It denies a store into
 * another application's memory when the code doing it is the current
 * application's own (it lies in the current application's domain). The
 * hooks leave the page protected and pass the fault on, and the system
 * stops the application. Stores made by system code (the Process Manager,
 * the Window Manager and others, which live outside every domain) are
 * still allowed and logged. XLog marks the stores Guard would deny.
 *
 * Each running application's partition (from the Process Manager) is a
 * domain. Its whole pages ([lo, hi), the partition rounded inward to pages)
 * are write-protected while the application is not current, and their bits
 * are set in a page bitmap over the blue area. Both write hooks manage
 * exactly the pages whose bit is set:
 *   - 68k code: common/h68k.s, in front of the emulator;
 *   - native code: common/hnat.s, in front of the Exception Manager.
 * A hook opens the faulting page, retries the store and records it in its
 * ring, unless the address is in the hooks' current range (the current
 * application's domain), where it only counts it.
 *
 * A 16 ms Time Manager task:
 *   - notices a process switch (ApplZone changes), points both hooks'
 *     current range at the new application's domain, and protects all of
 *     the previous application's domain again;
 *   - takes the rings. A store is the target's own when ApplZone at the
 *     store is inside the target's partition, and cross-domain otherwise:
 *     logged by writer (PC and LR, or the 68k PC; application) and target.
 *     The page is protected again unless it is the current application's.
 * If a ring overflowed between ticks, every domain but the current one is
 * protected again.
 *
 * A write that reaches the Exception Manager without going through the
 * native hook (from another context) goes to a fallback handler in the
 * Exception Manager's system-wide slot. It opens the page and records the
 * store in a third ring.
 *
 * Safety rule: a protected page always has its bit set in the bitmap and a
 * live domain behind it; otherwise a fault on it kills the application or
 * hangs the machine. Domains are added as live, then bits, then protect;
 * removed as dying, unprotect, then bits, then dead.
 *
 * XLog's own partition is not a domain. Run it only in a SCRATCH=1 guest
 * with virtual memory off. To stop, bring XLog to the front and press a key.
 */

#ifndef XLOG_ENFORCE
#define XLOG_ENFORCE 0
#endif
#if XLOG_ENFORCE
#define kName "Guard"
#else
#define kName "XLog"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <Events.h>
#include <Processes.h>
#include <Timer.h>
#include <MixedMode.h>

#include "nk.h"
#include "hook.h"
#include "h68k.h"
#include "hnat.h"

enum {
    kMaxDomains = 32,
    kMaxWriters = 512,
    kFallbackRing = 16,
    kTimerMs = 16,
    kRefreshTicks = 30, /* re-read the process list every half second */
    kStatusTicks = 600, /* print a status line every 10 seconds */
    kOffenseLog = 16,
    kMagic = 'XLG3'
};

/* The bitmap covers the whole blue area, one bit per page. */
#define kSpanLo 0UL
#define kSpanHi 0x40000000UL
#define kBitmapBytes ((kSpanHi - kSpanLo) / kPage / 8)
#define kPageMask (~(unsigned long)(kPage - 1))

/* Where a store was caught. In Writer.source and Offense.source, combined
   with kWriterAppCode: the store's code is the writing application's own
   (Guard denies these stores). */
enum { kSrcNative, kSrc68k, kSrcFallback, kSources };
enum { kSrcMask = 0xFF, kWriterAppCode = 0x100 };

typedef struct {
    volatile unsigned long live;  /* in use: its pages may be protected */
    volatile unsigned long dying; /* being removed: never protect again */
    unsigned long seen;           /* found by the last refresh */
    unsigned long lo, hi;         /* whole pages inside the partition */
    unsigned long location, size; /* the partition */
    ProcessSerialNumber psn;
    char name[16];
    unsigned long self_writes, foreign_writes_in;
} Domain;

/* A cross-domain writer: a store site, the application current at the
   store, and the domain written. For 68k stores, pc is the 68k instruction
   after the store and lr is 0. */
typedef struct {
    unsigned long pc, lr, writer_zone, target_lo;
    unsigned long first_addr, last_addr, count;
    unsigned long source;
    char writer[16];
    char target[16];
} Writer;

/* A store by an application's own code into another domain: denied by
   Guard, only logged by XLog. */
typedef struct {
    unsigned long seq; /* 1 for the first */
    unsigned long pc;  /* native PC, or the 68k instruction after the store */
    unsigned long addr, source, denied;
    char writer[16];
    char target[16];
} Offense;

/* tools/xlog-read.py names these in this order. */
typedef struct {
    unsigned long self[kSources], foreign[kSources], lost[kSources];
    unsigned long outside; /* ring entries outside every live domain */
    unsigned long switches, timer_runs, kernel_errors;
    unsigned long refreshes, added, removed;
    unsigned long dropped, nwriters;
    unsigned long info_errors; /* GetProcessInformation failures */
    unsigned long app_code[kSources]; /* offenses, by where caught */
} Counters;

typedef struct {
    /* For tools/xlog-read.py: where everything is. */
    unsigned long magic, self;
    unsigned long off_counters, n_counters;
    unsigned long off_domains, n_domains, domain_size;
    unsigned long off_writers, max_writers, writer_size;
    unsigned long off_cur;
    unsigned long off_offenses, max_offenses, offense_size, enforce;

    Counters c;
    long cur; /* domain of the current application, or -1 */
    unsigned long cur_zone;
    volatile unsigned long active;
    unsigned long area;
    unsigned long my_lo, my_hi; /* XLog's own partition */
    unsigned long seen[kSources];
    volatile HookData *h68k, *hnat;
    /* The hooks' own and passed-on counts, kept for the report because
       removing a hook frees its data. */
    unsigned long hook_self[2], hook_passed[2];
    unsigned char *bitmap;
    Domain domains[kMaxDomains];
    Writer writers[kMaxWriters];
    /* The last kOffenseLog offenses: number n is at (n - 1) % kOffenseLog. */
    volatile unsigned long n_offenses;
    Offense offenses[kOffenseLog];
    /* Written by the fallback handler; fb_count is the last seq. */
    volatile unsigned long fb_count, fb_idx;
    HookEvent fb_ring[kFallbackRing];
    TMTask timer;
    RoutineDescriptor timer_rd;
} State;

static State *g_state;
static ExceptionHandler g_prev;

static inline volatile void *lowmem(unsigned long addr)
{
    __asm__("" : "+r"(addr));
    return (volatile void *)addr;
}

/* A long at an address that is only 2-byte aligned. */
static unsigned long lowmem_long(unsigned long addr)
{
    volatile unsigned short *p = lowmem(addr);
    return ((unsigned long)p[0] << 16) | p[1];
}

static unsigned long current_zone(void)
{
    return lowmem_long(0x2AA); /* ApplZone */
}

static void copy_name(char out[16], const char *in)
{
    int i = 0;
    for (; i < 15 && in[i]; i++)
        out[i] = in[i];
    out[i] = 0;
}

/* "zone 1234ABCD" without snprintf, which is not safe at interrupt time. */
static void zone_name(char out[16], unsigned long zone)
{
    static const char hex[] = "0123456789ABCDEF";
    memcpy(out, "zone ", 5);
    for (int i = 0; i < 8; i++)
        out[5 + i] = hex[(zone >> (28 - 4 * i)) & 15];
    out[13] = 0;
}

/* The live domain whose protected pages hold addr. */
static int domain_at(State *s, unsigned long addr)
{
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (d->live && addr >= d->lo && addr < d->hi)
            return i;
    }
    return -1;
}

/* The live domain whose partition holds addr (an ApplZone). */
static int domain_of_zone(State *s, unsigned long addr)
{
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (d->live && addr >= d->location && addr - d->location < d->size)
            return i;
    }
    return -1;
}

static void set_bits(State *s, unsigned long lo, unsigned long hi, int on)
{
    for (unsigned long p = (lo - kSpanLo) >> 12; p < (hi - kSpanLo) >> 12; p++) {
        unsigned char bit = 0x80 >> (p & 7);
        if (on)
            s->bitmap[p >> 3] |= bit;
        else
            s->bitmap[p >> 3] &= ~bit;
    }
}

/* Protect pages of domain i, unless it is going away. */
static void protect(State *s, int i, unsigned long lo, unsigned long len)
{
    Domain *d = &s->domains[i];
    if (!d->live || d->dying)
        return;
    if (nk_protect(s->area, lo, len))
        s->c.kernel_errors++;
}

static void protect_domain(State *s, int i)
{
    protect(s, i, s->domains[i].lo, s->domains[i].hi - s->domains[i].lo);
}

static void protect_others(State *s)
{
    for (int i = 0; i < kMaxDomains; i++)
        if (i != s->cur)
            protect_domain(s, i);
}

/* Point both hooks' current range at domain i (-1: none): its whole
   partition, not only its whole pages, because the hooks also test the
   store's code against it, and an application's code can start on the
   partly used first page. The partly used pages are never protected, so
   for fault addresses the two ranges are the same. */
static void set_current(State *s, int i)
{
    unsigned long lo = 0, hi = 0;
    if (i >= 0) {
        lo = s->domains[i].location;
        hi = s->domains[i].location + s->domains[i].size;
    }
    hook_set_current(s->h68k, lo, hi);
    hook_set_current(s->hnat, lo, hi);
    s->cur = i;
}

/* On a process switch, protect the previous application's domain. */
static void check_switch(State *s)
{
    unsigned long zone = current_zone();
    if (zone == s->cur_zone)
        return;
    s->cur_zone = zone;
    int d = domain_of_zone(s, zone);
    if (d >= 0 && s->domains[d].dying)
        d = -1;
    if (d == s->cur)
        return;
    int old = s->cur;
    set_current(s, d);
    s->c.switches++;
    if (old >= 0)
        protect_domain(s, old);
}

static void writer_name(State *s, unsigned long zone, char out[16])
{
    int w = domain_of_zone(s, zone);
    if (w >= 0)
        copy_name(out, s->domains[w].name);
    else if (zone >= s->my_lo && zone < s->my_hi)
        copy_name(out, kName);
    else if (zone == lowmem_long(0x2A6)) /* SysZone */
        copy_name(out, "System");
    else
        zone_name(out, zone);
}

static void record(State *s, int src, unsigned long pc, unsigned long lr,
                   unsigned long zone, int t, unsigned long addr)
{
    Domain *d = &s->domains[t];
    d->foreign_writes_in++;
    for (unsigned long i = 0; i < s->c.nwriters; i++) {
        Writer *w = &s->writers[i];
        if (w->pc == pc && w->lr == lr && w->writer_zone == zone &&
            w->target_lo == d->lo && w->source == (unsigned long)src) {
            w->count++;
            w->last_addr = addr;
            return;
        }
    }
    if (s->c.nwriters == kMaxWriters) {
        s->c.dropped++;
        return;
    }
    Writer *w = &s->writers[s->c.nwriters];
    w->pc = pc;
    w->lr = lr;
    w->writer_zone = zone;
    w->target_lo = d->lo;
    w->first_addr = w->last_addr = addr;
    w->count = 1;
    w->source = src;
    writer_name(s, zone, w->writer);
    copy_name(w->target, d->name);
    s->c.nwriters++;
}

static void record_offense(State *s, int src, unsigned long pc,
                           unsigned long zone, int t, unsigned long addr)
{
    unsigned long n = s->n_offenses + 1;
    Offense *o = &s->offenses[(n - 1) % kOffenseLog];
    o->seq = n;
    o->pc = pc;
    o->addr = addr;
    o->source = src;
    o->denied = XLOG_ENFORCE && src != kSrcFallback;
    writer_name(s, zone, o->writer);
    copy_name(o->target, s->domains[t].name);
    s->c.app_code[src]++;
    s->n_offenses = n;
}

/* Take the stores a ring holds with seq in (seen, handled]. */
static void drain(State *s, int src, volatile HookEvent *ring, unsigned long n,
                  unsigned long handled)
{
    unsigned long seen = s->seen[src];
    if (handled == seen || !ring)
        return;
    if (handled - seen > n) {
        /* Pages opened by stores no longer in the ring. */
        s->c.lost[src] += handled - seen - n;
        protect_others(s);
    }
    for (unsigned long i = 0; i < n; i++) {
        volatile HookEvent *e = &ring[i];
        if (e->seq - seen - 1 >= handled - seen)
            continue;
        unsigned long addr = e->addr, zone = e->zone;
        int app = (e->pc & kHookAppCode) != 0;
        int t = domain_at(s, addr);
        if (t < 0) {
            s->c.outside++;
            continue;
        }
        if (domain_of_zone(s, zone) == t) {
            s->c.self[src]++;
            s->domains[t].self_writes++;
        } else {
            unsigned long pc = src == kSrc68k ? hook_next_pc(e->r24)
                                              : e->pc & ~3UL;
            unsigned long lr = src == kSrc68k ? 0 : e->lr;
            s->c.foreign[src]++;
            record(s, src | (app ? kWriterAppCode : 0), pc, lr, zone, t, addr);
            if (app)
                record_offense(s, src, pc, zone, t, addr);
        }
        if (t != s->cur)
            protect(s, t, addr & kPageMask, kPage);
    }
    s->seen[src] = handled;
}

static void drain_all(State *s)
{
    drain(s, kSrc68k, s->h68k->ring, kHookRing, s->h68k->count_handled);
    drain(s, kSrcNative, s->hnat->ring, kHookRing, s->hnat->count_handled);
    drain(s, kSrcFallback, s->fb_ring, kFallbackRing, s->fb_count);
}

/* The Exception Manager's system-wide handler: writes to managed pages
   that did not come through the native hook. */
static long handler(ExceptionInfo *e)
{
    State *s = g_state;
    MemoryExceptionInfo *mi = e->memoryInfo;
    if (!s || !s->active || e->theKind != kReadOnlyMemoryException || !mi ||
        mi->theAddress >= kSpanHi ||
        !hook_bit(s->bitmap, kSpanLo, mi->theAddress))
        return g_prev ? g_prev(e) : -1;
    unsigned long addr = mi->theAddress, pc = e->machineState->PC.lo;
    int app = pc >= s->hnat->cur_lo && pc < s->hnat->cur_hi;
    /* A store the native hook denied (it is in its ring already): let the
       system stop the application. */
    if (app && XLOG_ENFORCE)
        return g_prev ? g_prev(e) : -1;
    if (nk_unprotect(s->area, addr & kPageMask, kPage))
        s->c.kernel_errors++;
    unsigned long seq = s->fb_count + 1;
    volatile HookEvent *ev = &s->fb_ring[s->fb_idx];
    s->fb_idx = (s->fb_idx + 1) % kFallbackRing;
    ev->seq = seq;
    ev->pc = pc | (app ? kHookAppCode : 0);
    ev->addr = addr;
    ev->r24 = 0;
    ev->lr = e->machineState->LR.lo;
    ev->zone = current_zone();
    ev->r4 = 0;
    ev->sp = 0;
    s->fb_count = seq;
    return 0;
}

static void timer_proc(TMTask *tm)
{
    State *s = g_state;
    if (s && s->active) {
        s->c.timer_runs++;
        check_switch(s);
        drain_all(s);
    }
    PrimeTime((QElemPtr)tm, kTimerMs);
}

enum { uppTimerProcInfo = 0x0000B802 }; /* register-based, A1 = task */

static int same_psn(const ProcessSerialNumber *a, const ProcessSerialNumber *b)
{
    return a->lowLongOfPSN == b->lowLongOfPSN &&
           a->highLongOfPSN == b->highLongOfPSN;
}

/* Copy a Pascal-string process name, leaving out control characters:
   Calculator's process name is "\0Calculator". */
static void set_name(Domain *d, const unsigned char *name)
{
    int n = 0;
    for (int i = 1; i <= name[0] && n < 15; i++)
        if (name[i] >= 0x20)
            d->name[n++] = name[i];
    d->name[n] = 0;
}

/* Runs in XLog's own context (never at interrupt time). */
static void refresh_domains(State *s)
{
    ProcessSerialNumber me, psn = {0, kNoProcess};
    GetCurrentProcess(&me);
    for (int i = 0; i < kMaxDomains; i++)
        s->domains[i].seen = 0;

    ProcessInfoRec info;
    Str31 name;
    while (GetNextProcess(&psn) == noErr) {
        Boolean same = false;
        if (SameProcess(&psn, &me, &same) == noErr && same)
            continue;
        memset(&info, 0, sizeof info);
        info.processInfoLength = sizeof info;
        info.processName = name;
        name[0] = 0;
        int ok = GetProcessInformation(&psn, &info) == noErr;
        unsigned long loc = (unsigned long)info.processLocation;
        unsigned long lo = (loc + kPage - 1) & kPageMask;
        unsigned long hi = (loc + info.processSize) & kPageMask;

        int found = -1, free_slot = -1;
        for (int i = 0; i < kMaxDomains; i++) {
            Domain *d = &s->domains[i];
            if (d->live && same_psn(&d->psn, &psn) &&
                (!ok || d->location == loc))
                found = i;
            else if (!d->live && free_slot < 0)
                free_slot = i;
        }
        if (ok && !name[0]) {
            /* A process with no name: use the creator code, as 'calc'. */
            unsigned long sig = info.processSignature;
            name[0] = 6;
            name[1] = name[6] = '\'';
            for (int i = 0; i < 4; i++)
                name[2 + i] = (sig >> (24 - 8 * i)) & 0xFF;
        }
        if (!ok) {
            /* Keep a domain the Process Manager could not describe this
               time; add nothing. */
            s->c.info_errors++;
            if (found >= 0)
                s->domains[found].seen = 1;
            continue;
        }
        if (found >= 0) {
            Domain *d = &s->domains[found];
            d->seen = 1;
            /* A process being launched has no name yet. */
            if (!d->name[0] && name[0])
                set_name(d, name);
            continue;
        }
        if (free_slot < 0 || hi <= lo || lo < kSpanLo || hi > kSpanHi ||
            (lo < s->my_hi && hi > s->my_lo))
            continue;

        Domain *d = &s->domains[free_slot];
        memset(d, 0, sizeof *d);
        d->lo = lo;
        d->hi = hi;
        d->location = loc;
        d->size = info.processSize;
        d->psn = psn;
        set_name(d, name);
        d->seen = 1;
        d->live = 1;
        set_bits(s, lo, hi, 1);
        protect_domain(s, free_slot);
        s->c.added++;
    }

    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (!d->live || d->seen)
            continue;
        d->dying = 1;
        if (s->cur == i) {
            set_current(s, -1);
            s->cur_zone = 0; /* the next tick looks again */
        }
        if (nk_unprotect(s->area, d->lo, d->hi - d->lo))
            s->c.kernel_errors++;
        set_bits(s, d->lo, d->hi, 0);
        d->live = 0;
        s->c.removed++;
    }
    s->c.refreshes++;
}

static void status(State *s)
{
    Counters *c = &s->c;
    printf("domains +%lu -%lu, switches %lu; own/cross: native %lu/%lu, 68k "
           "%lu/%lu, fallback %lu/%lu; lost %lu/%lu; writers %lu; by app "
           "code %lu\n",
           c->added, c->removed, c->switches, c->self[kSrcNative],
           c->foreign[kSrcNative], c->self[kSrc68k], c->foreign[kSrc68k],
           c->self[kSrcFallback], c->foreign[kSrcFallback],
           c->lost[kSrcNative], c->lost[kSrc68k], c->nwriters,
           s->n_offenses);
}

static const char *source_name(unsigned long src)
{
    switch (src & kSrcMask) {
    case kSrc68k:
        return "68k";
    case kSrcFallback:
        return "native, fallback";
    default:
        return "native";
    }
}

/* Print the offenses recorded since *shown (main loop only). */
static void show_offenses(State *s, unsigned long *shown)
{
    unsigned long n = s->n_offenses;
    if (n - *shown > kOffenseLog)
        *shown = n - kOffenseLog;
    for (; *shown < n; (*shown)++) {
        Offense *o = &s->offenses[*shown % kOffenseLog];
        printf("%s: %s's own code (%s, %08lX) wrote into %s at %08lX\n",
               o->denied ? "DENIED" : "would deny", o->writer,
               source_name(o->source), o->pc, o->target, o->addr);
    }
}

static int by_count(const void *a, const void *b)
{
    const Writer *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void report(State *s)
{
    printf("\nrefreshes %lu (process info errors %lu), timer runs %lu, "
           "kernel errors %lu, outside domains %lu, writers not kept %lu\n",
           s->c.refreshes, s->c.info_errors, s->c.timer_runs,
           s->c.kernel_errors, s->c.outside, s->c.dropped);
    printf("68k hook: own %lu, passed on %lu; native hook: own %lu, passed "
           "on %lu\n", s->hook_self[0], s->hook_passed[0], s->hook_self[1],
           s->hook_passed[1]);
    status(s);
    printf("domains:\n");
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (d->live)
            printf("  %-15s %08lX-%08lX own %lu, written by others %lu\n",
                   d->name, d->lo, d->hi, d->self_writes, d->foreign_writes_in);
    }
    /* Names of processes that were still being launched when recorded. */
    for (unsigned long i = 0; i < s->c.nwriters; i++) {
        Writer *w = &s->writers[i];
        int t = domain_at(s, w->target_lo);
        if (!w->target[0] && t >= 0)
            copy_name(w->target, s->domains[t].name);
        if (!w->writer[0])
            writer_name(s, w->writer_zone, w->writer);
    }
    qsort(s->writers, s->c.nwriters, sizeof(Writer), by_count);
    printf("cross-domain writers (68k: PC is the instruction after the "
           "store):\n count PC       LR       writer          target          "
           "addr\n");
    for (unsigned long i = 0; i < s->c.nwriters && i < 24; i++) {
        Writer *w = &s->writers[i];
        unsigned long src = w->source & kSrcMask;
        char lr[9];
        if (src == kSrc68k)
            snprintf(lr, sizeof lr, " (68k)  ");
        else
            snprintf(lr, sizeof lr, "%08lX", w->lr);
        printf("%6lu %08lX %s %-15s %-15s %08lX%s%s\n", w->count, w->pc, lr,
               w->writer, w->target, w->first_addr,
               src == kSrcFallback ? " (fallback)" : "",
               w->source & kWriterAppCode ? " (app code)" : "");
    }
    if (s->n_offenses) {
        unsigned long shown = 0;
        printf("stores by an application's own code (the last %d):\n",
               kOffenseLog);
        show_offenses(s, &shown);
    }
}

static void unhook(State *s, int had68, int hadnat)
{
    s->hook_self[0] = s->h68k->count_self;
    s->hook_passed[0] = s->h68k->count_passed;
    s->hook_self[1] = s->hnat->count_self;
    s->hook_passed[1] = s->hnat->count_passed;
    s->h68k->enforce = 0;
    s->hnat->enforce = 0;
    hook_set_bitmap(s->h68k, 0, 0, 0);
    hook_set_bitmap(s->hnat, 0, 0, 0);
    hook_set_current(s->h68k, 0, 0);
    hook_set_current(s->hnat, 0, 0);
    s->h68k->owner = 0;
    s->hnat->owner = 0;
    if (!had68)
        h68k_uninstall();
    if (!hadnat)
        hnat_uninstall();
}

int main(void)
{
#if XLOG_ENFORCE
    printf("Guard: stop applications whose own code writes into another "
           "application's memory\n");
#else
    printf("XLog: log writes from one application into another's memory\n");
#endif

    InstallFn sys_install = nk_find_system_installer();
    if (!sys_install) {
        printf("Exception Manager system slot not found (not 9.2.2?)\n");
        getchar();
        return 1;
    }
    State *s = (State *)NewPtrSysClear(sizeof(State));
    unsigned char *bitmap = (unsigned char *)NewPtrSysClear(kBitmapBytes);
    UniversalProcPtr timer_upp = NewRoutineDescriptor(
        (ProcPtr)timer_proc, uppTimerProcInfo, kPowerPCISA);
    if (!s || !bitmap || !timer_upp) {
        printf("out of memory\n");
        getchar();
        return 1;
    }
    s->magic = kMagic;
    s->self = (unsigned long)s;
    s->off_counters = offsetof(State, c);
    s->n_counters = sizeof(Counters) / 4;
    s->off_domains = offsetof(State, domains);
    s->n_domains = kMaxDomains;
    s->domain_size = sizeof(Domain);
    s->off_writers = offsetof(State, writers);
    s->max_writers = kMaxWriters;
    s->writer_size = sizeof(Writer);
    s->off_cur = offsetof(State, cur);
    s->off_offenses = offsetof(State, n_offenses);
    s->max_offenses = kOffenseLog;
    s->offense_size = sizeof(Offense);
    s->enforce = XLOG_ENFORCE;
    s->bitmap = bitmap;
    s->cur = -1;

    ProcessSerialNumber me;
    ProcessInfoRec info;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    GetCurrentProcess(&me);
    GetProcessInformation(&me, &info);
    s->my_lo = (unsigned long)info.processLocation;
    s->my_hi = s->my_lo + info.processSize;
    s->cur_zone = current_zone();

    long err = nk_area_from_address(s->my_lo, &s->area);
    int had68 = h68k_installed(), hadnat = hnat_installed();
    long e68 = err ? 0 : h68k_install(s->area);
    long enat = err || e68 ? 0 : hnat_install(s->area);
    if (err || e68 || enat) {
        printf("area err %ld, 68k hook err %ld, native hook err %ld\n", err,
               e68, enat);
        if (!e68 && !had68)
            h68k_uninstall();
        getchar();
        return 1;
    }
    s->h68k = h68k_data();
    s->hnat = hnat_data();
    s->seen[kSrc68k] = s->h68k->count_handled;
    s->seen[kSrcNative] = s->hnat->count_handled;
    /* For tools/xlog-read.py. */
    s->h68k->owner = (unsigned long)s;
    s->hnat->owner = (unsigned long)s;

    printf("state %08lX, my partition %08lX-%08lX; to stop, bring %s to "
           "the front and press a key\n", (unsigned long)s, s->my_lo,
           s->my_hi, kName);

    g_state = s;
    set_current(s, -1);
    hook_set_bitmap(s->h68k, kSpanLo, kSpanHi, bitmap);
    hook_set_bitmap(s->hnat, kSpanLo, kSpanHi, bitmap);
    s->h68k->enforce = XLOG_ENFORCE;
    s->hnat->enforce = XLOG_ENFORCE;
    g_prev = sys_install(handler);
    s->active = 1;
    BlockMoveData(timer_upp, &s->timer_rd, sizeof s->timer_rd);
    s->timer.tmAddr = (ProcPtr)&s->timer_rd;
    InsTime((QElemPtr)&s->timer);
    PrimeTime((QElemPtr)&s->timer, kTimerMs);

    refresh_domains(s);
    for (int i = 0; i < kMaxDomains; i++)
        if (s->domains[i].live)
            printf("  %-15s %08lX-%08lX\n", s->domains[i].name,
                   s->domains[i].lo, s->domains[i].hi);

    unsigned long last_refresh = TickCount(), last_status = TickCount();
    unsigned long shown = 0;
    EventRecord ev;
    for (;;) {
        if (WaitNextEvent(everyEvent, &ev, 6, NULL)) {
            if (ev.what == updateEvt) {
                BeginUpdate((WindowPtr)ev.message);
                EndUpdate((WindowPtr)ev.message);
            } else if (ev.what == keyDown) {
                break;
            }
        }
        unsigned long now = TickCount();
        if (now - last_refresh >= kRefreshTicks) {
            refresh_domains(s);
            last_refresh = now;
        }
        if (now - last_status >= kStatusTicks) {
            status(s);
            last_status = now;
        }
        show_offenses(s, &shown);
    }

    /* Stop denying and stop the tick, take what the rings hold, then open
       everything before the hooks stop managing it. */
    s->h68k->enforce = 0;
    s->hnat->enforce = 0;
    s->active = 0;
    RmvTime((QElemPtr)&s->timer);
    drain_all(s);
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (!d->live)
            continue;
        d->dying = 1;
        if (nk_unprotect(s->area, d->lo, d->hi - d->lo))
            s->c.kernel_errors++;
    }
    sys_install(g_prev);
    unhook(s, had68, hadnat);
    g_state = 0;

    WindowPtr w = FrontWindow();
    if (w) {
        SetPort(w);
        InvalRect(&w->portRect);
    }
    report(s);
    printf("\n(Press Return)\n");
    getchar();
    DisposePtr((Ptr)bitmap);
    DisposePtr((Ptr)s);
    return 0;
}
