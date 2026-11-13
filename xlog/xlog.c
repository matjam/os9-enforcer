/*
 * XLog: log writes from one application into another's memory, across all
 * running applications. Log only: every write is allowed.
 *
 * Guard: the same program built with XLOG_ENFORCE=1. It denies a store into
 * another application's memory when the code doing it is the current
 * application's own (it lies in the current application's partition). The
 * hooks leave the page protected and pass the fault on, and the system
 * stops the application. Stores made by system code (the Process Manager,
 * the Window Manager and others, which live outside every partition) are
 * still allowed and logged. XLog marks the stores Guard would deny.
 *
 * SysLog and SysGuard: XLog and Guard built with XLOG_SYSHEAP=1, which
 * also watch the system heap page by page (log only, in both). They print
 * how many traps that costs and log stores into it by an application's own
 * code.
 *
 * All are console front ends on the protection engine (common/engine.c).
 * Run them only in a SCRATCH=1 guest with virtual memory off. To stop,
 * bring the window to the front and press a key.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <Events.h>
#include <Windows.h>

#include "engine.h"

#ifndef XLOG_ENFORCE
#define XLOG_ENFORCE 0
#endif
#ifndef XLOG_SYSHEAP
#define XLOG_SYSHEAP 0
#endif
#if XLOG_ENFORCE
#define kName (XLOG_SYSHEAP ? "SysGuard" : "Guard")
#else
#define kName (XLOG_SYSHEAP ? "SysLog" : "XLog")
#endif

enum {
    kRefreshTicks = 30, /* re-read the process list every half second */
    kStatusTicks = 600  /* print a status line every 10 seconds */
};

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
    SysPages *sp = s->sys;
    if (sp)
        printf("system heap %s: %lu pages, traps %lu (app code %lu) in %lu "
               "ticks, %.1f a tick; hot now %lu, made hot %lu, cooled %lu, "
               "reheated %lu, full %lu; stacks %lu, resweeps %lu\n",
               s->sys_on ? "watched" : "not watched", sp->n, sp->c.traps,
               sp->c.app_traps, sp->tick,
               sp->tick ? (double)sp->c.traps / sp->tick : 0.0, sp->n_hot,
               sp->c.hot, sp->c.cooled, sp->c.reheated, sp->c.hot_full,
               sp->c.stacks, sp->c.resweeps);
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

/* Print the offenses recorded since *shown. */
static void show_offenses(State *s, unsigned long *shown)
{
    unsigned long n = s->n_offenses;
    if (n - *shown > kOffenseLog)
        *shown = n - kOffenseLog;
    for (; *shown < n; (*shown)++) {
        Offense *o = &s->offenses[*shown % kOffenseLog];
        printf("%s: %s's own code (%s, %08lX) wrote into %s at %08lX\n",
               o->denied == kOffenseStopped   ? "DENIED"
               : o->denied == kOffenseBlocked ? "BLOCKED"
                                              : "would deny",
               o->writer,
               source_name(o->source), o->pc, o->target, o->addr);
    }
}

static int by_count(const void *a, const void *b)
{
    const Writer *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

static void copy_name(char out[16], const char *in)
{
    int i = 0;
    for (; i < 15 && in[i]; i++)
        out[i] = in[i];
    out[i] = 0;
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
        int t = engine_domain_at(s, w->target_lo);
        if (!w->target[0] && t >= 0)
            copy_name(w->target, s->domains[t].name);
        if (!w->writer[0])
            engine_writer_name(s, w->writer_zone, w->writer);
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

int main(void)
{
#if XLOG_ENFORCE
    printf("Guard: stop applications whose own code writes into another "
           "application's memory\n");
#else
    printf("XLog: log writes from one application into another's memory\n");
#endif

    State *s;
    long err = engine_start(&s, XLOG_ENFORCE ? kModeEnforce : kModeLog,
                            kName);
    if (err) {
        printf("could not start: error %ld (%s)\n", err,
               err == kEngineNoSlot ? "Exception Manager system slot not "
                                      "found; not 9.2.2?" :
               err == kEngineNoMemory ? "out of memory" :
               err == kEngineNoArea ? "no NanoKernel area" :
               err == kEngineBusy ? "XLog, Guard or Enforcer is running" :
               "a write hook would not install");
        getchar();
        return 1;
    }
    printf("state %08lX, my partition %08lX-%08lX; to stop, bring %s to "
           "the front and press a key\n", (unsigned long)s, s->my_lo,
           s->my_hi, kName);
    if (XLOG_SYSHEAP) {
        engine_set_sys(s, 1);
        SysPages *sp = s->sys;
        if (!s->sys_on)
            printf("could not watch the system heap\n");
        else
            printf("system heap %08lX-%08lX: %lu pages, %lu of the engine's "
                   "own, %lu not read/write\n", sp->lo, sp->lo + sp->n * 4096,
                   sp->n, sp->c.excluded, sp->c.not_rw);
    }
    for (int i = 0; i < kMaxDomains; i++)
        if (s->domains[i].live && s->domains[i].size)
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
            engine_poll(s);
            last_refresh = now;
        }
        if (now - last_status >= kStatusTicks) {
            status(s);
            last_status = now;
        }
        show_offenses(s, &shown);
    }

    engine_stop(s);
    WindowPtr w = FrontWindow();
    if (w) {
        SetPort(w);
        InvalRect(&w->portRect);
    }
    report(s);
    printf("\n(Press Return)\n");
    getchar();
    DisposePtr((Ptr)s->bitmap);
    if (s->sys)
        DisposePtr((Ptr)s->sys);
    DisposePtr((Ptr)s);
    return 0;
}
