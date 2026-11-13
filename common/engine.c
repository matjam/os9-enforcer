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

#include "engine.h"
#include "nk.h"
#include "hook.h"
#include "h68k.h"
#include "hnat.h"

enum { kTimerMs = 16, kBusErrorVector = 0x08 };

/* SysPages.flags */
enum { kPageExcluded = 1, kPageHot = 2, kPageCooled = 4 };

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
    if (i == s->sys_dom)
        sys_protect_all(s);
    else
        protect(s, i, s->domains[i].lo, s->domains[i].hi - s->domains[i].lo);
}

static void unprotect_domain(State *s, int i)
{
    Domain *d = &s->domains[i];
    if (i == s->sys_dom)
        sys_runs(s, 0, s->sys->n, kPageExcluded, 0);
    else if (nk_unprotect(s->area, d->lo, d->hi - d->lo))
        s->c.kernel_errors++;
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
        if (!s->domains[i].live)
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
                               hook_denied(s, src, addr));
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
        if (d->live)
            return d->location;
    }
    return lowmem_long(0x2A6); /* SysZone */
}

/* One store the kernel allowed and recorded, as the hooks' drain does with
   theirs. The writer is known from the store's code here, not from
   ApplZone: a store is the target's own when its code is the target's, or
   when system code stores into the application current at the time. */
static void kernel_event(State *s, const KEvent *e)
{
    unsigned flags = kev_flags(e->info), writer = kev_writer(e->info);
    unsigned current = (e->info >> 8) & 0xFF;
    int src = flags & kEvf68k ? kSrc68k : kSrcNative;
    int app = (flags & kEvfOffense) != 0;
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
            record_offense(s, src, pc, h.zone, t, addr, 0);
    }
    if (t != s->cur)
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
    if (app && addr >= s->deny_from) {
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
        unsigned long lo = (loc + kPage - 1) & kPageMask;
        unsigned long hi = (loc + info.processSize) & kPageMask;

        int found = -1, free_slot = -1;
        for (int i = 0; i < kMaxDomains; i++) {
            Domain *d = &s->domains[i];
            if (i == s->sys_dom)
                continue;
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
        if (s->sys_dom >= 0 && lo < s->domains[s->sys_dom].hi &&
            hi > s->domains[s->sys_dom].lo)
            continue; /* never: partitions are above the system heap */

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
        mark(s, lo, hi, owner_code(s, free_slot));
        protect_domain(s, free_slot);
        s->c.added++;
    }

    for (int i = 0; i < kMaxDomains; i++) {
        Domain *d = &s->domains[i];
        if (!d->live || d->seen || i == s->sys_dom)
            continue;
        d->dying = 1;
        if (s->cur == i) {
            set_current(s, -1);
            s->cur_zone = 0; /* the next tick looks again */
        }
        if (nk_unprotect(s->area, d->lo, d->hi - d->lo))
            s->c.kernel_errors++;
        mark(s, d->lo, d->hi, kOwnerNone);
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
    long ram = 0;
    if (Gestalt(gestaltLogicalRAMSize, &ram) || ram <= 0 ||
        (unsigned long)ram > kSpanHi)
        ram = kSpanHi;
    unsigned long entries = (unsigned long)ram >> 12;
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
    unsigned long phys = kcb_physical(k, kKcbSize);
    if (!phys)
        goto fail;
    for (unsigned long i = 0; i < tsize / kPage; i++) {
        unsigned long p = kcb_physical(t + i * kPage, kPage);
        if (!p)
            goto fail;
        k->pages[i] = p;
    }
    k->magic = kKcbMagic;
    k->version = kKcbVersion;
    k->mode = kKcbPass;
    k->entries = entries;
    k->frame = s->h68k->frame;
    err = kcb_register(phys);
    if (err)
        goto fail;
    s->kcb_raw = kraw;
    s->owners_raw = traw;
    s->owners = t;
    s->kseen = 0;
    s->kcb = k;
    s->h68k->kdeny = &k->denied_page;
    s->hnat->kdeny = &k->denied_page;
    s->kcb_err = 0;
    return;
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
    s->kcb = 0;
    s->owners = 0;
    DisposePtr(s->kcb_raw);
    DisposePtr(s->owners_raw);
    s->kcb_raw = s->owners_raw = 0;
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
    kcb_start(s);
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
    if (s->want_sys != s->sys_on)
        engine_set_sys(s, s->want_sys);
    sys_follow(s);
}

void engine_stop(State *s)
{
    if (!s->running)
        return;
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
