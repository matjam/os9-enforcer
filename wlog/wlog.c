/*
 * WLog: log writes into this application's heap made while another process
 * is current.
 *
 * WLog grows its heap to full size, write-protects every page of it, and
 * installs a handler in the Exception Manager's system-wide slot, which runs
 * for exceptions in every process. When a store hits a protected page the
 * handler records who made it (PC, LR, current application), makes that
 * page writable and returns 0, so the store runs again. A Time Manager task
 * re-protects the opened pages every tick, so later writes are caught too.
 * Pages stay protected until you bring WLog to the front and press a key;
 * then it prints the writers it saw and which heap blocks they wrote.
 *
 * Only native PowerPC stores are caught. A store from 68k code to a
 * protected page never reaches the Exception Manager: the emulator turns it
 * into a 68k bus error and the current application is killed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <Events.h>
#include <Windows.h>
#include <MixedMode.h>
#include <Processes.h>

#include "nk.h"
#include "h68k.h"

/* Build switches for narrowing down hangs:
   WLOG_TIMER  re-protect opened pages from a Time Manager task
   WLOG_GROW   grow the heap first and protect from the page holding the
               zone header, instead of only the heap's whole pages */
#ifndef WLOG_TIMER
#define WLOG_TIMER 1
#endif
#ifndef WLOG_GROW
#define WLOG_GROW 1
#endif

enum {
    kOpenPages = 8, /* pages left writable between ticks; at least two, for
                       stores that span pages */
    kMaxWriters = 128,
    kTimerMs = 16
};

/* A writer: a store site seen writing this heap while another application
   was current. For native code, pc and lr are PowerPC addresses. For 68k
   code (is68k), pc is the 68k instruction after the store and lr is 0. */
typedef struct {
    unsigned long pc, lr, zone, first_addr, last_addr, count;
    int is68k;
    char app[16];
} Writer;

/* Everything the handler and the timer write lives here, in the system
   heap, because they run while WLog's own heap is protected. */
/* Recent faults, for finding out what happened after a hang: the state
   block starts with a magic number, so it can be found in a dump of
   physical memory. */
typedef struct {
    unsigned long pc, lr, addr, sp, zone, seq;
} Event;

enum { kEvents = 32, kMagic = 'WLG!' };

typedef struct {
    unsigned long magic;
    unsigned long self; /* logical address of this block */
    unsigned long seq;
    unsigned long timer_sp_min; /* lowest stack address seen in the timer */
    Event events[kEvents];
    volatile int active;
    volatile int busy; /* handler running; the timer leaves the pages alone */
    unsigned long area, lo, hi, my_zone;
    unsigned long open[kOpenPages];
    int nopen;
    unsigned long faults, self_faults, foreign_faults, kernel_errors;
    unsigned long timer_runs, overflow;
    unsigned long dropped; /* foreign writers past kMaxWriters */
    /* 68k stores, caught by the 68k write hook (h68k.s) and taken from its
       ring every tick. h68k_lost: more in one tick than the ring holds. */
    unsigned long h68k_seen, h68k_lost, own_68k, foreign_68k;
    int nwriters;
    Writer writers[kMaxWriters];
    TMTask timer;
    /* The Time Manager calls the timer through this descriptor at
       interrupt time, from 68k code. It must not be in the protected heap:
       a 68k store that faults at interrupt time hangs the whole system. */
    RoutineDescriptor timer_rd;
} State;

static State *g_state;
static ExceptionHandler g_prev;

/* A pointer to a fixed low-memory address. The empty asm hides the constant
   from GCC, which otherwise warns that address 0x2AA is outside any object. */
static inline volatile void *lowmem(unsigned long addr)
{
    __asm__("" : "+r"(addr));
    return (volatile void *)addr;
}

/* Low-memory globals the Process Manager swaps per process. */
static unsigned long current_zone(void)
{
    volatile unsigned short *p = lowmem(0x2AA); /* ApplZone, 2-byte aligned */
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

/* The name of the process whose partition holds a heap zone, from the
   Process Manager (not at interrupt time). */
static void app_for_zone(unsigned long zone, char out[16])
{
    ProcessSerialNumber psn = {0, kNoProcess};
    ProcessInfoRec info;
    Str31 name;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof info);
        info.processInfoLength = sizeof info;
        info.processName = name;
        if (GetProcessInformation(&psn, &info) != noErr)
            continue;
        unsigned long lo = (unsigned long)info.processLocation;
        if (zone >= lo && zone < lo + info.processSize) {
            int n = name[0] < 15 ? name[0] : 15;
            memcpy(out, name + 1, n);
            out[n] = 0;
            return;
        }
    }
    if (zone == (unsigned long)SystemZone())
        snprintf(out, 16, "(system zone)");
    else
        snprintf(out, 16, "zone %08lX", zone);
}

