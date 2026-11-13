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
#include <Gestalt.h>
#include <Processes.h>
#include <Timer.h>
#include <MixedMode.h>
#include <Traps.h>
#include <DriverServices.h>

#include "engine.h"
#include "nk.h"
#include "hook.h"
#include "h68k.h"
#include "hnat.h"

enum { kTimerMs = 16, kBusErrorVector = 0x08 };

/* SysPages.flags */
enum { kPageExcluded = 1, kPageHot = 2, kPageCooled = 4 };

/* The bitmap covers logical memory up to 2 GB, one bit per page: Mac OS 9
   uses up to 1.5 GB of RAM, from 0, and the Process Manager places
   applications from the top of it down. */
#define kSpanLo 0UL
#define kSpanHi 0x80000000UL
/* With virtual memory on, how far above logical RAM the owner table goes:
   the Code Fragment Manager maps code files there (engine.h, kCodeOwned). */
#define kMappedSpan 0x10000000UL
#define kBitmapBytes ((kSpanHi - kSpanLo) / kPage / 8)
#define kPageMask (~(unsigned long)(kPage - 1))

static State *g_state;
static ExceptionHandler g_prev;
static InstallFn g_sys_install;
/* The program's own ExitToShell patch (self_exit) and the routine it
   chains to. */
static UniversalProcPtr g_exit_rd, g_exit_old;

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

/* With virtual memory on, keep (on) or stop keeping a block the engine
   uses at interrupt time or in the hooks in physical memory: a page fault
   there cannot be served. Nothing with virtual memory off. */
