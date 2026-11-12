/*
 * engine.c: the protection engine (see engine.h).
 *
 * The timer task and the fallback handler run at interrupt time and use no
 * Toolbox calls other than the NanoKernel's and PrimeTime. Everything that
 * reads the process list runs from the program's event loop.
 */

#include <stddef.h>
#include <string.h>
#include <MacMemory.h>
#include <Processes.h>
#include <Timer.h>
#include <MixedMode.h>

#include "engine.h"
#include "nk.h"
#include "hook.h"
#include "h68k.h"
#include "hnat.h"

enum { kTimerMs = 16, kBusErrorVector = 0x08 };

/* The bitmap covers the whole blue area, one bit per page. */
#define kSpanLo 0UL
#define kSpanHi 0x40000000UL
#define kBitmapBytes ((kSpanHi - kSpanLo) / kPage / 8)
#define kPageMask (~(unsigned long)(kPage - 1))

static State *g_state;
static ExceptionHandler g_prev;
static InstallFn g_sys_install;

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

static unsigned long now_seconds(void)
{
    return lowmem_long(0x20C); /* Time: seconds since 1904 */
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

/* Protect pages of domain i, unless protection is off or the domain is
   going away. */
static void protect(State *s, int i, unsigned long lo, unsigned long len)
{
    Domain *d = &s->domains[i];
    if (s->mode == kModeOff || !d->live || d->dying)
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
        copy_name(out, s->name);
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
    o->time = now_seconds();
    o->pc = pc;
    o->addr = addr;
    o->source = src;
    o->denied = s->mode == kModeEnforce && src != kSrcFallback;
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

/* Clean stop ---------------------------------------------------------- */

#define HI16(x) ((unsigned short)((x) >> 16))
#define LO16(x) ((unsigned short)((x) & 0xFFFF))

/* Put the 68k stub in front of the bus error vector. The stub runs on the
   application's stack with the exception frame on top (SR first):

     move.l  d0,-(sp)
     move.l  zone,d0          ; a hook denied a store...
     beq.s   pass
     cmp.l   ApplZone.w,d0    ; ...of this application
     bne.s   pass
     clr.l   zone
     move.w  4(sp),d0         ; the frame's SR
     andi.w  #$0700,d0        ; interrupt mask: not at task level
     bne.s   pass
     addq.l  #1,stops68k
     move.w  #$2000,sr
     _ExitToShell             ; never returns
     bra.s   *
   pass:
     move.l  (sp)+,d0
     move.l  old_vector,-(sp) ; every register as it arrived
     rts
*/
static Stopper *stopper_install(void)
{
    Stopper *st = (Stopper *)NewPtrSysClear(sizeof(Stopper));
    if (!st)
        return 0;
    unsigned long z = (unsigned long)&st->zone;
    unsigned long n = (unsigned long)&st->stops[kSrc68k];
    unsigned long o = (unsigned long)&st->old_vector;
    const unsigned short code[kStopperCode] = {
        0x2F00,                       /* 00 */
        0x2039, HI16(z), LO16(z),     /* 02 */
        0x6724,                       /* 08 beq.s 2E */
        0xB0B8, 0x02AA,               /* 0A */
        0x661E,                       /* 0E bne.s 2E */
        0x42B9, HI16(z), LO16(z),     /* 10 */
        0x302F, 0x0004,               /* 16 */
        0x0240, 0x0700,               /* 1A */
        0x660E,                       /* 1E bne.s 2E */
        0x52B9, HI16(n), LO16(n),     /* 20 */
        0x46FC, 0x2000,               /* 26 */
        0xA9F4,                       /* 2A */
        0x60FE,                       /* 2C */
        0x201F,                       /* 2E */
        0x2F39, HI16(o), LO16(o),     /* 30 */
        0x4E75                        /* 36 */
    };
    st->magic = kStopperMagic;
    BlockMove(code, st->code, sizeof code); /* flushes the code caches */
    volatile unsigned long *vec = lowmem(kBusErrorVector);
    st->old_vector = *vec;
    *vec = (unsigned long)st->code;
    return st;
}

/* Restore the vector. If another program has patched it since, it chains
   to the stub, so the stub stays: with zone 0 it passes everything on. */
static void stopper_remove(State *s)
{
    Stopper *st = s->stopper;
    if (!st)
        return;
    st->zone = 0;
    volatile unsigned long *vec = lowmem(kBusErrorVector);
    if (*vec == (unsigned long)st->code) {
        *vec = st->old_vector;
        s->stopper = 0;
        DisposePtr((Ptr)st);
    }
}

/* If a hook has just denied a store by the current application, make the
   fault resume at ExitToShell, in the application's own context. */
static int stop_native(State *s, ExceptionInfo *e)
{
    Stopper *st = s->stopper;
    if (!st || !st->zone || st->zone != current_zone() || !e->registerImage)
        return 0;
    st->zone = 0;
    st->stops[kSrcNative]++;
    const unsigned long *tv = (const unsigned long *)(void *)ExitToShell;
    e->machineState->PC.lo = tv[0];
    e->machineState->LR.lo = 0; /* never returns */
    e->registerImage->R[2].lo = tv[1];
    e->registerImage->R[12].lo = (unsigned long)tv;
    return 1;
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
    /* A store the native hook denied (it is in its ring already): stop
       the application, or let the system do it. */
    if (app && s->hnat->enforce) {
        if (stop_native(s, e))
            return 0;
        return g_prev ? g_prev(e) : -1;
    }
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

void engine_set_mode(State *s, unsigned long mode)
{
    if (mode > kModeEnforce)
        mode = kModeEnforce;
    unsigned long old = s->mode;
    s->want_mode = mode;
    if (mode == old)
        return;
    if (mode == kModeOff) {
        /* After mode is off, the timer protects nothing, so every page
           opened here stays open. */
        s->h68k->enforce = 0;
        s->hnat->enforce = 0;
        s->mode = kModeOff;
        for (int i = 0; i < kMaxDomains; i++) {
            Domain *d = &s->domains[i];
            if (d->live && nk_unprotect(s->area, d->lo, d->hi - d->lo))
                s->c.kernel_errors++;
        }
        return;
    }
    s->h68k->enforce = mode == kModeEnforce;
    s->hnat->enforce = mode == kModeEnforce;
    s->mode = mode;
    if (old == kModeOff)
        protect_others(s);
}

long engine_start(State **out, unsigned long mode, const char *name)
{
    *out = 0;
    /* Another engine owns the hooks. */
    if ((h68k_installed() && h68k_data()->owner) ||
        (hnat_installed() && hnat_data()->owner))
        return kEngineBusy;
    InstallFn sys_install = nk_find_system_installer();
    if (!sys_install)
        return kEngineNoSlot;
    State *s = (State *)NewPtrSysClear(sizeof(State));
    unsigned char *bitmap = (unsigned char *)NewPtrSysClear(kBitmapBytes);
    UniversalProcPtr timer_upp = NewRoutineDescriptor(
        (ProcPtr)timer_proc, uppTimerProcInfo, kPowerPCISA);
    if (!s || !bitmap || !timer_upp) {
        if (s)
            DisposePtr((Ptr)s);
        if (bitmap)
            DisposePtr((Ptr)bitmap);
        return kEngineNoMemory;
    }
    s->magic = kEngineMagic;
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
    s->off_mode = offsetof(State, mode);
    s->off_stopper = offsetof(State, stopper);
    s->mode = s->want_mode = kModeOff;
    copy_name(s->name, name);
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

    long err = 0;
    if (nk_area_from_address(s->my_lo, &s->area))
        err = kEngineNoArea;
    s->had68k = h68k_installed();
    s->hadnat = hnat_installed();
    if (!err && h68k_install(s->area))
        err = kEngineHook68k;
    if (!err && hnat_install(s->area)) {
        if (!s->had68k)
            h68k_uninstall();
        err = kEngineHookNative;
    }
    if (err) {
        DisposePtr((Ptr)bitmap);
        DisposePtr((Ptr)s);
        return err;
    }
    s->h68k = h68k_data();
    s->hnat = hnat_data();
    s->seen[kSrc68k] = s->h68k->count_handled;
    s->seen[kSrcNative] = s->hnat->count_handled;
    /* For tools/xlog-read.py. */
    s->h68k->owner = (unsigned long)s;
    s->hnat->owner = (unsigned long)s;

    /* Without memory for it, denied applications are stopped by Mac OS. */
    s->stopper = stopper_install();
    if (s->stopper) {
        s->h68k->stop = &s->stopper->zone;
        s->hnat->stop = &s->stopper->zone;
    }

    g_state = s;
    g_sys_install = sys_install;
    set_current(s, -1);
    hook_set_bitmap(s->h68k, kSpanLo, kSpanHi, bitmap);
    hook_set_bitmap(s->hnat, kSpanLo, kSpanHi, bitmap);
    g_prev = sys_install(handler);
    s->active = 1;
    BlockMoveData(timer_upp, &s->timer_rd, sizeof s->timer_rd);
    s->timer.tmAddr = (ProcPtr)&s->timer_rd;
    InsTime((QElemPtr)&s->timer);
    PrimeTime((QElemPtr)&s->timer, kTimerMs);
    s->running = 1;

    refresh_domains(s);
    engine_set_mode(s, mode);
    *out = s;
    return 0;
}

void engine_poll(State *s)
{
    if (!s->running)
        return;
    refresh_domains(s);
    if (s->want_mode != s->mode)
        engine_set_mode(s, s->want_mode);
}

void engine_stop(State *s)
{
    if (!s->running)
        return;
    /* Stop denying and protecting, stop the tick, take what the rings
       hold, then open everything before the hooks stop managing it. */
    s->h68k->enforce = 0;
    s->hnat->enforce = 0;
    s->mode = kModeOff;
    s->active = 0;
    RmvTime((QElemPtr)&s->timer);
    drain_all(s);
    s->h68k->stop = 0;
    s->hnat->stop = 0;
    stopper_remove(s);
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (!d->live)
            continue;
        d->dying = 1;
        if (nk_unprotect(s->area, d->lo, d->hi - d->lo))
            s->c.kernel_errors++;
    }
    g_sys_install(g_prev);

    s->hook_self[0] = s->h68k->count_self;
    s->hook_passed[0] = s->h68k->count_passed;
    s->hook_self[1] = s->hnat->count_self;
    s->hook_passed[1] = s->hnat->count_passed;
    hook_set_bitmap(s->h68k, 0, 0, 0);
    hook_set_bitmap(s->hnat, 0, 0, 0);
    hook_set_current(s->h68k, 0, 0);
    hook_set_current(s->hnat, 0, 0);
    s->h68k->owner = 0;
    s->hnat->owner = 0;
    if (!s->had68k)
        h68k_uninstall();
    if (!s->hadnat)
        hnat_uninstall();
    s->h68k = s->hnat = 0;
    g_state = 0;
    s->running = 0;
}

int engine_domain_at(State *s, unsigned long addr)
{
    return domain_at(s, addr);
}

void engine_writer_name(State *s, unsigned long zone, char out[16])
{
    writer_name(s, zone, out);
}