static void record_foreign(State *s, unsigned long pc, unsigned long lr,
                           unsigned long zone, unsigned long addr, int is68k);

/* Take the 68k stores the hook caught since the last call: log them, and
   protect their pages again (the hook leaves them writable). */
static void drain_h68k(State *s)
{
    volatile H68kData *hd = h68k_data();
    unsigned long handled = hd->count_handled;
    unsigned long seen = s->h68k_seen;
    if (handled == seen)
        return;
    if (handled - seen > kH68kRing) {
        /* Some opened pages are no longer in the ring: protect the whole
           range again. */
        s->h68k_lost += handled - seen - kH68kRing;
        if (nk_protect(s->area, s->lo, s->hi - s->lo))
            s->kernel_errors++;
    }
    for (int i = 0; i < kH68kRing; i++) {
        volatile H68kEvent *e = &hd->ring[i];
        if (e->seq <= seen || e->seq > handled)
            continue;
        if (nk_protect(s->area, e->addr & ~(unsigned long)(kPage - 1), kPage))
            s->kernel_errors++;
        if (e->zone == s->my_zone) {
            s->own_68k++;
        } else {
            s->foreign_68k++;
            record_foreign(s, h68k_next_pc(e->r24), 0, e->zone, e->addr, 1);
        }
    }
    s->h68k_seen = handled;
}

static void close_all(State *s)
{
    for (int i = 0; i < s->nopen; i++)
        if (nk_protect(s->area, s->open[i], kPage))
            s->kernel_errors++;
    s->nopen = 0;
    drain_h68k(s);
}

static void open_page(State *s, unsigned long page)
{
    if (s->nopen == kOpenPages) {
        /* More pages written within one tick than we keep open: start
           over, keeping only this one. */
        s->overflow++;
        close_all(s);
    }
    s->open[s->nopen++] = page;
    if (nk_unprotect(s->area, page, kPage))
        s->kernel_errors++;
}

static void record_foreign(State *s, unsigned long pc, unsigned long lr,
                           unsigned long zone, unsigned long addr, int is68k)
{
    for (int i = 0; i < s->nwriters; i++) {
        Writer *w = &s->writers[i];
        if (w->pc == pc && w->lr == lr && w->zone == zone &&
            w->is68k == is68k) {
            w->count++;
            w->last_addr = addr;
            return;
        }
    }
    if (s->nwriters == kMaxWriters) {
        s->dropped++;
        return;
    }
    Writer *w = &s->writers[s->nwriters++];
    w->pc = pc;
    w->lr = lr;
    w->zone = zone;
    w->first_addr = w->last_addr = addr;
    w->count = 1;
    w->is68k = is68k;
    /* A 68k store is logged a tick late, when another application may be
       current; its name comes from its zone at report time. */
    if (is68k)
        w->app[0] = 0;
    else
        current_app(w->app);
}

static long handler(ExceptionInfo *e)
{
    State *s = g_state;
    MemoryExceptionInfo *mi = e->memoryInfo;
    if (!s || !s->active || e->theKind != kReadOnlyMemoryException || !mi ||
        mi->theAddress < s->lo || mi->theAddress >= s->hi)
        return g_prev ? g_prev(e) : -1;

    s->busy = 1;
    unsigned long addr = mi->theAddress;
    Event *ev = &s->events[s->seq % kEvents];
    ev->seq = ++s->seq;
    ev->pc = e->machineState->PC.lo;
    ev->lr = e->machineState->LR.lo;
    ev->addr = addr;
    ev->sp = (unsigned long)__builtin_frame_address(0);
    ev->zone = current_zone();
    open_page(s, addr & ~(unsigned long)(kPage - 1));
    s->faults++;
    unsigned long zone = current_zone();
    if (zone == s->my_zone) {
        s->self_faults++;
    } else {
        s->foreign_faults++;
        record_foreign(s, e->machineState->PC.lo, e->machineState->LR.lo,
                       zone, addr, 0);
    }
    s->busy = 0;
    return 0;
}

/* Runs every tick at interrupt time, called through Mixed Mode with the
   task in A1. */
static void timer_proc(TMTask *tm)
{
    State *s = g_state;
    if (s && s->active && !s->busy) {
        unsigned long sp = (unsigned long)__builtin_frame_address(0);
        if (!s->timer_sp_min || sp < s->timer_sp_min)
            s->timer_sp_min = sp;
        s->timer_runs++;
        close_all(s);
    }
    PrimeTime((QElemPtr)tm, kTimerMs);
}

enum { uppTimerProcInfo = 0x0000B802 }; /* register-based, A1 = task */