static void hold(State *s, const volatile void *p, unsigned long len, int on)
{
    if (!s->vm || !p || !len)
        return;
    if (on)
        HoldMemory((void *)p, len);
    else
        UnholdMemory((void *)p, len);
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

/* The live domain whose protected pages hold addr: its partition's or one
   of its temporary memory chunks'. */
static int domain_at(State *s, unsigned long addr)
{
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (d->live && addr >= d->lo && addr < d->hi)
            return i;
    }
    for (int i = 0; i < kMaxChunks; i++) {
        Chunk *c = &s->chunks[i];
        if (c->live && addr >= c->lo && addr < c->hi)
            return c->domain;
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

/* As domain_of_zone, or else the domain dropped last whose partition held
   addr (engine.h, Domain.dropped). For naming only. */
static int domain_of_zone_any(State *s, unsigned long addr)
{
    int found = domain_of_zone(s, addr);
    if (found >= 0)
        return found;
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (!d->live && d->dropped && d->name[0] && addr >= d->location &&
            addr - d->location < d->size &&
            (found < 0 || d->dropped > s->domains[found].dropped))
            found = i;
    }
    return found;
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

/* Domain i's byte in the kernel's owner table. */
static unsigned char owner_code(State *s, int i)
{
    return i == s->sys_dom ? kOwnerSystem : kOwnerApp + i;
}

/* Manage the pages in [lo, hi) for owner (bitmap bits and owner bytes), or
   stop managing them (owner 0). */
static void mark(State *s, unsigned long lo, unsigned long hi,
                 unsigned char owner)
{
    set_bits(s, lo, hi, owner != kOwnerNone);
    if (!s->kcb)
        return;
    unsigned long n = s->kcb->entries;
    for (unsigned long p = lo >> 12; p < hi >> 12 && p < n; p++)
        s->owners[p] = owner;
}

/* Give application domain i the owner bytes of its code's pages in
   [lo, hi) that are outside its partition and have no owner (engine.h,
   kCodeOwned). Only the owner table changes. */
static void own_code(State *s, int i, unsigned long lo, unsigned long hi)
{
    Domain *d = &s->domains[i];
    unsigned char code = owner_code(s, i);
    unsigned long n = s->kcb->entries;
    lo &= kPageMask;
    hi = (hi + kPage - 1) & kPageMask;
    for (unsigned long p = lo >> 12; p < hi >> 12 && p < n; p++) {
        unsigned long a = p << 12;
        if ((a < d->lo || a >= d->hi) && s->owners[p] == kOwnerNone)
            s->owners[p] = code;
    }
    d->code_lo = lo;
    d->code_hi = hi;
    d->code_state = kCodeOwned;
}

/* Give back the owner bytes of domain i's code. The range stays in the
   slot (engine.h, Domain.dropped). */
static void disown_code(State *s, int i)
{
    Domain *d = &s->domains[i];
    if (d->code_state != kCodeOwned || !s->kcb)
        return;
    unsigned char code = owner_code(s, i);
    unsigned long n = s->kcb->entries;
    for (unsigned long p = d->code_lo >> 12; p < d->code_hi >> 12 && p < n;
         p++)
        if (s->owners[p] == code)
            s->owners[p] = kOwnerNone;
    d->code_state = kCodeUnknown;
}

/* Protect pages of application domain i, unless protection is off or the
   domain is going away. The system heap's pages go through sys_protect_*
   instead, which leave out its excluded and hot pages. */
static void protect(State *s, int i, unsigned long lo, unsigned long len)
{
    Domain *d = &s->domains[i];
    if (s->mode == kModeOff || !d->live || d->dying || i == s->sys_dom)
        return;
    if (nk_protect(s->area, lo, len))
        s->c.kernel_errors++;
}

static void sys_protect_all(State *s);
static void sys_runs(State *s, unsigned long from, unsigned long to,
                     unsigned char skip, int on);

static void protect_domain(State *s, int i)
{
    if (i == s->sys_dom) {
        sys_protect_all(s);
        return;
    }
    protect(s, i, s->domains[i].lo, s->domains[i].hi - s->domains[i].lo);
    for (int j = 0; j < kMaxChunks; j++) {
        Chunk *c = &s->chunks[j];
        if (c->live && c->domain == i)
            protect(s, i, c->lo, c->hi - c->lo);
    }
}

static void unprotect_domain(State *s, int i)
{
    Domain *d = &s->domains[i];
    if (i == s->sys_dom) {
        sys_runs(s, 0, s->sys->n, kPageExcluded, 0);
        return;
    }
    if (nk_unprotect(s->area, d->lo, d->hi - d->lo))
        s->c.kernel_errors++;
    for (int j = 0; j < kMaxChunks; j++) {
        Chunk *c = &s->chunks[j];
        if (c->live && c->domain == i &&
            nk_unprotect(s->area, c->lo, c->hi - c->lo))
            s->c.kernel_errors++;
    }
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
    if (s->kcb)
        s->kcb->current = i >= 0 ? owner_code(s, i) : kOwnerNone;
    s->cur = i;
}

/* Stop managing a chunk's pages, before its memory is freed. Safety rule
   (engine.h): not live first, so that nothing protects it again, then
   open its pages, then clear their bits and owners. */
static void drop_chunk(State *s, Chunk *c)
{
    c->live = 0;
    if (nk_unprotect(s->area, c->lo, c->hi - c->lo))
        s->c.kernel_errors++;
    mark(s, c->lo, c->hi, kOwnerNone);
}

/* Stop managing application domain i and its chunks: its process is gone,
   or going (quit: keep the slot while the process is still listed, so
   that the next refresh does not add it again). */
static void drop_domain(State *s, int i, int quit)
{
    Domain *d = &s->domains[i];
    d->dying = 1;
    if (s->cur == i) {
        set_current(s, -1);
        s->cur_zone = 0; /* the next tick looks again */
    }
    for (int j = 0; j < kMaxChunks; j++) {
        Chunk *c = &s->chunks[j];
        if (c->live && c->domain == i) {
            drop_chunk(s, c);
            s->pc.chunks_dropped++;
        }
    }
    if (nk_unprotect(s->area, d->lo, d->hi - d->lo))
        s->c.kernel_errors++;
    mark(s, d->lo, d->hi, kOwnerNone);
    disown_code(s, i);
    d->quit = quit;
    d->live = 0;
    s->c.removed++;
    d->dropped = s->c.removed;
}

/* Drop every domain and chunk whose master pointer no longer points at
   it: the Process Manager has freed its memory. Reads only memory, so it
   can run at interrupt time when that memory cannot be paged out (virtual
   memory off). */
static void sweep(State *s)
{
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (d->live && !d->dying && d->mp &&
            *(volatile unsigned long *)d->mp != d->location) {
            drop_domain(s, i, 1);
            s->pc.gone++;
        }
    }
    for (int i = 0; i < kMaxChunks; i++) {
        Chunk *c = &s->chunks[i];
        if (c->live && *(volatile unsigned long *)c->handle != c->block) {
            drop_chunk(s, c);
            s->pc.gone++;
        }
    }
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
    int w = domain_of_zone_any(s, zone);
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

/* What the hooks did with a store by the current application's code: the
   fallback handler records only the stores it let through. */
static int hook_denied(State *s, int src, unsigned long addr)
{
    return src != kSrcFallback && addr >= s->deny_from;
}

static void record_offense(State *s, int src, unsigned long pc,
                           unsigned long zone, int t, unsigned long addr,
                           int denied)
{
    unsigned long n = s->n_offenses + 1;
    Offense *o = &s->offenses[(n - 1) % kOffenseLog];
    o->seq = n;
    o->time = now_seconds();
    o->pc = pc;
    o->addr = addr;
    o->source = src;
    o->denied = denied;
    writer_name(s, zone, o->writer);
    copy_name(o->target, s->domains[t].name);
    s->c.app_code[src]++;
    s->n_offenses = n;
}

/* The system heap (see SysPages in engine.h) ---------------------------- */

/* The hooks deny app-code stores at or above deny_from: everything above
   the system heap in kModeEnforce (the system heap is log only), nothing
   otherwise. Change it before the system heap's pages are managed and
   after they no longer are. */
static void set_deny(State *s, unsigned long mode)
{
    unsigned long from = ~0UL;
    if (mode == kModeEnforce)
        from = s->sys_dom >= 0 ? s->domains[s->sys_dom].hi : 0;
    s->deny_from = from;
    s->h68k->deny_from = from;
    s->hnat->deny_from = from;
    /* The kernel's mode follows: kModeOff has nothing protected, and the
       kernel then only lets other tasks store (patch 3). */
    if (s->kcb)
        s->kcb->mode = mode == kModeEnforce ? kKcbEnforce
                       : mode == kModeLog   ? kKcbLog
                                            : kKcbPass;
}

static int sys_live(State *s)
{
    if (s->sys_dom < 0)
        return 0;
    Domain *d = &s->domains[s->sys_dom];
    return d->live && !d->dying;
}

/* addr is in a watched page. */
static int in_sys(State *s, unsigned long addr)
{
    return s->sys_dom >= 0 && addr >= s->sys->lo &&
           (addr - s->sys->lo) >> 12 < s->sys->n;
}

static void sys_protect_page(State *s, unsigned long p)
{
    SysPages *sp = s->sys;
    if (s->mode == kModeOff || !sys_live(s) || p >= sp->n ||
        (sp->flags[p] & (kPageExcluded | kPageHot)))
        return;
    if (nk_protect(s->area, sp->lo + p * kPage, kPage))
        s->c.kernel_errors++;
}

/* Protect (on) or open the pages in [from, to) that have none of the skip
   flags, one kernel call per run. */
static void sys_runs(State *s, unsigned long from, unsigned long to,
                     unsigned char skip, int on)
{
    SysPages *sp = s->sys;
    unsigned long p = from;
    while (p < to) {
        while (p < to && (sp->flags[p] & skip))
            p++;
        unsigned long q = p;
        while (q < to && !(sp->flags[q] & skip))
            q++;
        if (q > p) {
            unsigned long a = sp->lo + p * kPage, len = (q - p) * kPage;
            if (on ? nk_protect(s->area, a, len) : nk_unprotect(s->area, a, len))
                s->c.kernel_errors++;
        }
        p = q;
    }
}

static void sys_protect_all(State *s)
{
    if (s->mode == kModeOff || !sys_live(s))
        return;
    sys_runs(s, 0, s->sys->n, kPageExcluded | kPageHot, 1);
}

/* Leave page p open for good: a stack is in it. */
static void sys_exclude_stack(State *s, unsigned long p)
{
    SysPages *sp = s->sys;
    if (p >= sp->n || (sp->flags[p] & kPageExcluded))
        return;
    sp->flags[p] |= kPageExcluded;
    sp->c.stacks++;
    if (nk_unprotect(s->area, sp->lo + p * kPage, kPage))
        s->c.kernel_errors++;
}

/* Leave page p open for a while. Returns 0 if the hot list is full. */
static int sys_make_hot(State *s, unsigned long p)
{
    SysPages *sp = s->sys;
    if (sp->n_hot == kSysMaxHot) {
        sp->c.hot_full++;
        return 0;
    }
    if (sp->flags[p] & kPageCooled) {
        unsigned short quiet = (unsigned short)sp->tick - sp->cooled_at[p];
        if (quiet < kSysHotTicks) {
            if (sp->backoff[p] < kSysMaxBackoff)
                sp->backoff[p]++;
            sp->c.reheated++;
        } else if (quiet >= kSysCoolTicks) {
            sp->backoff[p] = 0;
        }
    }
    sp->flags[p] |= kPageHot;
    sp->streak[p] = 0;
    sp->hot[sp->n_hot].page = p;
    sp->hot[sp->n_hot].until =
        sp->tick + ((unsigned long)kSysHotTicks << sp->backoff[p]);
    sp->n_hot++;
    sp->c.hot++;
    return 1;
}

/* Protect again the hot pages whose time is up. */
static void sys_expire(State *s)
{
    SysPages *sp = s->sys;
    for (unsigned long i = 0; i < sp->n_hot;) {
        HotPage *h = &sp->hot[i];
        if ((long)(sp->tick - h->until) < 0) {
            i++;
            continue;
        }
        unsigned long p = h->page;
        *h = sp->hot[--sp->n_hot];
        sp->flags[p] = (sp->flags[p] & ~kPageHot) | kPageCooled;
        sp->cooled_at[p] = (unsigned short)sp->tick;
        sp->c.cooled++;
        sys_protect_page(s, p);
    }
}

/* A store into watched page addr, which the hook opened. */
static void sys_trapped(State *s, int src, volatile HookEvent *e,
                        unsigned long addr)
{
    SysPages *sp = s->sys;
    unsigned long p = (addr - sp->lo) >> 12;
    sp->c.traps++;
    if (e->pc & kHookAppCode) {
        unsigned long pc = src == kSrc68k ? hook_next_pc(e->r24)
                                          : e->pc & ~3UL;
        unsigned long lr = src == kSrc68k ? 0 : e->lr;
        sp->c.app_traps++;
        record(s, src | kWriterAppCode, pc, lr, e->zone, s->sys_dom, addr);
        record_offense(s, src, pc, e->zone, s->sys_dom, addr, 0);
    }
    /* The stack pointer at the store: its page, and the one below, which
       an exception frame pushed on the stack can reach. */
    unsigned long stack = e->sp;
    if (in_sys(s, stack)) {
        unsigned long q = (stack - sp->lo) >> 12;
        sys_exclude_stack(s, q);
        if (q > 0)
            sys_exclude_stack(s, q - 1);
    }
    if (sp->flags[p] & (kPageExcluded | kPageHot))
        return;
    unsigned short t = (unsigned short)sp->tick;
    if (sp->last[p] != t) {
        int next = sp->last[p] == (unsigned short)(t - 1);
        sp->streak[p] = next && sp->streak[p] < 255 ? sp->streak[p] + 1 : 1;
        sp->last[p] = t;
    }
    if (sp->streak[p] >= kSysStreak && sys_make_hot(s, p))
        return;
    sys_protect_page(s, p);
}

/* The system heap's end, in whole pages: the zone's bkLim, but below every
   application's partition and the engine's own, and inside cap pages. */
static unsigned long sys_heap_hi(State *s, unsigned long lo, unsigned long cap)
{
    unsigned long hi = lowmem_long(lowmem_long(0x2A6)) & kPageMask; /* bkLim */
    if (hi > lo + cap * kPage)
        hi = lo + cap * kPage;
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (i != s->sys_dom && d->live && d->location >= lo &&
            d->location < hi)
            hi = d->location & kPageMask;
    }
    if (s->my_lo >= lo && s->my_lo < hi)
        hi = s->my_lo & kPageMask;
    return hi < lo ? lo : hi;
}

static SysPages *sys_alloc(unsigned long cap)
{
    unsigned long head = (sizeof(SysPages) + 3) & ~3UL;
    char *b = NewPtrSysClear(head + cap * 7);
    if (!b)
        return 0;
    SysPages *sp = (SysPages *)b;
    sp->magic = kSysMagic;
    sp->cap = cap;
    sp->last = (unsigned short *)(b + head);
    sp->cooled_at = sp->last + cap;
    sp->streak = (unsigned char *)(sp->cooled_at + cap);
    sp->backoff = sp->streak + cap;
    sp->flags = sp->backoff + cap;
    return sp;
}

static void sys_exclude_block(State *s, const volatile void *ptr,
                              unsigned long size)
{
    SysPages *sp = s->sys;
    unsigned long a = (unsigned long)ptr, end = a + size;
    if (!a || !size || end <= sp->lo)
        return;
    unsigned long from = a < sp->lo ? 0 : (a - sp->lo) >> 12;
    unsigned long to = (end - sp->lo + kPage - 1) >> 12;
    if (to > sp->cap)
        to = sp->cap;
    for (unsigned long p = from; p < to; p++)
        if (!(sp->flags[p] & kPageExcluded)) {
            sp->flags[p] |= kPageExcluded;
            sp->c.excluded++;
        }
}

/* Leave out the pages in [from, to) that are not plain read/write. */
static void sys_check_access(State *s, unsigned long from, unsigned long to)
{
    SysPages *sp = s->sys;
    for (unsigned long p = from; p < to; p++) {
        unsigned long bits = 0;
        if (sp->flags[p] & kPageExcluded)
            continue;
        if (nk_get_access(s->area, sp->lo + p * kPage, &bits) ||
            !nk_access_rw(bits)) {
            sp->flags[p] |= kPageExcluded;
            sp->c.not_rw++;
        }
    }
}

/* Set the bitmap bits of the pages in [from, to) that are not excluded. */
static void sys_set_bits(State *s, unsigned long from, unsigned long to)
{
    SysPages *sp = s->sys;
    for (unsigned long p = from; p < to; p++)
        if (!(sp->flags[p] & kPageExcluded))
            mark(s, sp->lo + p * kPage, sp->lo + (p + 1) * kPage,
                 kOwnerSystem);
}

/* Start watching (event loop). Order, for the safety rule: the domain is
   live and deny_from set, then bits, then n, then protect. */
static void sys_start(State *s)
{
    if (s->sys_dom >= 0)
        return;
    int slot = -1;
    for (int i = 0; i < kMaxDomains && slot < 0; i++)
        if (!s->domains[i].live && !s->domains[i].quit)
            slot = i;
    if (slot < 0)
        return;
    unsigned long lo = (lowmem_long(0x2A6) + kPage - 1) & kPageMask;
    SysPages *sp = s->sys;
    if (!sp) {
        unsigned long cap = (sys_heap_hi(s, lo, kSysMaxPages) - lo) / kPage +
                            kSysSpare;
        if (cap > kSysMaxPages)
            cap = kSysMaxPages;
        sp = sys_alloc(cap);
        if (!sp)
            return;
        hold(s, sp, GetPtrSize((Ptr)sp), 1); /* the timer uses it */
        s->sys = sp;
    } else {
        /* Watched before: keep the counters, start the pages afresh. */
        memset(sp->last, 0, sp->cap * 7);
        sp->n_hot = 0;
    }
    sp->lo = lo;
    sp->n = 0;

    /* The blocks the hooks write while handling a fault (a fault there
       would be fatal), and those the timer writes. */
    sys_exclude_block(s, s, sizeof(State));
    sys_exclude_block(s, s->bitmap, kBitmapBytes);
    sys_exclude_block(s, sp, GetPtrSize((Ptr)sp));
    sys_exclude_block(s, s->h68k->ring, kHookRing * sizeof(HookEvent));
    sys_exclude_block(s, (void *)s->hnat->block,
                      GetPtrSize((Ptr)s->hnat->block));
    sys_exclude_block(s, s->stopper, sizeof(Stopper));
    if (s->kcb) {
        sys_exclude_block(s, s->kcb_raw, GetPtrSize(s->kcb_raw));
        sys_exclude_block(s, s->owners_raw, GetPtrSize(s->owners_raw));
    }
    unsigned long hi = sys_heap_hi(s, lo, sp->cap), n = (hi - lo) / kPage;
    sys_check_access(s, 0, n);

    Domain *d = &s->domains[slot];
    memset(d, 0, sizeof *d);
    d->lo = lo;
    d->hi = hi;
    copy_name(d->name, "System heap");
    d->seen = 1;
    d->live = 1;
    s->sys_dom = slot;
    set_deny(s, s->mode);
    sys_set_bits(s, 0, n);
    sp->n = n;
    s->sys_on = 1;
    sys_protect_all(s);
}

/* Stop watching (event loop): dying, open, bits, then dead. */
static void sys_stop(State *s)
{
    if (s->sys_dom < 0)
        return;
    SysPages *sp = s->sys;
    Domain *d = &s->domains[s->sys_dom];
    d->dying = 1;
    sys_runs(s, 0, sp->n, kPageExcluded, 0);
    mark(s, sp->lo, sp->lo + sp->n * kPage, kOwnerNone);
    sp->n = 0;
    d->live = 0;
    s->sys_dom = -1;
    s->sys_on = 0;
    set_deny(s, s->mode);
}

/* Follow the heap as it grows or shrinks (event loop). */
static void sys_follow(State *s)
{
    if (!sys_live(s))
        return;
    SysPages *sp = s->sys;
    Domain *d = &s->domains[s->sys_dom];
    unsigned long hi = sys_heap_hi(s, sp->lo, sp->cap);
    unsigned long n = (hi - sp->lo) / kPage, old = sp->n;
    if (n > old) {
        sys_check_access(s, old, n);
        d->hi = hi;
        set_deny(s, s->mode);
        sys_set_bits(s, old, n);
        sp->n = n;
        if (s->mode != kModeOff)
            sys_runs(s, old, n, kPageExcluded | kPageHot, 1);
        sp->c.grown++;
    } else if (n < old) {
        /* The timer stops at n before the pages are opened. */
        sp->n = n;
        d->hi = hi;
        sys_runs(s, n, old, kPageExcluded, 0);
        mark(s, sp->lo + n * kPage, sp->lo + old * kPage, kOwnerNone);
        for (unsigned long p = n; p < old; p++)
            sp->flags[p] = 0;
        set_deny(s, s->mode);
        sp->c.shrunk++;
    }
}

void engine_set_sys(State *s, int on)
{
    s->want_sys = on != 0;
    if (on)
        sys_start(s);
    else
        sys_stop(s);
    s->want_sys = s->sys_on;
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
        if (sys_live(s))
            s->sys->c.resweeps++;
        protect_others(s);
    }
    for (unsigned long i = 0; i < n; i++) {
        volatile HookEvent *e = &ring[i];
        if (e->seq - seen - 1 >= handled - seen)
            continue;
        unsigned long addr = e->addr, zone = e->zone;
        if (in_sys(s, addr)) {
            sys_trapped(s, src, e, addr);
            continue;
        }
        int app = (e->pc & kHookAppCode) != 0;
        /* The hooks know only the partition: native code outside it
           (engine.h, kCodeOwned) is the application's too, also when the
           application has quit since (Domain.dropped). */
        if (!app && src != kSrc68k) {
            int w = domain_of_zone_any(s, zone);
            unsigned long pc = e->pc & ~3UL;
            const Domain *wd = w >= 0 ? &s->domains[w] : 0;
            app = wd && (wd->code_state == kCodeOwned || !wd->live) &&
                  pc >= wd->code_lo && pc < wd->code_hi;
        }
        int t = domain_at(s, addr);
        if (t < 0 || t == s->sys_dom) {
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
                record_offense(s, src, pc, zone, t, addr,
                               src == kSrcNative && (e->pc & kHookSkipped)
                                   ? kOffenseBlocked
                                   : hook_denied(s, src, addr));
        }
        if (t != s->cur)
            protect(s, t, addr & kPageMask, kPage);
    }
    s->seen[src] = handled;
}

