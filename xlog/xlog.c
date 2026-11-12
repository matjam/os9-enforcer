/*
 * XLog: log writes from one application into another's memory, across all
 * running applications. Log only: every write is allowed.
 *
 * Each running application's partition (from the Process Manager, rounded
 * inward to whole pages) is a domain. The pages of every domain start
 * write-protected. A write to a protected page faults:
 *   - native code: the Exception Manager's system-wide handler slot;
 *   - 68k code: the 68k write hook (common/h68k.s), which opens the page
 *     and records the store in its ring for the next tick.
 * If the domain written belongs to the current application (the one whose
 * heap zone is ApplZone), it is the owner's own write: the page stays open
 * until that application is switched out. Otherwise it is a cross-domain
 * write: it is logged by writer (PC, LR, current application) and target,
 * and the page is protected again on the next tick.
 *
 * XLog's own partition is not a domain, and nothing outside partitions is
 * protected. A 16 ms Time Manager task notices process switches (ApplZone
 * changes), protects pages that should no longer be open, and takes the
 * hook's ring.
 *
 * Safety rule: a protected page always has its bit set in the bitmap and a
 * live domain behind it. Otherwise a fault on it would go on to the
 * emulator or kill the application. Domains are added as live, then bits,
 * then protect; removed as dying, unprotect, then bits, then dead.
 *
 * Run it only in a SCRATCH=1 guest. To stop, bring XLog to the front and
 * press a key.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <Events.h>
#include <Processes.h>
#include <Timer.h>
#include <MixedMode.h>

#include "nk.h"
#include "h68k.h"

enum {
    kMaxDomains = 32,
    kMaxOpen = 256,
    kMaxWriters = 512,
    kTimerMs = 16,
    kRefreshTicks = 30, /* re-read the process list every half second */
    kStatusTicks = 600, /* print a status line every 10 seconds */
    kMagic = 'XLG!'
};

/* The bitmap covers the whole blue area, one bit per page. */
#define kSpanLo 0UL
#define kSpanHi 0x40000000UL
#define kBitmapBytes ((kSpanHi - kSpanLo) / kPage / 8)

typedef struct {
    volatile int live;   /* in use and protected */
    volatile int dying;  /* being removed: never protect its pages again */
    int seen;            /* found by the last refresh */
    unsigned long lo, hi;
    unsigned long location, size; /* partition, from the Process Manager */
    ProcessSerialNumber psn;
    char name[16];
    unsigned long self_writes, foreign_writes_in;
} Domain;

typedef struct {
    unsigned long page;
    int domain;
} OpenPage;

/* A cross-domain writer: a store site, the application current at the
   store, and the domain written. For 68k stores, pc is the 68k instruction
   after the store and lr is 0. */
typedef struct {
    unsigned long pc, lr, writer_zone, target_lo;
    unsigned long first_addr, last_addr, count;
    int is68k;
    char writer[16];
    char target[16];
} Writer;