/* Heap block containing addr, from a walk of the zone's block headers
   (12-byte headers: tag in the top two bits of byte 0, physical size at
   +4, relative handle at +8 for relocatable blocks). */
static void describe(THz zone, unsigned long addr, char *out, size_t len)
{
    unsigned long z = (unsigned long)zone;
    unsigned long p = z + 52; /* heapData */
    unsigned long lim = (unsigned long)zone->bkLim;
    if (addr < p) {
        snprintf(out, len, "zone header+%lX", addr - z);
        return;
    }
    while (p < lim) {
        unsigned char tag = *(unsigned char *)p >> 6;
        unsigned long size = *(unsigned long *)(p + 4);
        if (size < 12 || p + size > lim + 12)
            break;
        if (addr >= p && addr < p + size) {
            static const char *kinds[] = {"free", "ptr", "handle", "?"};
            if (tag == 2) {
                unsigned long mp = z + *(unsigned long *)(p + 8);
                snprintf(out, len, "handle %08lX data+%lX", mp,
                         addr - (p + 12));
            } else {
                snprintf(out, len, "%s %08lX+%lX", kinds[tag], p + 12,
                         addr - (p + 12));
            }
            return;
        }
        p += size;
    }
    snprintf(out, len, addr >= lim ? "above bkLim" : "walk lost at %08lX", p);
}

typedef struct {
    const char *name;
    unsigned long mp; /* master pointer address, for handles */
    unsigned long lo, hi;
} Known;

enum { kMaxKnown = 8 };
static Known g_known[kMaxKnown];
static int g_nknown;

static void know_ptr(const char *name, void *p, unsigned long size)
{
    if (p && g_nknown < kMaxKnown) {
        Known *k = &g_known[g_nknown++];
        k->name = name;
        k->mp = 0;
        k->lo = (unsigned long)p;
        k->hi = k->lo + size;
    }
}

static void know_handle(const char *name, Handle h)
{
    if (h && *h && g_nknown < kMaxKnown) {
        Known *k = &g_known[g_nknown++];
        k->name = name;
        k->mp = (unsigned long)h;
        k->lo = (unsigned long)*h;
        k->hi = k->lo + GetHandleSize(h);
    }
}

static void know_window(WindowPtr w)
{
    WindowPeek wp = (WindowPeek)w;
    g_nknown = 0;
    know_ptr("window", w, sizeof(WindowRecord));
    know_handle("strucRgn", (Handle)wp->strucRgn);
    know_handle("contRgn", (Handle)wp->contRgn);
    know_handle("updateRgn", (Handle)wp->updateRgn);
    know_handle("visRgn", (Handle)w->visRgn);
    know_handle("clipRgn", (Handle)w->clipRgn);
}

static const char *known_name(unsigned long addr)
{
    for (int i = 0; i < g_nknown; i++)
        if (addr >= g_known[i].lo && addr < g_known[i].hi)
            return g_known[i].name;
    return "";
}