/* The zone a kernel event's store is logged under: the writing code's
   application, or for system code the application current at the store,
   or the system heap. */
static unsigned long kernel_zone(State *s, unsigned writer, unsigned current)
{
    unsigned owner = writer >= kOwnerApp ? writer : current;
    if (owner >= kOwnerApp && owner - kOwnerApp < kMaxDomains) {
        Domain *d = &s->domains[owner - kOwnerApp];
        if (d->live || d->dropped) /* engine.h, Domain.dropped */
            return d->location;
    }
    return lowmem_long(0x2A6); /* SysZone */
}

/* One store the kernel allowed or dropped and recorded, as the hooks'
   drain does with theirs. The writer is known from the store's code here,
   not from ApplZone: a store is the target's own when its code is the
   target's, or when system code stores into the application current at the
   time. A dropped store never opened its page. */
static void kernel_event(State *s, const KEvent *e)
{
    unsigned flags = kev_flags(e->info), writer = kev_writer(e->info);
    unsigned current = (e->info >> 8) & 0xFF;
    int src = flags & kEvf68k ? kSrc68k : kSrcNative;
    int app = (flags & kEvfOffense) != 0;
    int skipped = (flags & kEvfSkipped) != 0;
    unsigned long addr = e->addr;
    HookEvent h;
    h.seq = e->seq;
    h.pc = (e->pc & ~3UL) | (app ? kHookAppCode : 0);
    h.addr = addr;
    h.r24 = e->r24;
    h.lr = e->lr;
    h.zone = kernel_zone(s, writer, current);
    h.r4 = 0;
    h.sp = e->sp;
    if (in_sys(s, addr)) {
        sys_trapped(s, src, &h, addr);
        return;
    }
    int t = domain_at(s, addr);
    if (t < 0 || t == s->sys_dom) {
        s->c.outside++;
        return;
    }
    unsigned code = writer >= kOwnerApp ? writer : current;
    if (code == owner_code(s, t)) {
        s->c.self[src]++;
        s->domains[t].self_writes++;
    } else {
        unsigned long pc = src == kSrc68k ? hook_next_pc(h.r24) : h.pc & ~3UL;
        unsigned long lr = src == kSrc68k ? 0 : h.lr;
        s->c.foreign[src]++;
        record(s, src | (app ? kWriterAppCode : 0), pc, lr, h.zone, t, addr);
        if (app)
            record_offense(s, src, pc, h.zone, t, addr,
                           skipped ? kOffenseBlocked : kOffenseAllowed);
    }
    if (t != s->cur && !skipped)
        protect(s, t, addr & kPageMask, kPage);
}

/* Take the stores the kernel recorded with seq in (kseen, kcb->seq]. A
   denied store is only counted: the hook that denies it records it. */
static void kernel_drain(State *s)
{
    KernelCB *k = s->kcb;
    if (!k)
        return;
    unsigned long last = k->seq, seen = s->kseen;
    if (last == seen)
        return;
    if (last - seen > kKcbRing) {
        /* Pages opened by stores no longer in the ring. */
        s->klost += last - seen - kKcbRing;
        if (sys_live(s))
            s->sys->c.resweeps++;
        protect_others(s);
        seen = last - kKcbRing;
    }
    for (unsigned long n = seen + 1; n - seen <= last - seen; n++) {
        volatile KEvent *v = &k->ring[(n - 1) % kKcbRing];
        KEvent e;
        e.seq = v->seq;
        e.pc = v->pc;
        e.addr = v->addr;
        e.r24 = v->r24;
        e.lr = v->lr;
        e.sp = v->sp;
        e.info = v->info;
        e.task = v->task;
        if (e.seq != n || v->seq != n) {
            /* Overwritten while being read (a task's store). */
            s->klost++;
            protect_others(s);
            continue;
        }
        s->kevents++;
        if (kev_flags(e.info) & kEvfDenied)
            s->kdenied++;
        else
            kernel_event(s, &e);
    }
    s->kseen = last;
}