typedef struct {
    unsigned long magic;
    unsigned long self;
    volatile int active;
    volatile int busy; /* the handler is running: the timer waits */
    unsigned long area;
    unsigned long my_lo, my_hi; /* XLog's own partition */
    unsigned long my_zone;
    Domain domains[kMaxDomains];
    int cur; /* domain of the current application, or -1 */
    unsigned long cur_zone;
    int nopen;
    OpenPage open[kMaxOpen];
    unsigned char *bitmap;
    unsigned long h68k_seen, h68k_lost;
    unsigned long native_self, native_foreign, m68k_self, m68k_foreign;
    unsigned long switches, timer_runs, kernel_errors, open_overflows;
    unsigned long refreshes, added, removed, chained;
    unsigned long dropped;
    int nwriters;
    Writer writers[kMaxWriters];
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

static unsigned long current_zone(void)
{
    volatile unsigned short *p = lowmem(0x2AA); /* ApplZone */
    return ((unsigned long)p[0] << 16) | p[1];
}

static void current_app(char out[16])
{
    volatile unsigned char *name = lowmem(0x910); /* CurApName */
    int n = name[0] < 15 ? name[0] : 15;
    for (int i = 0; i < n; i++)
        out[i] = name[1 + i];
    out[n] = 0;
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

static int domain_of(State *s, unsigned long addr)
{
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (d->live && addr >= d->lo && addr < d->hi)
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

static void protect(State *s, unsigned long lo, unsigned long len)
{
    if (nk_protect(s->area, lo, len))
        s->kernel_errors++;
}

/* Protect again every open page outside domain keep (keep = -1: all).
   Pages of dying domains are dropped without protecting them. */
static void close_others(State *s, int keep)
{
    int n = 0;
    for (int i = 0; i < s->nopen; i++) {
        OpenPage *o = &s->open[i];
        if (o->domain == keep && keep >= 0) {
            s->open[n++] = *o;
            continue;
        }
        Domain *d = &s->domains[o->domain];
        if (d->live && !d->dying)
            protect(s, o->page, kPage);
    }
    s->nopen = n;
}

/* Note a page that is now writable. */
static void note_open(State *s, unsigned long page, int d)
{
    if (s->nopen == kMaxOpen) {
        s->open_overflows++;
        close_others(s, -1);
    }
    s->open[s->nopen].page = page;
    s->open[s->nopen].domain = d;
    s->nopen++;
}

/* On a process switch, protect the pages the previous application had
   open. */
static void check_switch(State *s)
{
    unsigned long zone = current_zone();
    if (zone == s->cur_zone)
        return;
    s->cur_zone = zone;
    int d = domain_of(s, zone);
    if (d != s->cur) {
        s->switches++;
        s->cur = d;
        close_others(s, d);
    }
}

static void writer_name(State *s, unsigned long zone, int is68k, char out[16])
{
    int w = domain_of(s, zone);
    if (w >= 0)
        copy_name(out, s->domains[w].name);
    else if (zone == s->my_zone)
        copy_name(out, "XLog");
    else if (!is68k)
        current_app(out);
    else
        zone_name(out, zone);
}

static void record(State *s, unsigned long pc, unsigned long lr,
                   unsigned long zone, int t, unsigned long addr, int is68k)
{
    Domain *d = &s->domains[t];
    d->foreign_writes_in++;
    for (int i = 0; i < s->nwriters; i++) {
        Writer *w = &s->writers[i];
        if (w->pc == pc && w->lr == lr && w->writer_zone == zone &&
            w->target_lo == d->lo && w->is68k == is68k) {
            w->count++;
            w->last_addr = addr;
            return;
        }
    }
    if (s->nwriters == kMaxWriters) {
        s->dropped++;
        return;
    }
    Writer *w = &s->writers[s->nwriters];
    w->pc = pc;
    w->lr = lr;
    w->writer_zone = zone;
    w->target_lo = d->lo;
    w->first_addr = w->last_addr = addr;
    w->count = 1;
    w->is68k = is68k;
    writer_name(s, zone, is68k, w->writer);
    copy_name(w->target, d->name);
    s->nwriters++;
}

/* Take the 68k stores the hook caught since the last call. */
static void drain_h68k(State *s)
{
    volatile H68kData *hd = h68k_data();
    unsigned long handled = hd->count_handled;
    unsigned long seen = s->h68k_seen;
    if (handled == seen)
        return;
    if (handled - seen > kH68kRing) {
        /* Pages the hook opened that are no longer in its ring: protect
           every domain but the current one again. */
        s->h68k_lost += handled - seen - kH68kRing;
        for (int i = 0; i < kMaxDomains; i++) {
            Domain *d = &s->domains[i];
            if (d->live && !d->dying && i != s->cur)
                protect(s, d->lo, d->hi - d->lo);
        }
    }
    for (int i = 0; i < kH68kRing; i++) {
        volatile H68kEvent *e = &hd->ring[i];
        if (e->seq <= seen || e->seq > handled)
            continue;
        int t = domain_of(s, e->addr);
        if (t < 0)
            continue;
        if (!s->domains[t].dying)
            note_open(s, e->addr & ~(unsigned long)(kPage - 1), t);
        if (domain_of(s, e->zone) == t) {
            s->m68k_self++;
            s->domains[t].self_writes++;
        } else {
            s->m68k_foreign++;
            record(s, h68k_next_pc(e->r24), 0, e->zone, t, e->addr, 1);
        }
    }
    s->h68k_seen = handled;
}

static long handler(ExceptionInfo *e)
{
    State *s = g_state;
    MemoryExceptionInfo *mi = e->memoryInfo;
    if (!s || !s->active || e->theKind != kReadOnlyMemoryException || !mi ||
        mi->theAddress >= kSpanHi ||
        !h68k_bit(s->bitmap, kSpanLo, mi->theAddress))
        return g_prev ? g_prev(e) : -1;
    unsigned long addr = mi->theAddress;
    int t = domain_of(s, addr);
    if (t < 0) {
        /* Bit set without a live domain: should not happen. */
        s->chained++;
        return g_prev ? g_prev(e) : -1;
    }

    s->busy = 1;
    check_switch(s);
    unsigned long page = addr & ~(unsigned long)(kPage - 1);
    if (nk_unprotect(s->area, page, kPage))
        s->kernel_errors++;
    if (!s->domains[t].dying)
        note_open(s, page, t);
    if (t == s->cur) {
        s->native_self++;
        s->domains[t].self_writes++;
    } else {
        s->native_foreign++;
        record(s, e->machineState->PC.lo, e->machineState->LR.lo,
               current_zone(), t, addr, 0);
    }
    s->busy = 0;
    return 0;
}

static void timer_proc(TMTask *tm)
{
    State *s = g_state;
    if (s && s->active && !s->busy) {
        s->timer_runs++;
        check_switch(s);
        drain_h68k(s);
        close_others(s, s->cur);
    }
    PrimeTime((QElemPtr)tm, kTimerMs);
}

enum { uppTimerProcInfo = 0x0000B802 }; /* register-based, A1 = task */

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
        if (GetProcessInformation(&psn, &info) != noErr)
            continue;
        unsigned long loc = (unsigned long)info.processLocation;
        unsigned long lo = (loc + kPage - 1) & ~(unsigned long)(kPage - 1);
        unsigned long hi = (loc + info.processSize) & ~(unsigned long)(kPage - 1);

        int found = -1, free_slot = -1;
        for (int i = 0; i < kMaxDomains; i++) {
            Domain *d = &s->domains[i];
            if (d->live && d->location == loc && d->psn.lowLongOfPSN ==
                psn.lowLongOfPSN && d->psn.highLongOfPSN == psn.highLongOfPSN)
                found = i;
            else if (!d->live && free_slot < 0)
                free_slot = i;
        }
        if (found >= 0) {
            s->domains[found].seen = 1;
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
        int n = name[0] < 15 ? name[0] : 15;
        memcpy(d->name, name + 1, n);
        d->name[n] = 0;
        d->seen = 1;
        d->live = 1;
        set_bits(s, lo, hi, 1);
        protect(s, lo, hi - lo);
        s->added++;
    }

    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (!d->live || d->seen)
            continue;
        d->dying = 1;
        if (nk_unprotect(s->area, d->lo, d->hi - d->lo))
            s->kernel_errors++;
        set_bits(s, d->lo, d->hi, 0);
        d->live = 0;
        s->removed++;
    }
    s->refreshes++;
}

static void status(State *s)
{
    printf("domains %lu-%lu, switches %lu; native self %lu foreign %lu; "
           "68k self %lu foreign %lu lost %lu; writers %d\n", s->added,
           s->removed, s->switches, s->native_self, s->native_foreign,
           s->m68k_self, s->m68k_foreign, s->h68k_lost, s->nwriters);
}

static int by_count(const void *a, const void *b)
{
    const Writer *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void report(State *s)
{
    printf("\nrefreshes %lu, timer runs %lu, kernel errors %lu, open "
           "overflows %lu, chained %lu, writers not kept %lu\n",
           s->refreshes, s->timer_runs, s->kernel_errors, s->open_overflows,
           s->chained, s->dropped);
    status(s);
    printf("domains:\n");
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (d->live)
            printf("  %-15s %08lX-%08lX self %lu, written by others %lu\n",
                   d->name, d->lo, d->hi, d->self_writes, d->foreign_writes_in);
    }
    qsort(s->writers, s->nwriters, sizeof(Writer), by_count);
    printf("cross-domain writers (68k: PC is the instruction after the "
           "store):\n count PC       LR       writer          target          "
           "addr\n");
    for (int i = 0; i < s->nwriters && i < 24; i++) {
        Writer *w = &s->writers[i];
        char lr[9];
        if (w->is68k)
            snprintf(lr, sizeof lr, " (68k)  ");
        else
            snprintf(lr, sizeof lr, "%08lX", w->lr);
        printf("%6lu %08lX %s %-15s %-15s %08lX\n", w->count, w->pc, lr,
               w->writer, w->target, w->first_addr);
    }
}

int main(void)
{
    printf("XLog: log writes from one application into another's memory\n");

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
    s->bitmap = bitmap;
    s->cur = -1;
    s->my_zone = (unsigned long)ApplicationZone();

    ProcessSerialNumber me;
    ProcessInfoRec info;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    GetCurrentProcess(&me);
    GetProcessInformation(&me, &info);
    s->my_lo = (unsigned long)info.processLocation;
    s->my_hi = s->my_lo + info.processSize;

    long err = nk_area_from_address(s->my_lo, &s->area);
    int had_h68k = h68k_installed();
    long herr = err ? 0 : h68k_install(s->area);
    if (err || herr) {
        printf("area err %ld, 68k hook err %ld\n", err, herr);
        getchar();
        return 1;
    }
    s->h68k_seen = h68k_data()->count_handled;
    h68k_data()->owner = (unsigned long)s; /* for tools/xlog-read.py */

    printf("state %08lX, my partition %08lX-%08lX; to stop, bring XLog to "
           "the front and press a key\n", (unsigned long)s, s->my_lo,
           s->my_hi);

    g_state = s;
    g_prev = sys_install(handler);
    h68k_set_bitmap(kSpanLo, kSpanHi, bitmap);
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
    }

    s->active = 0;
    RmvTime((QElemPtr)&s->timer);
    drain_h68k(s);
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (!d->live)
            continue;
        d->dying = 1;
        if (nk_unprotect(s->area, d->lo, d->hi - d->lo))
            s->kernel_errors++;
    }
    h68k_set_range(0, 0);
    sys_install(g_prev);
    g_state = 0;
    if (!had_h68k)
        h68k_uninstall();
    memset(bitmap, 0, kBitmapBytes);

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