static int by_count(const void *a, const void *b)
{
    const Writer *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

int main(void)
{
    printf("WLog: log other processes' writes into this heap (timer %d, "
           "grow %d)\n", WLOG_TIMER, WLOG_GROW);

    InstallFn sys_install = nk_find_system_installer();
    if (!sys_install) {
        printf("Exception Manager system slot not found (not 9.2.2?)\n");
        getchar();
        return 1;
    }

    State *s = (State *)NewPtrSysClear(sizeof(State));
    UniversalProcPtr timer_upp = NewRoutineDescriptor(
        (ProcPtr)timer_proc, uppTimerProcInfo, kPowerPCISA);
    if (!s || !timer_upp) {
        printf("out of memory\n");
        getchar();
        return 1;
    }

    /* Grow the heap to its limit first, so blocks allocated later are
       inside the protected range. */
#if WLOG_GROW
    MaxApplZone();
#endif
    THz zone = ApplicationZone();
#if WLOG_GROW
    unsigned long lo = (unsigned long)zone & ~(unsigned long)(kPage - 1);
    unsigned long hi = ((unsigned long)zone->bkLim + kPage - 1) &
                       ~(unsigned long)(kPage - 1);
#else
    unsigned long lo = ((unsigned long)zone + kPage - 1) &
                       ~(unsigned long)(kPage - 1);
    unsigned long hi = (unsigned long)zone->bkLim & ~(unsigned long)(kPage - 1);
#endif
    unsigned long sp = (unsigned long)__builtin_frame_address(0);
    s->magic = kMagic;
    s->self = (unsigned long)s;
    s->my_zone = (unsigned long)zone;
    s->lo = lo;
    s->hi = hi;
    long err = nk_area_from_address(lo, &s->area);
    printf("zone %08lX, protecting %08lX-%08lX (%lu pages), stack %08lX\n",
           (unsigned long)zone, lo, hi, (hi - lo) / kPage, sp);
    if (err || hi <= lo || (sp >= lo && sp < hi + kPage)) {
        printf("cannot protect this heap (area err %ld)\n", err);
        getchar();
        return 1;
    }

    /* Without the hook, a 68k store into the protected heap from interrupt
       time or a process switch hangs the system. */
    int had_h68k = h68k_installed();
    long herr = h68k_install(s->area);
    if (herr) {
        printf("cannot install the 68k write hook (%ld)\n", herr);
        getchar();
        return 1;
    }
    s->h68k_seen = h68k_data()->count_handled;

    WindowPtr console = FrontWindow();
    if (console)
        know_window(console);

    printf("Protecting. Bring another application to the front (Cmd-Tab)\n"
           "and move its windows over this one. To stop, come back here\n"
           "and press a key.\n");

    g_state = s;
    g_prev = sys_install(handler);
    s->active = 1;
#if WLOG_TIMER
    BlockMoveData(timer_upp, &s->timer_rd, sizeof s->timer_rd);
    s->timer.tmAddr = (ProcPtr)&s->timer_rd;
    InsTime((QElemPtr)&s->timer);
    PrimeTime((QElemPtr)&s->timer, kTimerMs);
#endif
    h68k_set_range(lo, hi);
    err = nk_protect(s->area, lo, hi - lo);

    EventRecord ev;
    for (;;) {
        if (!WaitNextEvent(everyEvent, &ev, 30, NULL))
            continue;
        if (ev.what == updateEvt) {
            BeginUpdate((WindowPtr)ev.message);
            EndUpdate((WindowPtr)ev.message);
        } else if (ev.what == keyDown) {
            break;
        }
    }

    s->active = 0;
#if WLOG_TIMER
    RmvTime((QElemPtr)&s->timer);
#endif
    drain_h68k(s);
    h68k_set_range(0, 0);
    long uerr = nk_unprotect(s->area, lo, hi - lo);
    sys_install(g_prev);
    g_state = 0;

    /* Copy the hook's results out before removing it. */
    volatile H68kData *hd = h68k_data();
    unsigned long h68k_passed = hd->count_passed;
    H68kPassed passed = *(H68kPassed *)&hd->passed;
    if (!had_h68k)
        h68k_uninstall();

    for (int i = 0; i < s->nwriters; i++)
        if (s->writers[i].is68k)
            app_for_zone(s->writers[i].zone, s->writers[i].app);

    if (console) {
        SetPort(console);
        InvalRect(&console->portRect);
        know_window(console);
    }
    printf("\nprotect err %ld, unprotect err %ld; in handler: kernel errors "
           "%lu, overflows %lu; timer runs %lu\n",
           err, uerr, s->kernel_errors, s->overflow, s->timer_runs);
    printf("native faults %lu: own %lu, other processes %lu\n", s->faults,
           s->self_faults, s->foreign_faults);
    printf("68k faults %lu: own %lu, other processes %lu, lost %lu\n",
           s->own_68k + s->foreign_68k + s->h68k_lost, s->own_68k,
           s->foreign_68k, s->h68k_lost);
    qsort(s->writers, s->nwriters, sizeof(Writer), by_count);
    printf("writers from other processes: %d (%lu not kept)\n", s->nwriters,
           s->dropped);
    printf("count PC       LR       app             addr     block\n");
    for (int i = 0; i < s->nwriters && i < 20; i++) {
        Writer *w = &s->writers[i];
        char where[48], lr[9];
        describe(zone, w->first_addr, where, sizeof where);
        if (w->is68k)
            snprintf(lr, sizeof lr, "  (68k) ");
        else
            snprintf(lr, sizeof lr, "%08lX", w->lr);
        printf("%5lu %08lX %s %-15s %08lX %s %s\n", w->count, w->pc, lr,
               w->app, w->first_addr, where, known_name(w->first_addr));
    }
    printf("(for 68k writers, PC is the instruction after the store)\n");
    printf("window: ");
    for (int i = 0; i < g_nknown; i++)
        printf("%s %08lX ", g_known[i].name,
               g_known[i].mp ? g_known[i].mp : g_known[i].lo);
    printf("\n");

    if (h68k_passed) {
        char app[16];
        app_for_zone(passed.zone, app);
        printf("68k write faults outside the range: %lu; last: addr %08lX, "
               "next 68k %08lX, %s\n", h68k_passed, passed.addr,
               h68k_next_pc(passed.r24), app);
    }

    DisposePtr((Ptr)s);
    printf("\n(Press Return)\n");
    getchar();
    return 0;
}