static void drain_all(State *s)
{
    drain(s, kSrc68k, s->h68k->ring, kHookRing, s->h68k->count_handled);
    drain(s, kSrcNative, s->hnat->ring, kHookRing, s->hnat->count_handled);
    drain(s, kSrcFallback, s->fb_ring, kFallbackRing, s->fb_count);
    kernel_drain(s);
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

/* A denied native store by code running above task level: a deferred task
   or a secondary interrupt handler that belongs to the current application.
   Stopping the application there would leave the interrupted code
   unfinished, as for a Time Manager task (the patched kernel drops those
   stores itself when the 68k interrupt mask is up, but deferred tasks run
   with it at 0). Skip the store instead and mark it, in the native hook's
   ring and in the offense log if the timer has drained it already, as
   blocked. A skipped stwcx. reports success, so a retry loop ends. */
static void skip_store(State *s, ExceptionInfo *e, unsigned long pc,
                       unsigned long addr)
{
    if (s->stopper)
        s->stopper->zone = 0;
    unsigned long insn = *(const unsigned long *)pc;
    if ((insn & 0xFC0007FF) == 0x7C00012D) /* stwcx. */
        e->machineState->CR = (e->machineState->CR & 0x0FFFFFFF) | 0x20000000;
    e->machineState->PC.lo = pc + 4;
    volatile HookData *h = s->hnat;
    volatile HookEvent *ring = h->ring;
    if (ring) {
        volatile HookEvent *ev = &ring[(h->ring_idx + kHookRing - 1) % kHookRing];
        if ((ev->pc & ~3UL) == pc && ev->addr == addr)
            ev->pc |= kHookSkipped;
    }
    unsigned long n = s->n_offenses;
    if (n) {
        Offense *o = &s->offenses[(n - 1) % kOffenseLog];
        if (o->pc == pc && o->addr == addr && o->denied == kOffenseStopped)
            o->denied = kOffenseBlocked;
    }
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
    if (!app && s->cur >= 0) {
        Domain *cd = &s->domains[s->cur];
        app = cd->code_state == kCodeOwned && pc >= cd->code_lo &&
              pc < cd->code_hi;
    }
    /* A store the native hook denied (it is in its ring already): stop
       the application, or let the system do it. The hook names the
       application in the stopper when it denies; the kernel can deny a
       store by the application's code outside its partition (in one of its
       temporary memory chunks), which the PC test does not see, and
       opening that page would only fault again. */
    if ((app && addr >= s->deny_from) ||
        (s->stopper && s->stopper->zone &&
         s->stopper->zone == current_zone())) {
        if (CurrentExecutionLevel() != kTaskLevel) {
            skip_store(s, e, pc, addr);
            return 0;
        }
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

static void pool_keep(State *s);

static void timer_proc(TMTask *tm)
{
    State *s = g_state;
    if (s && s->active) {
        s->c.timer_runs++;
        if (!s->vm)
            sweep(s);
        pool_keep(s);
        check_switch(s);
        if (s->sys_dom >= 0)
            s->sys->tick++;
        drain_all(s);
        if (sys_live(s))
            sys_expire(s);
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

/* A process with no name: use the creator code, as 'calc'. */
static void default_name(unsigned char *name, const ProcessInfoRec *info)
{
    if (name[0])
        return;
    unsigned long sig = info->processSignature;
    name[0] = 6;
    name[1] = name[6] = '\'';
    for (int i = 0; i < 4; i++)
        name[2 + i] = (sig >> (24 - 8 * i)) & 0xFF;
}

/* The partition's master pointer, or 0. The partition is a locked handle
   in the Process Manager's zone: its block header ends with the handle
   relative to that zone. */
static unsigned long partition_mp(State *s, unsigned long loc)
{
    if (!s->pm_zone)
        return 0;
    long rel = *(volatile long *)(loc - 4);
    if (rel < -0x1000000 || rel > 0x1000000 || (rel & 3))
        return 0;
    unsigned long mp = s->pm_zone + rel;
    return *(volatile unsigned long *)mp == loc ? mp : 0;
}

/* Make slot i a domain for process psn, described by info and name.
   Returns i, or -1 when its partition cannot be one. */
static int add_domain(State *s, int i, const ProcessSerialNumber *psn,
                      const ProcessInfoRec *info, const unsigned char *name)
{
    unsigned long loc = (unsigned long)info->processLocation;
    unsigned long lo = (loc + kPage - 1) & kPageMask;
    unsigned long hi = (loc + info->processSize) & kPageMask;
    if (i < 0 || hi <= lo || lo < kSpanLo || hi > kSpanHi ||
        (lo < s->my_hi && hi > s->my_lo))
        return -1;
    if (s->sys_dom >= 0 && lo < s->domains[s->sys_dom].hi &&
        hi > s->domains[s->sys_dom].lo)
        return -1; /* never: partitions are above the system heap */

    Domain *d = &s->domains[i];
    memset(d, 0, sizeof *d);
    d->lo = lo;
    d->hi = hi;
    d->location = loc;
    d->size = info->processSize;
    d->psn = *psn;
    d->mp = partition_mp(s, loc);
    set_name(d, name);
    d->seen = 1;
    d->live = 1;
    mark(s, lo, hi, owner_code(s, i));
    protect_domain(s, i);
    /* The current application: its zone has not changed, so make the next
       tick look again, or it never becomes current. */
    unsigned long z = current_zone();
    if (z >= loc && z - loc < d->size)
        s->cur_zone = 0;
    s->c.added++;
    return i;
}

static void refresh_domains(State *s)
{
    ProcessSerialNumber me, psn = {0, kNoProcess};
    GetCurrentProcess(&me);
    for (int i = 0; i < kMaxDomains; i++)
        if (i != s->sys_dom) /* not a process */
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

        int found = -1, free_slot = -1, quitting = 0;
        for (int i = 0; i < kMaxDomains; i++) {
            Domain *d = &s->domains[i];
            if (i == s->sys_dom)
                continue;
            if (d->quit && same_psn(&d->psn, &psn)) {
                d->seen = 1;
                quitting = 1;
            } else if (d->live && same_psn(&d->psn, &psn) &&
                       (!ok || d->location == loc))
                found = i;
            else if (!d->live && !d->quit && free_slot < 0)
                free_slot = i;
        }
        if (quitting)
            continue; /* dropped; its memory is being freed */
        if (!ok) {
            /* Keep a domain the Process Manager could not describe this
               time; add nothing. */
            s->c.info_errors++;
            if (found >= 0)
                s->domains[found].seen = 1;
            continue;
        }
        default_name(name, &info);
        if (found >= 0) {
            Domain *d = &s->domains[found];
            d->seen = 1;
            /* A process being launched has no name yet. */
            if (!d->name[0] && name[0])
                set_name(d, name);
            continue;
        }
        add_domain(s, free_slot, &psn, &info, name);
    }

    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (d->seen || i == s->sys_dom)
            continue;
        if (d->live)
            drop_domain(s, i, 0);
        d->quit = 0; /* the process is gone: the slot is free */
    }
    s->c.refreshes++;
}

/* Add the current process as a domain, if it is not one yet. Returns its
   slot, or -1. Event-time only. */
static int add_current(State *s)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    Str31 name;
    if (GetCurrentProcess(&psn))
        return -1;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processName = name;
    name[0] = 0;
    if (GetProcessInformation(&psn, &info))
        return -1;
    int free_slot = -1;
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (i == s->sys_dom)
            continue;
        if ((d->live || d->quit) && same_psn(&d->psn, &psn))
            return -1; /* a domain already, or quitting */
        if (!d->live && !d->quit && free_slot < 0)
            free_slot = i;
    }
    default_name(name, &info);
    return add_domain(s, free_slot, &psn, &info, name);
}

/* Temporary memory pools (engine.h) ------------------------------------- */

/* Mixed Mode procedure information for the stubs' calls into native code:
   C calling convention, 4-byte arguments. */
enum {
    kPoolNewInfo = 0xF1,       /* long (long frame) */
    kPoolDisposedInfo = 0x3C1, /* void (long zone, long err) */
    kPoolQuitInfo = 0x01       /* void (void) */
};

/* A trap's entry in the trap table: OS traps (0xA0xx) at 0x400, Toolbox
   traps (0xA8xx) at 0xE00. */
static volatile unsigned long *trap_entry(unsigned short trap)
{
    if (trap & 0x0800)
        return lowmem(0xE00 + 4 * (trap & 0x3FF));
    return lowmem(0x400 + 4 * (trap & 0xFF));
}

/* The 68k stack is only 2-byte aligned. */
static unsigned long get32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
           (unsigned long)p[2] << 8 | p[3];
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

/* A handle of size bytes in chunk c, or 0. */
static Handle chunk_alloc(Chunk *c, unsigned long size)
{
    THz z = (THz)c->zone;
    if ((unsigned long)z->zcbFree < size)
        return 0;
    THz old = GetZone();
    SetZone(z);
    Handle h = NewHandle(size);
    SetZone(old);
    return h;
}

/* A new chunk for domain d with room for a handle of size bytes, or 0.
   Called by d's application, in TempNewHandle, so the Process Manager
   makes the chunk that application's temporary memory. */
static Chunk *new_chunk(State *s, int d, unsigned long size)
{
    int slot = -1, kept = 0;
    for (int i = 0; i < kMaxChunks; i++) {
        Chunk *c = &s->chunks[i];
        if (!c->live && slot < 0)
            slot = i;
        else if (c->live && c->domain == d && c->keep)
            kept = 1;
    }
    unsigned long csize = (size + kChunkSlack + kPage - 1) & kPageMask;
    if (slot < 0 || csize < size || csize > 0x40000000UL)
        return 0;
    if (csize < kChunkMin)
        csize = kChunkMin;
    OSErr err = noErr;
    s->pool->inside = 1; /* the stub passes this one on */
    Handle h = TempNewHandle(csize + kPage, &err);
    s->pool->inside = 0;
    if (!h)
        return 0;
    MoveHHi(h);
    HLock(h);
    unsigned long b = (unsigned long)*h;
    unsigned long lo = (b + kPage - 1) & kPageMask;
    unsigned long hi = (b + csize + kPage) & kPageMask;
    if (hi > kSpanHi || (s->kcb && (hi >> 12) > s->kcb->entries)) {
        DisposeHandle(h);
        return 0;
    }
    THz z = (THz)(lo + 16);
    THz old = GetZone();
    InitZone(NULL, kChunkMasters, (Ptr)hi, z); /* makes z current */
    SetZone(old);
    unsigned long *sig = (unsigned long *)lo;
    sig[0] = kChunkSig;
    sig[1] = (unsigned long)z;

    Chunk *c = &s->chunks[slot];
    c->domain = d;
    c->handle = h;
    c->block = b;
    c->lo = lo;
    c->hi = hi;
    c->zone = (unsigned long)z;
    c->handles = 0;
    c->keep = !kept && csize == kChunkMin;
    mark(s, lo, hi, owner_code(s, d));
    c->live = 1;
    if (d != s->cur)
        protect(s, d, lo, hi - lo);
    s->pc.chunks_made++;
    return c;
}

/* TempNewHandle, from the _OSDispatch stub. frame is the 68k stack at the
   stub: the return address, the selector (a word), the OSErr pointer, the
   size, then the function result. Returns 0 when it has stored the
   result (the stub then returns to the caller), or 1 to pass the call on
   to the Process Manager. */
static long pool_new(unsigned long frame)
{
    State *s = g_state;
    if (!s || !s->running || !s->kcb || !s->pool)
        return 1;
    unsigned char *f = (unsigned char *)frame;
    OSErr *res = (OSErr *)get32(f + 6);
    long size = (long)get32(f + 10);
    sweep(s);
    int d = domain_of_zone(s, current_zone());
    if (d < 0)
        d = add_current(s);
    if (size < 0 || d < 0 || d == s->sys_dom || s->domains[d].dying) {
        s->pc.passed++;
        return 1;
    }
    Handle h = 0;
    Chunk *c = 0;
    for (int i = 0; i < kMaxChunks && !h; i++) {
        c = &s->chunks[i];
        if (c->live && c->domain == d)
            h = chunk_alloc(c, size);
    }
    if (!h) {
        c = new_chunk(s, d, size);
        if (c)
            h = chunk_alloc(c, size);
    }
    if (!h) {
        s->pc.passed++;
        return 1;
    }
    c->handles++;
    s->pc.news++;
    put32(f + 14, (unsigned long)h);
    if (res)
        *res = noErr;
    return 0;
}

/* After DisposeHandle of a handle in a chunk (the zone at zone), from its
   stub; err is DisposeHandle's result. Frees the chunk when it is empty,
   not kept, and the caller is its owner. */
static void pool_disposed(unsigned long zone, long err)
{
    State *s = g_state;
    if (!s || !s->running || (short)err)
        return;
    for (int i = 0; i < kMaxChunks; i++) {
        Chunk *c = &s->chunks[i];
        if (!c->live || c->zone != zone)
            continue;
        s->pc.disposed++;
        if (c->handles)
            c->handles--;
        if (c->handles || c->keep ||
            domain_of_zone(s, current_zone()) != c->domain)
            return;
        Handle h = c->handle;
        drop_chunk(s, c);
        s->pc.chunks_freed++;
        DisposeHandle(h);
        return;
    }
}

/* ExitToShell, from its stub, in the quitting application's context:
   drop its domain and chunks before the Process Manager frees them. When
   the quitting application is the program running the engine and it did
   not call engine_stop (it crashed), stop the engine: the timer, the
   handler and the stubs call its code, which is freed next. */
static void pool_quit(void)
{
    State *s = g_state;
    if (!s || !s->running)
        return;
    unsigned long zone = current_zone();
    if (zone >= s->my_lo && zone < s->my_hi) {
        engine_stop(s);
        return;
    }
    if (!s->pool || !s->pool->enabled)
        return;
    int d = domain_of_zone(s, zone);
    if (d < 0 || d == s->sys_dom)
        return;
    drop_domain(s, d, 1);
    s->pc.quits++;
}

/* The program running the engine quits without engine_stop (it crashed):
   stop the engine before the Process Manager frees the program's code,
   which the timer, the handler and the stubs call.
 *
 * This is the program's own patch of _ExitToShell, made with
 * NSetTrapAddress, so the Process Manager puts it in the trap table
 * whenever the program is current. The engine's pool stub cannot be relied
 * on here: the Process Manager can put another routine in the trap table
 * when the program becomes current, and the timer puts the stub back only
 * on its next tick (Scribble X: the program's ExitToShell, called right
 * after a switch, went around the stub). */
enum { kExitInfo = 0 }; /* pascal void (void) */

static pascal void self_exit(void)
{
    State *s = g_state;
    unsigned long zone = current_zone();
    if (s && s->running && zone >= s->my_lo && zone < s->my_hi)
        engine_stop(s);
    CallUniversalProc(g_exit_old, kExitInfo); /* never returns */
}

static void self_exit_install(void)
{
    g_exit_rd = NewRoutineDescriptor((ProcPtr)self_exit, kExitInfo,
                                     kPowerPCISA);
    if (!g_exit_rd)
        return;
    g_exit_old = NGetTrapAddress(_ExitToShell, kToolboxTrapType);
    NSetTrapAddress(g_exit_rd, _ExitToShell, kToolboxTrapType);
}

/* In the program's own context. */
static void self_exit_remove(void)
{
    if (g_exit_rd &&
        NGetTrapAddress(_ExitToShell, kToolboxTrapType) == g_exit_rd)
        NSetTrapAddress(g_exit_old, _ExitToShell, kToolboxTrapType);
}

/* An application's native code (engine.h, kCodeOwned) ------------------ */

static unsigned get16(const unsigned char *p)
{
    return (unsigned)p[0] << 8 | p[1];
}

static int read_at(int16_t ref, unsigned long off, void *buf, long len)
{
    LONGINT n = len;
    return SetFPos(ref, fsFromStart, (long)off) == noErr &&
           FSRead(ref, &n, buf) == noErr && n == len;
}

/* From the file's 'cfrg' 0 resource, read from its resource fork with the
   File Manager (the application has the fork open; a Resource Manager call
   could close it): the data fork offset, length and name of its PowerPC
   application fragment. Returns 0, or the step that failed. */
static int app_fragment(FSSpec *spec, unsigned long *off,
                        unsigned long *len, unsigned char *name)
{
    int16_t ref;
    unsigned char h[16], b[64];
    if (FSpOpenRF(spec, fsRdPerm, &ref))
        return 1;
    int r = 2;
    if (!read_at(ref, 0, h, 16))
        goto out;
    unsigned long data = get32(h), map = get32(h + 4);
    if (!read_at(ref, map + 24, b, 2))
        goto out;
    unsigned long types = map + get16(b);
    if (!read_at(ref, types, b, 2))
        goto out;
    unsigned ntypes = (get16(b) + 1) & 0xFFFF;
    r = 3;
    for (unsigned i = 0; i < ntypes && i < 512; i++) {
        if (!read_at(ref, types + 2 + 8 * i, b, 8))
            goto out;
        if (get32(b) != 'cfrg')
            continue;
        unsigned count = (get16(b + 4) + 1) & 0xFFFF;
        unsigned long refs = types + get16(b + 6);
        for (unsigned j = 0; j < count && j < 512; j++) {
            if (!read_at(ref, refs + 12 * j, b, 12))
                goto out;
            if (get16(b))
                continue; /* not ID 0 */
            unsigned long at = data + (get32(b + 4) & 0xFFFFFF);
            r = 4;
            if (!read_at(ref, at + 4, b, 32))
                goto out;
            unsigned long members = get32(b + 28);
            at += 4 + 32;
            r = 5;
            for (unsigned long k = 0; k < members && k < 64; k++) {
                if (!read_at(ref, at, b, 64))
                    goto out;
                unsigned size = get16(b + 40);
                /* PowerPC, an application, in the data fork */
                if (get32(b) == 'pwpc' && b[22] == 1 && b[23] == 1) {
                    *off = get32(b + 24);
                    *len = get32(b + 28);
                    memcpy(name, b + 42, b[42] < 21 ? b[42] + 1 : 0);
                    if (b[42] >= 21) /* longer than this read */
                        name[0] = 0;
                    r = 0;
                    goto out;
                }
                if (size < 43)
                    goto out;
                at += size;
            }
            goto out;
        }
    }
    r = -1; /* no 'cfrg' resource: a 68k application */
out:
    FSClose(ref);
    return r;
}

/* PEF pattern-initialized data: the 8 bytes (a transition vector) at
   offset want of the unpacked section. */
typedef struct {
    unsigned long pos, want;
    unsigned char w[8];
} Unpack;

static void emit(Unpack *u, const unsigned char *src, unsigned long count)
{
    if (u->pos + count <= u->want || u->pos >= u->want + 8) {
        u->pos += count;
        return;
    }
    for (unsigned long i = 0; i < count; i++, u->pos++)
        if (u->pos >= u->want && u->pos < u->want + 8)
            u->w[u->pos - u->want] = src ? src[i] : 0;
}

static unsigned long pidata_arg(const unsigned char **p,
                                const unsigned char *end)
{
    unsigned long v = 0;
    for (int i = 0; i < 5 && *p < end; i++) {
        unsigned char b = *(*p)++;
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80))
            break;
    }
    return v;
}

static int pidata_tvector(const unsigned char *p, unsigned long n,
                          unsigned long want, unsigned long tv[2])
{
    const unsigned char *end = p + n;
    Unpack u = {0, want, {0}};
    while (p < end && u.pos < want + 8) {
        unsigned op = *p >> 5;
        unsigned long count = *p++ & 31, custom, rep;
        if (!count)
            count = pidata_arg(&p, end);
        switch (op) {
        case 0: /* zero */
            emit(&u, 0, count);
            break;
        case 1: /* block copy */
            if (count > (unsigned long)(end - p))
                return 0;
            emit(&u, p, count);
            p += count;
            break;
        case 2: /* repeated block */
            rep = pidata_arg(&p, end) + 1;
            if (count > (unsigned long)(end - p))
                return 0;
            for (unsigned long r = 0; r < rep && u.pos < want + 8; r++)
                if (u.pos + count * (rep - r) <= want) {
                    u.pos += count * (rep - r);
                    break;
                } else
                    emit(&u, p, count);
            p += count;
            break;
        case 3: /* repeated custom blocks with common data */
        case 4: /* and with zeros */
        {
            custom = pidata_arg(&p, end);
            rep = pidata_arg(&p, end);
            const unsigned char *common = op == 3 ? p : 0;
            if (op == 3) {
                if (count > (unsigned long)(end - p))
                    return 0;
                p += count;
            }
            emit(&u, common, count);
            for (unsigned long r = 0; r < rep && u.pos < want + 8; r++) {
                if (custom > (unsigned long)(end - p))
                    return 0;
                emit(&u, p, custom);
                p += custom;
                emit(&u, common, count);
            }
            break;
        }
        default:
            return 0;
        }
    }
    if (u.pos < want + 8)
        return 0;
    tv[0] = get32(u.w);
    tv[1] = get32(u.w + 4);
    return 1;
}

/* What the PEF container at off in the data fork says about its main
   symbol, a transition vector in a data section: its offset in that
   section, its two words before relocation (the main routine's offset in
   the code section, and the TOC's offset in the data section), and the
   code section's length. Returns 0, or the step that failed. */
typedef struct {
    unsigned long main_off, raw_code, raw_toc, code_len;
    unsigned long align; /* the code section's, as a mask */
    unsigned char code_head[32], main_head[16]; /* the code section's first
                                                   bytes, and main's */
} PefMain;

static int pef_main(FSSpec *spec, unsigned long off, PefMain *m)
{
    int16_t ref;
    unsigned char h[40];
    if (FSpOpenDF(spec, fsRdPerm, &ref))
        return 6;
    int r = 7;
    if (!read_at(ref, off, h, 40) || get32(h) != 'Joy!' ||
        get32(h + 4) != 'peff' || get32(h + 8) != 'pwpc')
        goto out;
    unsigned nsec = get16(h + 32);
    unsigned long loader = 0, code_off = 0;
    int code = -1;
    r = 8;
    for (unsigned i = 0; i < nsec && i < 64; i++) {
        if (!read_at(ref, off + 40 + 28 * i, h, 28))
            goto out;
        if (h[24] == 0 && code < 0 && get32(h + 4) == 0) {
            code = (int)i;
            m->code_len = get32(h + 8);
            code_off = get32(h + 20);
            m->align = (1UL << (h[26] < 12 ? h[26] : 12)) - 1;
        } else if (h[24] == 4)
            loader = get32(h + 20);
    }
    if (code < 0 || !loader || !read_at(ref, off + loader, h, 8))
        goto out;
    unsigned long main_sec = get32(h), tv[2];
    m->main_off = get32(h + 4);
    r = 9;
    if (main_sec >= nsec ||
        !read_at(ref, off + 40 + 28 * main_sec, h, 28) ||
        get32(h + 4) != 0)
        goto out;
    unsigned long sec_off = get32(h + 20), sec_len = get32(h + 16);
    if (h[24] == 1) { /* unpacked data */
        unsigned char wb[8];
        if (!read_at(ref, off + sec_off + m->main_off, wb, 8))
            goto out;
        tv[0] = get32(wb);
        tv[1] = get32(wb + 4);
    } else if (h[24] == 2 && sec_len <= 0x100000) { /* pattern data */
        r = 10;
        Ptr buf = NewPtrSys((long)sec_len);
        if (!buf)
            goto out;
        int ok = read_at(ref, off + sec_off, buf, (long)sec_len) &&
                 pidata_tvector((unsigned char *)buf, sec_len, m->main_off,
                                tv);
        DisposePtr(buf);
        if (!ok)
            goto out;
    } else
        goto out;
    r = 11;
    if (m->code_len < 32 || tv[0] + 16 > m->code_len ||
        !read_at(ref, off + code_off, m->code_head, 32) ||
        !read_at(ref, off + code_off + tv[0], m->main_head, 16))
        goto out;
    m->raw_code = tv[0];
    m->raw_toc = tv[1];
    r = 0;
out:
    FSClose(ref);
    return r;
}

/* Whether len bytes at a are mapped and hold the bytes at want: they are
   held in physical memory while they are compared (an address with no
   page fails the hold, instead of faulting). */
static int memory_is(unsigned long a, const unsigned char *want,
                     unsigned long len)
{
    if (HoldMemory((void *)a, len))
        return 0;
    int same = memcmp((const void *)a, want, len) == 0;
    UnholdMemory((void *)a, len);
    return same;
}

/* The code section of domain i's main fragment, found from main's
   transition vector in its partition: at address a, the vector's second
   word is the data section's address (a - main_off) plus raw_toc, and its
   first word is the code section's address plus raw_code. A candidate
   counts only when that code section is aligned as the container says,
   above the system heap, inside the owner table, in no other application's
   partition, and holds the container's code bytes at its start and at
   main. Returns the code section's address, or 0. The data section is in
   the application heap, near its start; the search reads at most 32 MB. */
static unsigned long find_main(State *s, int i, const PefMain *m)
{
    const Domain *d = &s->domains[i];
    unsigned long sys_end = lowmem_long(lowmem_long(0x2A6)); /* bkLim */
    unsigned long top = s->kcb->entries << 12;
    unsigned long lo = (d->location + 3) & ~3UL;
    unsigned long hi = d->location + d->size;
    if (hi - lo > 0x2000000)
        hi = lo + 0x2000000;
    for (unsigned long a = lo; a + 8 <= hi; a += 4) {
        const unsigned long *v = (const unsigned long *)a;
        if (v[1] != a - m->main_off + m->raw_toc || v[0] < m->raw_code)
            continue;
        unsigned long base = v[0] - m->raw_code, end = base + m->code_len;
        if ((base & m->align) || base < sys_end || end > top || end < base ||
            (base < s->my_hi && end > s->my_lo))
            continue;
        int other = 0;
        for (int j = 0; j < kMaxDomains && !other; j++) {
            const Domain *o = &s->domains[j];
            other = j != i && o->live && base < o->location + o->size &&
                    end > o->location;
        }
        if (!other && memory_is(base, m->code_head, 32) &&
            memory_is(v[0], m->main_head, 16))
            return base;
    }
    return 0;
}

/* Find and own application domain i's native code. It reads the
   application's file and its partition, so it can run in any process.
   Event time. */
static void find_code(State *s, int i)
{
    Domain *d = &s->domains[i];
    if (!s->vm || !s->kcb) {
        d->code_state = kCodeInside;
        return;
    }
    d->code_state = kCodeFailed;
    ProcessInfoRec info;
    FSSpec spec;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    info.processAppSpec = &spec;
    if (GetProcessInformation(&d->psn, &info))
        return;
    unsigned long off, len;
    unsigned char name[64];
    int r = app_fragment(&spec, &off, &len, name);
    if (r) {
        d->code_state = r < 0 ? kCodeInside : kCodeFailed + r;
        return;
    }
    PefMain m;
    memset(&m, 0, sizeof m);
    r = pef_main(&spec, off, &m);
    if (r) {
        d->code_state = kCodeFailed + r;
        return;
    }
    unsigned long lo = find_main(s, i, &m);
    if (!lo) {
        d->code_state = kCodeFailed + 12;
        return;
    }
    if (lo >= d->location && lo - d->location < d->size) {
        d->code_state = kCodeInside;
        return;
    }
    own_code(s, i, lo, lo + m.code_len);
}

/* The GetNextEvent filter stub, when ApplZone has changed since the last
   such call: add the current application as a domain if it
   is not one yet (it has just been launched), and find its native code if
   that has not been looked for. */
static void pool_launch(void)
{
    State *s = g_state;
    PoolStubs *p = s ? s->pool : 0;
    if (!p)
        return;
    unsigned long zone = current_zone();
    p->last = zone;
    if (!s->running)
        return;
    int d = domain_of_zone(s, zone);
    if (d < 0) {
        if ((zone >= s->my_lo && zone < s->my_hi) ||
            zone == lowmem_long(0x2A6) || zone == s->pm_zone)
            return;
        d = add_current(s);
        if (d < 0)
            return;
        s->pc.launched++;
    }
    if (d != s->sys_dom && !s->domains[d].dying &&
        s->domains[d].code_state == kCodeUnknown)
        find_code(s, d);
}

/* Install the stubs, disabled. The stubs, as assembled:

   _OSDispatch:
     00 tst.w   enabled          ; off: pass
     06 beq.s   30
     08 tst.w   inside           ; the engine's own call: pass
     0E bne.s   30
     10 cmpi.w  #$001D,4(sp)     ; not TempNewHandle: pass
     16 bne.s   30
     18 pea     (sp)             ; pool_new(frame)
     1A movea.l #rd_new,a0
     20 jsr     (a0)
     22 addq.l  #4,sp
     24 tst.l   d0
     26 bne.s   30
     28 movea.l (sp)+,a0         ; taken: drop the selector and arguments
     2A lea     10(sp),sp
     2E jmp     (a0)
     30 move.l  osd_old,-(sp)
     36 rts

   DisposeHandle (A0 the handle; the trap dispatcher keeps A1, D1, D2):
     00 tst.w   enabled
     06 beq.s   4E
     08 move.l  a0,d2            ; no handle, or an empty one: pass
     0A beq.s   4E
     0C movea.l d2,a1
     0E move.l  (a1),d2
     10 beq.s   4E
     12 movea.l d2,a1
     14 move.l  a0,d2
     16 sub.l   -4(a1),d2        ; its zone: handle - relative handle
     1A movea.l d2,a1
     1C cmpi.l  #'EnfT',-16(a1)  ; not a chunk's zone: pass
     24 bne.s   4E
     26 cmpa.l  -12(a1),a1
     2A bne.s   4E
     2C move.l  a1,-(sp)         ; dispose, then pool_disposed(zone, d0)
     2E movea.l dh_old,a1
     34 jsr     (a1)
     36 move.l  d0,-(sp)
     38 move.l  4(sp),-(sp)
     3C movea.l #rd_disposed,a1
     42 jsr     (a1)
     44 addq.l  #4,sp
     46 move.l  (sp)+,d0
     48 addq.l  #4,sp
     4A tst.w   d0
     4C rts
     4E move.l  dh_old,-(sp)
     54 rts

   _ExitToShell:
     00 tst.w   quit_hook
     06 beq.s   10
     08 movea.l #rd_quit,a0      ; pool_quit()
     0E jsr     (a0)
     10 move.l  ets_old,-(sp)
     16 rts

   jGNEFilter (A1 the event record, the result in D0 and at 4(sp); every
   register kept):
     00 tst.w   enabled
     06 beq.s   24
     08 movem.l d0-d2/a0-a1,-(sp)
     0C move.l  $02AA.w,d0       ; ApplZone unchanged: nothing to do
     10 cmp.l   last,d0
     16 beq.s   20
     18 movea.l #rd_launch,a0    ; pool_launch()
     1E jsr     (a0)
     20 movem.l (sp)+,d0-d2/a0-a1
     24 tst.l   gne_old          ; on to the filter installed before, if any
     2A beq.s   34
     2C move.l  gne_old,-(sp)
     32 rts
     34 rts
*/
enum { kGNEFilter = 0x29A };

static PoolStubs *pool_install(void)
{
    PoolStubs *p = (PoolStubs *)NewPtrSysClear(sizeof(PoolStubs));
    UniversalProcPtr un = NewRoutineDescriptor((ProcPtr)pool_new,
                                               kPoolNewInfo, kPowerPCISA);
    UniversalProcPtr ud = NewRoutineDescriptor((ProcPtr)pool_disposed,
                                               kPoolDisposedInfo, kPowerPCISA);
    UniversalProcPtr uq = NewRoutineDescriptor((ProcPtr)pool_quit,
                                               kPoolQuitInfo, kPowerPCISA);
    UniversalProcPtr ul = NewRoutineDescriptor((ProcPtr)pool_launch,
                                               kPoolQuitInfo, kPowerPCISA);
    if (!p || !un || !ud || !uq || !ul) {
        if (p)
            DisposePtr((Ptr)p);
        return 0;
    }
    PoolStubs t;
    memset(&t, 0, sizeof t);
    t.magic = kPoolMagic;
    BlockMoveData(un, &t.rd_new, sizeof(RoutineDescriptor));
    BlockMoveData(ud, &t.rd_disposed, sizeof(RoutineDescriptor));
    BlockMoveData(uq, &t.rd_quit, sizeof(RoutineDescriptor));
    BlockMoveData(ul, &t.rd_launch, sizeof(RoutineDescriptor));
    DisposeRoutineDescriptor(un);
    DisposeRoutineDescriptor(ud);
    DisposeRoutineDescriptor(uq);
    DisposeRoutineDescriptor(ul);
    /* The trap table must be where the engine writes it. */
    volatile unsigned long *osd_e = trap_entry(_OSDispatch);
    volatile unsigned long *dh_e = trap_entry(_DisposeHandle);
    volatile unsigned long *ets_e = trap_entry(_ExitToShell);
    volatile unsigned long *gne_e = lowmem(kGNEFilter);
    t.osd_old = *osd_e;
    t.dh_old = *dh_e;
    t.ets_old = *ets_e;
    t.gne_old = *gne_e;
    if (t.osd_old != (unsigned long)NGetTrapAddress(_OSDispatch,
                                                    kToolboxTrapType) ||
        t.dh_old != (unsigned long)NGetTrapAddress(_DisposeHandle,
                                                   kOSTrapType) ||
        t.ets_old != (unsigned long)NGetTrapAddress(_ExitToShell,
                                                    kToolboxTrapType)) {
        DisposePtr((Ptr)p);
        return 0;
    }

    /* Addresses in the block the stubs run from. */
    unsigned long en = (unsigned long)&p->enabled;
    unsigned long in = (unsigned long)&p->inside;
    unsigned long oo = (unsigned long)&p->osd_old;
    unsigned long dold = (unsigned long)&p->dh_old;
    unsigned long eo = (unsigned long)&p->ets_old;
    unsigned long rn = (unsigned long)&p->rd_new;
    unsigned long rd = (unsigned long)&p->rd_disposed;
    unsigned long rq = (unsigned long)&p->rd_quit;
    const unsigned short osd[kPoolOsd] = {
        0x4A79, HI16(en), LO16(en), 0x6728,
        0x4A79, HI16(in), LO16(in), 0x6620,
        0x0C6F, 0x001D, 0x0004, 0x6618,
        0x4857, 0x207C, HI16(rn), LO16(rn), 0x4E90, 0x588F,
        0x4A80, 0x6608,
        0x205F, 0x4FEF, 0x000A, 0x4ED0,
        0x2F39, HI16(oo), LO16(oo), 0x4E75};
    const unsigned short dh[kPoolDh] = {
        0x4A79, HI16(en), LO16(en), 0x6746,
        0x2408, 0x6742, 0x2242, 0x2411, 0x673C,
        0x2242, 0x2408, 0x94A9, 0xFFFC, 0x2242,
        0x0CA9, 0x456E, 0x6654, 0xFFF0, 0x6628,
        0xB3E9, 0xFFF4, 0x6622,
        0x2F09, 0x2279, HI16(dold), LO16(dold), 0x4E91,
        0x2F00, 0x2F2F, 0x0004, 0x227C, HI16(rd), LO16(rd), 0x4E91,
        0x588F, 0x201F, 0x588F, 0x4A40, 0x4E75,
        0x2F39, HI16(dold), LO16(dold), 0x4E75};
    unsigned long qh = (unsigned long)&p->quit_hook;
    const unsigned short ets[kPoolEts] = {
        0x4A79, HI16(qh), LO16(qh), 0x6708,
        0x207C, HI16(rq), LO16(rq), 0x4E90,
        0x2F39, HI16(eo), LO16(eo), 0x4E75};
    memcpy(t.osd, osd, sizeof osd);
    memcpy(t.dh, dh, sizeof dh);
    memcpy(t.ets, ets, sizeof ets);
    unsigned long last = (unsigned long)&p->last;
    unsigned long rl = (unsigned long)&p->rd_launch;
    unsigned long go = (unsigned long)&p->gne_old;
    const unsigned short gne[kPoolGne] = {
        0x4A79, HI16(en), LO16(en), 0x671C,
        0x48E7, 0xE0C0,
        0x2038, 0x02AA,
        0xB0B9, HI16(last), LO16(last), 0x6708,
        0x207C, HI16(rl), LO16(rl), 0x4E90,
        0x4CDF, 0x0307,
        0x4AB9, HI16(go), LO16(go), 0x6708,
        0x2F39, HI16(go), LO16(go), 0x4E75,
        0x4E75};
    memcpy(t.gne, gne, sizeof gne);
    BlockMove(&t, p, sizeof t); /* flushes the code caches */
    *osd_e = (unsigned long)p->osd;
    *dh_e = (unsigned long)p->dh;
    *ets_e = (unsigned long)p->ets;
    *gne_e = (unsigned long)p->gne;
    return p;
}

/* Put back the stub at the trap table entry e (stub) if the Process
   Manager has put back the routine it goes on to (old). */
static void keep_one(State *s, volatile unsigned long *e, unsigned long old,
                     const unsigned short *stub)
{
    if (*e == old) {
        *e = (unsigned long)stub;
        s->pc.reinstalled++;
    }
}

/* Take out the stub at e if it is still first. */
static void remove_one(volatile unsigned long *e, unsigned long old,
                       const unsigned short *stub)
{
    if (*e == (unsigned long)stub)
        *e = old;
}

/* Put back a stub that the Process Manager has taken out of the table (it
   holds the stub's own next routine again). Interrupt time. */
static void pool_keep(State *s)
{
    PoolStubs *p = s->pool;
    if (!p) /* disabled stubs pass every call on */
        return;
    keep_one(s, trap_entry(_OSDispatch), p->osd_old, p->osd);
    keep_one(s, trap_entry(_DisposeHandle), p->dh_old, p->dh);
    keep_one(s, trap_entry(_ExitToShell), p->ets_old, p->ets);
}

/* Disable the stubs and take out those still first in their traps. The
   block stays (engine.h). */
static void pool_remove(State *s)
{
    PoolStubs *p = s->pool;
    if (!p)
        return;
    p->enabled = 0;
    p->quit_hook = 0;
    s->pool = 0;
    remove_one(lowmem(kGNEFilter), p->gne_old, p->gne);
    remove_one(trap_entry(_ExitToShell), p->ets_old, p->ets);
    remove_one(trap_entry(_DisposeHandle), p->dh_old, p->dh);
    remove_one(trap_entry(_OSDispatch), p->osd_old, p->osd);
}

void engine_set_mode(State *s, unsigned long mode)
{
    if (mode > kModeEnforce)
        mode = kModeEnforce;
    unsigned long old = s->mode;
    s->want_mode = mode;
    if (mode == old)
        return;
    /* Off leaves temporary memory as Mac OS has it: new handles come from
       the Process Manager again. Chunks already made stay with their
       applications. */
    if (s->pool)
        s->pool->enabled = mode != kModeOff;
    if (mode == kModeOff) {
        /* After mode is off, the timer protects nothing, so every page
           opened here stays open. */
        set_deny(s, kModeOff);
        s->mode = kModeOff;
        for (int i = 0; i < kMaxDomains; i++)
            if (s->domains[i].live)
                unprotect_domain(s, i);
        return;
    }
    set_deny(s, mode);
    s->mode = mode;
    if (old == kModeOff)
        protect_others(s);
}

/* The kernel's owner check (common/kcb.h): allocate the control block and
   the owner table in the system heap, where they outlive a program that
   stops without cleaning up, and register the block. On any failure the
   engine runs on its hooks alone, with the reason in kcb_err. Call with
   no domain marked yet. */
static void kcb_start(State *s)
{
    /* One byte per page of logical RAM, inside the bitmap's span; a table
       page covers kPage pages. (MemTop is no use: on 9.2.2 it ends a
       little above the system heap, below the applications.) */
    long lram = 0;
    unsigned long ram = kSpanHi;
    if (Gestalt(gestaltLogicalRAMSize, &lram) == noErr && lram > 0)
        ram = (unsigned long)lram;
    if (s->vm)
        ram = ram < kSpanHi - kMappedSpan ? ram + kMappedSpan : kSpanHi;
    if (ram > kSpanHi)
        ram = kSpanHi;
    unsigned long entries = (ram + kPage - 1) >> 12;
    unsigned long most = (unsigned long)kKcbTablePages * kPage;
    if (entries > most)
        entries = most;
    unsigned long tsize = (entries + kPage - 1) & kPageMask;
    char *kraw = NewPtrSysClear(kKcbSize + kPage);
    char *traw = NewPtrSysClear(tsize + kPage);
    long err = kEngineNoMemory;
    if (!kraw || !traw)
        goto fail;
    KernelCB *k = (KernelCB *)(((unsigned long)kraw + kPage - 1) & kPageMask);
    unsigned char *t =
        (unsigned char *)(((unsigned long)traw + kPage - 1) & kPageMask);
    err = kEngineNoArea;
    /* The kernel uses them by physical address: with virtual memory on,
       keep them in physical memory where they are, and make the block one
       physical range. */
    if (s->vm) {
        if (LockMemoryContiguous(k, kKcbSize))
            goto fail;
        if (LockMemory(t, tsize)) {
            UnlockMemory(k, kKcbSize);
            goto fail;
        }
    }
    unsigned long phys = kcb_physical(k, kKcbSize);
    if (!phys)
        goto unlock;
    for (unsigned long i = 0; i < tsize / kPage; i++) {
        unsigned long p = kcb_physical(t + i * kPage, kPage);
        if (!p)
            goto unlock;
        k->pages[i] = p;
    }
    k->magic = kKcbMagic;
    k->version = kKcbVersion;
    k->mode = kKcbPass;
    k->entries = entries;
    k->frame = s->h68k->frame;
    err = kcb_register(phys);
    if (err)
        goto unlock;
    s->kcb_raw = kraw;
    s->owners_raw = traw;
    s->owners = t;
    s->owners_size = tsize;
    s->kseen = 0;
    s->kcb = k;
    s->h68k->kdeny = &k->denied_page;
    s->hnat->kdeny = &k->denied_page;
    s->kcb_err = 0;
    return;
unlock:
    if (s->vm) {
        UnlockMemory(k, kKcbSize);
        UnlockMemory(t, tsize);
    }
fail:
    if (kraw)
        DisposePtr(kraw);
    if (traw)
        DisposePtr(traw);
    s->kcb_err = err;
}

/* Unregister and free the block, with nothing protected. */
static void kcb_stop(State *s)
{
    if (!s->kcb)
        return;
    s->kcb->mode = kKcbPass;
    kcb_register(0);
    s->h68k->kdeny = 0;
    s->hnat->kdeny = 0;
    if (s->vm) {
        UnlockMemory(s->kcb, kKcbSize);
        UnlockMemory(s->owners, s->owners_size);
    }
    s->kcb = 0;
    s->owners = 0;
    DisposePtr(s->kcb_raw);
    DisposePtr(s->owners_raw);
    s->kcb_raw = s->owners_raw = 0;
}

static void hold_all(State *s, int on)
{
    hold(s, s, sizeof(State), on);
    hold(s, s->bitmap, kBitmapBytes, on);
    hold(s, (void *)s->my_lo, s->my_hi - s->my_lo, on);
    hold(s, s->h68k->ring, kHookRing * sizeof(HookEvent), on);
    hold(s, (void *)s->hnat->block, GetPtrSize((Ptr)s->hnat->block), on);
    if (s->sys)
        hold(s, s->sys, GetPtrSize((Ptr)s->sys), on);
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
    s->off_sys = offsetof(State, sys_dom);
    s->off_kernel = offsetof(State, kcb);
    s->mode = s->want_mode = kModeOff;
    copy_name(s->name, name);
    s->bitmap = bitmap;
    s->cur = -1;
    s->sys_dom = -1;

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
    set_deny(s, kModeOff);
    /* Where the 68k emulator fills a 68k-to-native call frame (9.2.2): its
       stores are not the application's although r24 points at its trap
       (common/hook.h). Only when the code is the one measured. */
    {
        volatile unsigned long *fc = lowmem(0x6806E344);
        if (fc[0] == 0x90650214 && fc[1] == 0x90C50218 && fc[2] == 0x90050210)
            s->h68k->frame = 0x6806E300;
    }
    {
        long vm = 0;
        s->vm = Gestalt(gestaltVMAttr, &vm) == noErr &&
                (vm & gestaltVMPresent);
    }
    kcb_start(s);
    {
        /* The Process Manager's zone, from a handle of its own. */
        OSErr e = noErr;
        Handle t = TempNewHandle(4, &e);
        if (t) {
            s->pm_zone = (unsigned long)HandleZone(t);
            DisposeHandle(t);
        }
    }
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
    /* What the timer, the hooks, the handler and the stopper use: this
       program's code and globals, and the system heap blocks. The stopper
       stays held: it can outlive the engine. */
    hold_all(s, 1);
    hold(s, s->stopper, sizeof(Stopper), 1);

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
    /* Temporary memory pools, with the kernel's owner check only: the
       hooks' current range is the partition alone. */
    if (s->kcb) {
        s->pool = pool_install();
        if (s->pool) {
            s->pool->enabled = s->mode != kModeOff;
            s->pool->quit_hook = 1;
        }
    }
    /* After the pool stubs: the patch goes on to them. */
    self_exit_install();
    *out = s;
    return 0;
}

void engine_poll(State *s)
{
    if (!s->running)
        return;
    sweep(s);
    refresh_domains(s);
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (d->live && !d->dying && i != s->sys_dom &&
            d->code_state == kCodeUnknown)
            find_code(s, i);
    }
    if (s->want_mode != s->mode)
        engine_set_mode(s, s->want_mode);
    if (s->want_sys != s->sys_on)
        engine_set_sys(s, s->want_sys);
    sys_follow(s);
}

void engine_stop(State *s)
{
    if (!s->running)
        return;
    /* No more chunks. Those made stay with their applications, as
       ordinary temporary memory. */
    pool_remove(s);
    /* Stop denying and protecting, stop the tick, take what the rings
       hold, then open everything before the hooks stop managing it. */
    set_deny(s, kModeOff);
    s->mode = kModeOff;
    s->active = 0;
    RmvTime((QElemPtr)&s->timer);
    drain_all(s);
    s->h68k->stop = 0;
    s->hnat->stop = 0;
    s->h68k->frame = 0;
    stopper_remove(s);
    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (!d->live)
            continue;
        d->dying = 1;
        unprotect_domain(s, i);
    }
    s->sys_on = 0;
    kcb_stop(s);
    self_exit_remove();
    g_sys_install(g_prev);
    hold_all(s, 0);

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
