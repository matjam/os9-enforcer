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
#include <Events.h>
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
#include "hyield.h"
#include "hyield_code.h"

enum { kTimerMs = 16, kBusErrorVector = 0x08 };

/* The hook in the Process Manager's switch (pm_hook): an instruction its
   site S has, and the stub's layout in longs (a save area, the count of
   its calls, the instruction it replaced, then code). */
enum {
    kPmSite = 0x807F002C, /* lwz r3,0x2C(r31) */
    kStubSave = 0,
    kStubCalls = 14,
    kStubInsn = 15,
    kStubCode = 16,
    kStubLongs = 72
};

/* The launch size hook (size_hook): the instruction at its site, and the
   stub's layout in longs (the cap, the count of sizes raised, the last
   size before and after, then code). */
enum {
    kSizeInsn = 0x7FA3EB78, /* mr r3,r29 */
    kSzCap = 0,
    kSzRaised = 1,
    kSzFrom = 2,
    kSzTo = 3,
    kSzSparse = 4, /* the size of a sparse partition, or 0 for none */
    kSzCode = 8,
    kSzLongs = 48
};

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

static OSErr own_info(const ProcessSerialNumber *psn, ProcessInfoRec *info);
static void stack_set(State *s, int d, unsigned long lo, unsigned long hi);
static int chunk_is_sparse(const Chunk *c);

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

/* Set the owner bytes of the pages from lo >> 12 up to hi >> 12 to owner,
   where the byte is replace. With replace -1, every byte but the tables'
   (kOwnerTables), which the kernel never replaces except on request. */
static void set_owners(State *s, unsigned long lo, unsigned long hi,
                       unsigned char owner, long replace)
{
    unsigned long first = lo >> 12, end = hi >> 12;
    if (s->kcb && end > first &&
        kcb_owners(first, end - first, owner, replace))
        s->c.kernel_errors++;
}

/* The segments of [lo, hi), bit n for segment n: those up to 5, which
   application VSIDs can cover (6 and up hold the kernel, the emulator,
   I/O and the ROM). */
static unsigned long segments(unsigned long lo, unsigned long hi)
{
    unsigned long mask = 0;
    for (unsigned long g = lo >> 28; hi > lo && g <= (hi - 1) >> 28 && g < 6;
         g++)
        mask |= 1UL << g;
    return mask;
}

/* [lo, hi) is read-only to other applications by its VSIDs, with no page
   protection: application VSIDs are on and cover all of it. */
static int by_vsid(State *s, unsigned long lo, unsigned long hi)
{
    return s->spaces_on && hi > lo && (hi - 1) >> 28 < 6 &&
           (segments(lo, hi) & ~s->vsid_mask) == 0;
}

/* Application memory in [lo, hi) is marked: cover its segments with
   application VSIDs. A new segment turns them on again with the larger
   mask, which drops every application page's PTE. */
static void spaces_cover(State *s, unsigned long lo, unsigned long hi)
{
    unsigned long mask = s->vsid_mask | segments(lo, hi);
    if (!s->spaces || mask == s->vsid_mask)
        return;
    s->vsid_mask = mask;
    if (s->spaces_on && (kcb_set2(kKcbOpSpaces, 1, mask) ||
                         kcb_set(kKcbOpSwitch, current_zone())))
        s->c.kernel_errors++;
}

/* Manage the pages in [lo, hi) for owner (bitmap bits and owner bytes), or
   stop managing them (owner 0). */
static void mark(State *s, unsigned long lo, unsigned long hi,
                 unsigned char owner)
{
    set_bits(s, lo, hi, owner != kOwnerNone);
    set_owners(s, lo, hi, owner, -1);
    if (owner >= kOwnerApp)
        spaces_cover(s, lo, hi);
}

/* Give application domain i the owner bytes of its code's pages in
   [lo, hi) that are outside its partition and have no owner (engine.h,
   kCodeOwned). Only the owner table changes. */
static void own_code(State *s, int i, unsigned long lo, unsigned long hi)
{
    Domain *d = &s->domains[i];
    unsigned char code = owner_code(s, i) | kOwnerCode;
    lo &= kPageMask;
    hi = (hi + kPage - 1) & kPageMask;
    set_owners(s, lo, hi < d->lo ? hi : d->lo, code, kOwnerNone);
    set_owners(s, lo > d->hi ? lo : d->hi, hi, code, kOwnerNone);
    d->code_lo = lo;
    d->code_hi = hi;
    d->code_state = kCodeOwned;
}

/* The partly used first page of domain i's partition, where the heap
   starts and code often lies: its owner byte, so the kernel counts code
   there as the application's, but not protected (the Process Manager's
   block before the partition shares the page). on 0 gives the byte back. */
static void own_first_page(State *s, int i, int on)
{
    Domain *d = &s->domains[i];
    if (!s->kcb || i == s->sys_dom || (d->location & (kPage - 1)) == 0)
        return;
    unsigned long a = d->location & kPageMask;
    unsigned char code = owner_code(s, i) | kOwnerCode;
    if (a >= d->lo)
        return;
    if (on)
        set_owners(s, a, a + kPage, code, kOwnerNone);
    else
        set_owners(s, a, a + kPage, kOwnerNone, code);
}

/* Give back the owner bytes of domain i's code. The range stays in the
   slot (engine.h, Domain.dropped). */
static void disown_code(State *s, int i)
{
    Domain *d = &s->domains[i];
    if (d->code_state != kCodeOwned || !s->kcb)
        return;
    set_owners(s, d->code_lo, d->code_hi, kOwnerNone,
               owner_code(s, i) | kOwnerCode);
    d->code_state = kCodeUnknown;
}

/* Protect pages of application domain i, unless protection is off or the
   domain is going away. The system heap's pages go through sys_protect_*
   instead, which leave out its excluded and hot pages. Pages covered by
   application VSIDs are read-only by owner already: what is left to do
   is to drop the PTEs that stores the kernel allowed made writable. */
static void protect(State *s, int i, unsigned long lo, unsigned long len)
{
    Domain *d = &s->domains[i];
    if (s->mode == kModeOff || !d->live || d->dying || i == s->sys_dom)
        return;
    if (by_vsid(s, lo, lo + len)) {
        if (kcb_set2(kKcbOpFlush, lo >> 12, len >> 12))
            s->c.kernel_errors++;
        return;
    }
    if (nk_protect(s->area, lo, len))
        s->c.kernel_errors++;
}

static void sys_protect_all(State *s);
static void sys_runs(State *s, unsigned long from, unsigned long to,
                     unsigned char skip, int on);

/* Protect domain i's pages, at a switch away from it or when the engine
   lost track of which are open. Pages covered by application VSIDs need
   nothing. */
static void protect_domain(State *s, int i)
{
    Domain *d = &s->domains[i];
    if (i == s->sys_dom) {
        sys_protect_all(s);
        return;
    }
    if (!by_vsid(s, d->lo, d->hi))
        protect(s, i, d->lo, d->hi - d->lo);
    for (int j = 0; j < kMaxChunks; j++) {
        Chunk *c = &s->chunks[j];
        if (c->live && c->domain == i && !by_vsid(s, c->lo, c->hi))
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

/* Turn the kernel's application VSIDs on (with protection) or off. On,
   load the current application's at once; the hook in the Process
   Manager's switch loads them from then on. If the kernel refuses, the
   engine protects pages as before. */
static void spaces_set(State *s, int on)
{
    if (!s->spaces || !s->kcb || s->spaces_on == (unsigned long)on)
        return;
    if (on) {
        if (kcb_set2(kKcbOpSpaces, 1, s->vsid_mask)) {
            s->c.kernel_errors++;
            return;
        }
        s->spaces_on = 1;
        if (kcb_set(kKcbOpSwitch, current_zone()))
            s->c.kernel_errors++;
    } else {
        s->spaces_on = 0;
        if (kcb_set2(kKcbOpSpaces, 0, 0))
            s->c.kernel_errors++;
    }
}

/* Stores the engine no longer has events for opened pages it does not
   know: protect every page again. With application VSIDs, turning them on
   again drops every application page's PTE. */
static void protect_lost(State *s)
{
    if (s->spaces_on && s->mode != kModeOff) {
        if (kcb_set2(kKcbOpSpaces, 1, s->vsid_mask) ||
            kcb_set(kKcbOpSwitch, current_zone()))
            s->c.kernel_errors++;
    }
    protect_others(s);
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
    if (s->kcb && !s->kcb->applzone && /* else the kernel finds it itself */
        kcb_set(kKcbOpCurrent, i >= 0 ? owner_code(s, i) : kOwnerNone))
        s->c.kernel_errors++;
    s->cur = i;
}

/* Stop managing a chunk's pages, before its memory is freed. Safety rule
   (engine.h): not live first, so that nothing protects it again, then
   open its pages, then clear their bits and owners. */
static void sparse_release(State *s, unsigned long addr, unsigned long units);

static void drop_chunk(State *s, Chunk *c)
{
    int sparse = c->handle == (Handle)&c->block; /* chunk_is_sparse */
    c->live = 0;
    if (!sparse && nk_unprotect(s->area, c->lo, c->hi - c->lo))
        s->c.kernel_errors++;
    mark(s, c->lo, c->hi, kOwnerNone);
    if (sparse) /* its pages back to the pool */
        sparse_release(s, c->lo, (c->hi - c->lo) / kSparseUnit);
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
    own_first_page(s, i, 0);
    disown_code(s, i);
    stack_set(s, i, 0, 0);
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

static unsigned long timebase(void)
{
    unsigned long t;
    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}

/* In InterfaceLib, not in the Multiversal Interfaces. */
typedef struct {
    unsigned long hi, lo;
} MicroTime;
extern pascal void Microseconds(MicroTime *t);

/* Timebase ticks in a millisecond, over a 10 ms spin. Task level only. */
static unsigned long timebase_per_ms(void)
{
    MicroTime a, b;
    Microseconds(&a);
    unsigned long t0 = timebase();
    do
        Microseconds(&b);
    while (b.lo - a.lo < 10000);
    return (timebase() - t0) / ((b.lo - a.lo) / 1000);
}

/* The whole pages of domain i: its partition and its chunks. */
static unsigned long domain_pages(State *s, int i)
{
    unsigned long n = (s->domains[i].hi - s->domains[i].lo) >> 12;
    for (int j = 0; j < kMaxChunks; j++) {
        Chunk *c = &s->chunks[j];
        if (c->live && c->domain == i)
            n += (c->hi - c->lo) >> 12;
    }
    return n;
}

/* Tell the kernel where the current application's stack is (KernelCB.
   stacks): from ApplLimit to CurStackBase, read while the application is
   current, at each tick, since an application can move ApplLimit. The
   range stays while the application waits; drop_domain takes it out. */
static unsigned long g_stack[kMaxDomains][2];

static void stack_set(State *s, int d, unsigned long lo, unsigned long hi)
{
    if (!s->kcb || (g_stack[d][0] == lo && g_stack[d][1] == hi))
        return;
    KRegs k = {0};
    k.r3 = kKcbOpStack;
    k.r4 = (unsigned long)d;
    k.r5 = lo;
    k.r6 = hi;
    if (nk_call(kMPEnforcer, &k) == 0) {
        g_stack[d][0] = lo;
        g_stack[d][1] = hi;
    }
}

static void stack_follow(State *s)
{
    int d = s->cur;
    if (d < 0 || d == s->sys_dom)
        return;
    Domain *dom = &s->domains[d];
    if (!dom->live || dom->dying || current_zone() != dom->location)
        return;
    unsigned long lo = lowmem_long(0x130); /* ApplLimit */
    unsigned long hi = lowmem_long(0x908); /* CurStackBase */
    if (lo >= hi || lo < dom->location || hi > dom->location + dom->size)
        return;
    stack_set(s, d, lo, hi);
}

/* On a process switch, protect the previous application's domain. With
   application VSIDs, there is nothing to protect: load the new
   application's VSIDs, in case the hook in the Process Manager's switch
   did not (the kernel returns at once when it did). */
static void check_switch(State *s)
{
    if (s->pm_stub)
        s->c.pm_switches = s->pm_stub[kStubCalls];
    unsigned long zone = current_zone();
    if (zone == s->cur_zone)
        return;
    s->cur_zone = zone;
    if (s->spaces_on && kcb_set(kKcbOpSwitch, zone))
        s->c.kernel_errors++;
    int d = domain_of_zone(s, zone);
    if (d >= 0 && s->domains[d].dying)
        d = -1;
    if (d == s->cur)
        return;
    int old = s->cur;
    set_current(s, d);
    s->c.switches++;
    if (old >= 0) {
        unsigned long t = timebase();
        protect_domain(s, old);
        t = timebase() - t;
        s->c.switch_tb += t;
        if (t > s->c.switch_tb_max)
            s->c.switch_tb_max = t;
        if (old != s->sys_dom && !s->spaces_on)
            s->c.switch_pages += domain_pages(s, old);
    }
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
    if (s->kcb &&
        kcb_set(kKcbOpMode, mode == kModeEnforce ? kKcbEnforce
                            : mode == kModeLog   ? kKcbLog
                                                 : kKcbPass))
        s->c.kernel_errors++;
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
    /* The hook's stub stores into its save area at every switch, and the
       yield stub counts in its block. */
    sys_exclude_block(s, s->pm_stub, s->pm_stub ? kStubLongs * 4 : 0);
    sys_exclude_block(s, s->size_stub, s->size_stub ? kSzLongs * 4 : 0);
    sys_exclude_block(s, s->yield, s->yield ? sizeof hyield_code : 0);
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
/* Whether a hook's store came from the code of the application current at
   the store (the event's zone). The hooks mark it (kHookAppCode) from
   their current range, which moves on the timer's tick: up to 16 ms after
   a switch they cannot, and only the kernel's denial stops the writer.
   The event's zone is always right. The hooks know only the partition:
   native code outside it (engine.h, kCodeOwned) is the application's too,
   also when the application has quit since (Domain.dropped). */
static int event_app_code(State *s, int src, volatile HookEvent *e)
{
    if (e->pc & kHookAppCode)
        return 1;
    int w = domain_of_zone_any(s, e->zone);
    if (w < 0)
        return 0;
    const Domain *wd = &s->domains[w];
    unsigned long pc = e->pc & ~3UL;
    if (src == kSrc68k) {
        /* The emulator's code that fills a call frame is not the
           application's, although r24 then points at its trap. */
        unsigned long frame = s->h68k->frame;
        if (frame && pc - frame < kHookFrameLen)
            return 0;
        pc = hook_next_pc(e->r24);
    }
    if (pc >= wd->location && pc - wd->location < wd->size)
        return 1;
    return src != kSrc68k && (wd->code_state == kCodeOwned || !wd->live) &&
           pc >= wd->code_lo && pc < wd->code_hi;
}

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
        protect_lost(s);
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
        int app = event_app_code(s, src, e);
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
        /* Under application VSIDs the open PTE is the writer's VSID's,
           whichever application is current now. */
        if (t != s->cur || by_vsid(s, addr, addr + 1))
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
    if ((t != s->cur || by_vsid(s, addr, addr + 1)) && !skipped)
        protect(s, t, addr & kPageMask, kPage);
}

/* Take the stores the kernel recorded with seq in (kseen, kcb->seq]. A
   denied store is only counted: the hook that denies it records it. */
static void kernel_drain(State *s)
{
    const KernelCB *k = s->kcb;
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
        protect_lost(s);
        seen = last - kKcbRing;
    }
    for (unsigned long n = seen + 1; n - seen <= last - seen; n++) {
        const volatile KEvent *v = &k->ring[(n - 1) % kKcbRing];
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
            protect_lost(s);
            continue;
        }
        s->kevents++;
        if (kev_flags(e.info) & kEvfDenied) {
            s->kdenied++;
            /* The kernel made the page's PLE read-only for the denial
               (kernel/nanokernel.s, patch 5): open it again, and drop its
               PTE, which MPSetAreaAccess made writable in whichever VSID
               it was built for. */
            if (by_vsid(s, e.addr, e.addr + 1) &&
                (nk_unprotect(s->area, e.addr & kPageMask, kPage) ||
                 kcb_set2(kKcbOpFlush, e.addr >> 12, 1)))
                s->c.kernel_errors++;
        } else
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
    s->skips++;
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
    /* The current range moves on the timer's tick: up to 16 ms after a
       switch it is still the previous application's, and then says
       nothing about this store. */
    unsigned long zone = current_zone();
    int fresh = zone >= s->hnat->cur_lo && zone < s->hnat->cur_hi;
    int app = fresh && pc >= s->hnat->cur_lo && pc < s->hnat->cur_hi;
    if (fresh && !app && s->cur >= 0) {
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
    ev->zone = zone;
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
        stack_follow(s);
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
    /* Not preempted until it has given up the processor (yield_check). */
    if (s->kcb && kcb_set(kKcbOpFresh, owner_code(s, i)))
        s->c.kernel_errors++;
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
    own_first_page(s, i, 1);
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
        int ok = own_info(&psn, &info) == noErr;
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
    if (own_info(&psn, &info))
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
    kPoolQuitInfo = 0x01,      /* void (void) */
    kPoolClientInfo = 0xF1,    /* long (ProcessSerialNumber *), 68k */
    kPoolOverInfo = 0x3F1      /* long (long trap, long size) */
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

/* Sparse memory (engine.h) ---------------------------------------------- */

static long sparse_info(unsigned long which)
{
    return kcb_set(kKcbOpSparseInfo, which);
}

/* The pages with memory in [lo, hi) of the sparse range, for
   GetProcessInformation (pool_info), or -1 from a kernel without the
   count. The kernel walks the range, so the answer for each partition
   (slot 0 to kMaxParts - 1) and each chunk (after those) is kept for a
   second: the Finder asks about every process again and again while
   About This Computer is open. */
static unsigned long g_backed[kMaxParts + kMaxChunks];
static unsigned long g_backed_at[kMaxParts + kMaxChunks]; /* ticks; 0: none */

static long sparse_backed(int slot, unsigned long lo, unsigned long hi)
{
    unsigned long now = lowmem_long(0x16A) | 1; /* Ticks */
    if (!g_backed_at[slot] || now - g_backed_at[slot] >= 60) {
        long n = kcb_set2(kKcbOpSparseCount, lo, (hi - lo) / kPage);
        if (n < 0)
            return -1;
        g_backed[slot] = (unsigned long)n;
        g_backed_at[slot] = now;
    }
    return (long)g_backed[slot];
}

/* The engine's own GetProcessInformation calls want the partition as the
   Process Manager has it: pool_info leaves the record alone. */
static int g_own_info;

static OSErr own_info(const ProcessSerialNumber *psn, ProcessInfoRec *info)
{
    g_own_info++;
    OSErr err = GetProcessInformation((ProcessSerialNumber *)psn, info);
    g_own_info--;
    return err;
}

/* With virtual memory on, a page fault while the File Manager is busy
   cannot be served: the Virtual Memory Manager pages through the disk.
   New temporary memory, and the Memory Manager and Process Manager code
   that makes it, may not be in physical memory, so the engine makes none
   then: it can be called inside a File Manager call (an application's
   NewHandle, the disk driver's _MemoryDispatch) or with an asynchronous
   one queued. Aura (2026-11-15) and Scribble f stopped Mac OS in the
   kernel's debugger that way. FSBusy (0x360) is the File Manager queue's
   flags word. */
static int paging_unsafe(const State *s)
{
    return s->vm && *(volatile unsigned short *)lowmem(0x360) != 0;
}

/* Temporary memory that outlives this program: made in system mode, the
   Process Manager keeps it on its system list. */
static Handle sys_temp(State *s, unsigned long size)
{
    PoolStubs *p = s->pool;
    OSErr err = noErr;
    CallUniversalProc((UniversalProcPtr)&p->rd_sel, kPoolClientInfo, 0x40L);
    p->inside = 1;
    Handle h = TempNewHandle(size, &err);
    p->inside = 0;
    CallUniversalProc((UniversalProcPtr)&p->rd_sel, kPoolClientInfo, 0x41L);
    return h;
}

/* Give the kernel's pool one more block of pages. Task level. Returns the
   pages added, or 0. */
static unsigned long pool_grow(State *s)
{
    int slot = -1;
    for (int i = 0; i < kPoolBlocks && slot < 0; i++)
        if (!s->pool_blocks[i].h)
            slot = i;
    /* Leave the Process Manager some temporary memory of its own. The
       pages owed stay owed while paging is unsafe; the poll adds them. */
    if (slot < 0 || paging_unsafe(s) ||
        (unsigned long)TempFreeMem() < kOverReserve + kPoolBlock)
        return 0;
    /* With virtual memory on the block is locked, so that the Virtual
       Memory Manager leaves its page frames alone, and it is not one
       physical range: each page's physical address is kept in a table
       after the pages (PoolBlock.phys 0). */
    Handle h = sys_temp(s, kPoolBlock + (s->vm ? 2 * kPage : kPage));
    if (!h)
        return 0;
    MoveHHi(h);
    HLock(h);
    unsigned long base = ((unsigned long)*h + kPage - 1) & kPageMask;
    unsigned long phys = 0;
    if (s->vm) {
        unsigned long *t = (unsigned long *)(base + kPoolBlock);
        if (LockMemory((void *)base, kPoolBlock + kPage)) {
            DisposeHandle(h);
            return 0;
        }
        for (unsigned long i = 0; i < kPoolBlock / kPage; i++) {
            t[i] = kcb_physical((void *)(base + i * kPage), kPage);
            if (!t[i]) {
                UnlockMemory((void *)base, kPoolBlock + kPage);
                DisposeHandle(h);
                return 0;
            }
        }
        for (unsigned long i = 0; i < kPoolBlock / kPage; i++)
            kcb_set(kKcbOpPoolPut, t[i]);
    } else {
        phys = kcb_physical((void *)base, kPoolBlock);
        if (!phys) { /* not one physical range */
            DisposeHandle(h);
            return 0;
        }
        for (unsigned long o = 0; o < kPoolBlock; o += kPage)
            kcb_set(kKcbOpPoolPut, phys + o);
    }
    s->pool_blocks[slot].h = h;
    s->pool_blocks[slot].phys = phys;
    s->pool_grown++;
    return kPoolBlock / kPage;
}

/* kKcbOpPoolTake on one physical range: how many of its pages are in the
   pool (count), or take them out. */
static long pool_take(unsigned long lo, unsigned long hi, int count)
{
    KRegs k = {0};
    k.r3 = kKcbOpPoolTake;
    k.r4 = lo;
    k.r5 = hi;
    k.r6 = count;
    return nk_call(kMPEnforcer, &k);
}

/* pool_shrink for a block of virtual memory: its pages one by one, from
   the table pool_grow left. Returns 1 when the block is freed. */
static int pool_shrink_paged(PoolBlock *b)
{
    unsigned long base = ((unsigned long)*b->h + kPage - 1) & kPageMask;
    const unsigned long *t = (const unsigned long *)(base + kPoolBlock);
    for (unsigned long i = 0; i < kPoolBlock / kPage; i++)
        if (pool_take(t[i], t[i] + kPage, 1) != 1)
            return 0;
    for (unsigned long i = 0; i < kPoolBlock / kPage; i++)
        pool_take(t[i], t[i] + kPage, 0);
    UnlockMemory((void *)base, kPoolBlock + kPage);
    DisposeHandle(b->h);
    b->h = 0;
    return 1;
}

/* Take back and free one block whose pages are all in the pool. Returns
   the pages taken, or 0 when no block is whole. */
static unsigned long pool_shrink(State *s)
{
    for (int i = kPoolBlocks - 1; i >= 0; i--) {
        PoolBlock *b = &s->pool_blocks[i];
        if (!b->h)
            continue;
        if (!b->phys) {
            if (!pool_shrink_paged(b))
                continue;
            s->pool_shrunk++;
            return kPoolBlock / kPage;
        }
        if (pool_take(b->phys, b->phys + kPoolBlock, 1) != kPoolBlock / kPage)
            continue;
        pool_take(b->phys, b->phys + kPoolBlock, 0);
        DisposeHandle(b->h);
        b->h = 0;
        s->pool_shrunk++;
        return kPoolBlock / kPage;
    }
    return 0;
}

/* Make sure the pool holds its headroom and pages more. Task level, at a
   point where the Memory Manager can be called. Returns 0 when the
   machine has no memory for it. */
/* Pages promised to allocations and not yet given (State.pool_debt): the
   pages the kernel has given since the last look come off it. A touch
   the engine promised nothing for also counts, so the debt can run low;
   the headroom covers that. */
/* A promise ends when its pages are given, and also with time: a block
   made from heap space that has its pages already takes none, so its
   promise would stay for good, and a program that makes and frees large
   blocks (a browser) would raise the debt, and with it the pool, until
   the machine's free memory was all in the pool (About This Computer
   shows it as Mac OS). The debt is in two parts: g_debt_new is what was
   promised since g_debt_at, the rest is older. After kDebtTicks the
   older part is dropped and the new part becomes the older one, so a
   promise lasts between one and two times kDebtTicks. A program that is
   still filling a block after that goes on at the pace the headroom
   allows between two polls. */
enum { kDebtTicks = 600 }; /* 10 seconds */
static unsigned long g_debt_new, g_debt_at;

static void sparse_settle(State *s)
{
    long given = sparse_info(1);
    if (given < 0)
        return;
    unsigned long d = (unsigned long)given - s->pool_given;
    s->pool_given = (unsigned long)given;
    s->pool_debt = s->pool_debt > d ? s->pool_debt - d : 0;
    if (!s->sparse_used)
        s->pool_debt = 0;
    unsigned long now = lowmem_long(0x16A); /* Ticks */
    if (now - g_debt_at >= kDebtTicks) {
        if (s->pool_debt > g_debt_new)
            s->pool_debt = g_debt_new;
        g_debt_new = 0;
        g_debt_at = now;
    }
    if (g_debt_new > s->pool_debt) /* pages given come off the older part */
        g_debt_new = s->pool_debt;
}

/* The pages of the sparse partitions and chunks that have no memory yet:
   no more than these can be owed. From the counts sparse_backed keeps, so
   up to a second old: a count that is low makes this high, which only
   keeps more in the pool. */
static unsigned long sparse_unbacked(State *s)
{
    unsigned long total = 0;
    for (int i = 0; i < kMaxParts; i++) {
        SparsePart *p = &s->parts[i];
        if (!p->units)
            continue;
        unsigned long span = p->units * (kSparseUnit / kPage);
        long n = sparse_backed(i, p->lo, p->lo + p->units * kSparseUnit);
        if (n < 0)
            return 0xFFFFFFFFUL;
        total += span > (unsigned long)n ? span - (unsigned long)n : 0;
    }
    for (int i = 0; i < kMaxChunks; i++) {
        Chunk *c = &s->chunks[i];
        if (!c->live || !chunk_is_sparse(c))
            continue;
        unsigned long span = (c->hi - c->lo) / kPage;
        long n = sparse_backed(kMaxParts + i, c->lo, c->hi);
        if (n < 0)
            return 0xFFFFFFFFUL;
        total += span > (unsigned long)n ? span - (unsigned long)n : 0;
    }
    return total;
}

static int sparse_ensure(State *s, unsigned long pages, int promise)
{
    sparse_settle(s);
    long level = sparse_info(0);
    if (level < 0)
        return 0;
    /* With promise, the pages are for a block an application will use
       later: they stay in the pool until they are touched or the pieces
       are given back, so the engine's poll does not take them away while
       the application is still on its way through the block (Scribble E:
       a preempted write loop, the poll in between, and a touch with the
       pool empty). Without, they are touched right away. */
    if (promise) {
        unsigned long most = sparse_unbacked(s);
        s->pool_debt += pages;
        g_debt_new += pages;
        if (s->pool_debt > most)
            s->pool_debt = most;
        if (g_debt_new > s->pool_debt)
            g_debt_new = s->pool_debt;
    }
    pages = promise ? s->pool_debt : s->pool_debt + pages;
    while ((unsigned long)level < kPoolHeadroom + pages) {
        unsigned long n = pool_grow(s);
        if (!n) {
            if (!paging_unsafe(s))
                s->sparse_short++;
            return 0;
        }
        level += n;
    }
    return 1;
}

/* Keep the pool near its headroom (task level, from engine_poll): give
   memory back when it holds much more, all of it when nothing is in the
   range. */
static void sparse_balance(State *s)
{
    if (!s->sparse_area)
        return;
    sparse_settle(s);
    long level = sparse_info(0);
    unsigned long keep =
        s->sparse_used ? 2 * kPoolHeadroom + s->pool_debt : 0;
    while (level > 0 && (unsigned long)level > keep + kPoolBlock / kPage) {
        unsigned long n = pool_shrink(s);
        if (!n)
            break;
        level -= n;
    }
    if (s->sparse_used)
        sparse_ensure(s, 0, 0);
    else
        s->pool_debt = 0;
}

/* units pieces of the range, in a row: their address, or 0. */
static unsigned long sparse_alloc(State *s, unsigned long units)
{
    unsigned long total = (s->sparse_hi - s->sparse_lo) / kSparseUnit;
    if (!s->sparse_area || !units || units > total)
        return 0;
    unsigned long mask = units >= 32 ? 0xFFFFFFFFUL : (1UL << units) - 1;
    for (unsigned long i = 0; i + units <= total; i++)
        if (!(s->sparse_used & (mask << i))) {
            s->sparse_used |= mask << i;
            return s->sparse_lo + i * kSparseUnit;
        }
    return 0;
}

/* Give the pieces back: their pages return to the pool. Kernel calls
   only, so it can run where drop_chunk does. */
static void sparse_release(State *s, unsigned long addr, unsigned long units)
{
    if (!s->sparse_area || addr < s->sparse_lo)
        return;
    unsigned long i = (addr - s->sparse_lo) / kSparseUnit;
    unsigned long mask = units >= 32 ? 0xFFFFFFFFUL : (1UL << units) - 1;
    kcb_set2(kKcbOpSparseFree, addr, units * (kSparseUnit / kPage));
    s->sparse_used &= ~(mask << i);
}

static int chunk_is_sparse(const Chunk *c)
{
    return c->handle == (Handle)&c->block;
}

/* _MemoryDispatch for an address in the sparse range (engine.h), from its
   stub: sel 0 HoldMemory, 1 UnholdMemory, 2 LockMemory, 3
   LockMemoryContiguous, 4 UnlockMemory, 10 LockMemoryForOutput (a0 the
   address, a1 the length), 5 GetPhysical (a0 the table: a logical address
   and length, then physical address and length pairs; a1 how many pairs
   it holds). With virtual memory on, the disk driver locks a write's
   buffer with LockMemoryForOutput: without it here, FSWrite from sparse
   memory returned ioErr (Scribble w), and the installer, in a sparse
   partition, stopped while writing the Mac OS ROM file.
   Returns the error in the low word and, for GetPhysical, the pairs
   filled in the high word. Holding and locking give the pages memory;
   nothing takes it away before the chunk or partition is given back. */
static long pool_md(long sel, unsigned long a0, unsigned long a1)
{
    State *s = g_state;
    if (!s || !s->sparse_area)
        return (long)(unsigned short)paramErr;
    s->md_calls++;
    if (sel == 1 || sel == 4)
        return 0;
    unsigned long addr = a0, len = a1;
    unsigned long *t = (unsigned long *)a0;
    if (sel == 5) {
        addr = t[0];
        len = t[1];
    }
    if (!len || addr < s->sparse_lo || addr + len > s->sparse_hi ||
        addr + len < addr)
        return (long)(unsigned short)paramErr;
    unsigned long first = addr & kPageMask;
    unsigned long last = (addr + len - 1) & kPageMask;
    if (CurrentExecutionLevel() == kTaskLevel)
        sparse_ensure(s, (last - first) / kPage + 1, 0);
    unsigned long pairs = 0, run = 0, run_len = 0, done = 0;
    for (unsigned long p = first;; p += kPage) {
        (void)*(volatile unsigned char *)p; /* a page, if it had none */
        KRegs k = {0};
        k.r3 = s->sparse_area;
        k.r4 = p;
        if (nk_call(kcGetPhysicalAddress, &k))
            return (long)(unsigned short)paramErr;
        unsigned long phys = k.r5 & kPageMask;
        unsigned long from = p == first ? addr : p;
        unsigned long to = p == last ? addr + len : p + kPage;
        if (sel == 5) {
            if (run_len && run + run_len == phys + (from - p)) {
                run_len += to - from;
            } else {
                if (run_len) {
                    if (pairs == a1)
                        break; /* the table is full */
                    t[2 + 2 * pairs] = run;
                    t[3 + 2 * pairs] = run_len;
                    pairs++;
                    done += run_len;
                }
                run = phys + (from - p);
                run_len = to - from;
            }
        } else if (sel == 3) {
            if (run_len && run + run_len != phys)
                return (long)(unsigned short)-620; /* notEnoughMemoryErr */
            if (!run_len)
                run = phys;
            run_len += kPage;
        }
        if (p == last) {
            if (sel == 5 && run_len && pairs < a1) {
                t[2 + 2 * pairs] = run;
                t[3 + 2 * pairs] = run_len;
                pairs++;
                done += run_len;
            }
            break;
        }
    }
    if (sel == 5) { /* what is left to translate, as the ROM leaves it */
        t[0] = addr + done;
        t[1] = len - done;
        return (long)(pairs << 16);
    }
    return 0;
}

/* Make the range (engine_start, with the block registered and the stubs
   in): feed the kernel's free list for the area's page tables, from
   system heap pages it keeps, and create the area. Without kernel patch
   7, or with no room above RAM, there is none. */
static void sparse_start(State *s)
{
    if (!s->kcb || !s->pool || !s->sparse_want_hi || sparse_info(0) < 0)
        return;
    unsigned long lo = s->sparse_want_lo, hi = s->sparse_want_hi;
    unsigned long pages = (hi - lo) / kPage;
    unsigned long need = pages / 1024 + pages / 2048 + 8;
    /* While there is no range, sparse_short says why: 1 no system heap, 2
       not locked, or the kernel's error from MPCreateArea. */
    Ptr feed = NewPtrSysClear((need + 1) * kPage);
    s->sparse_short = 1;
    if (!feed)
        return;
    unsigned long a = ((unsigned long)feed + kPage - 1) & kPageMask;
    /* The kernel keeps these pages by physical address: with virtual
       memory on they stay locked for good. */
    if (s->vm && LockMemory((void *)a, need * kPage)) {
        DisposePtr(feed);
        s->sparse_short = 2;
        return;
    }
    for (unsigned long i = 0; i < need; i++, a += kPage) {
        KRegs k = {0};
        k.r3 = kcb_physical((void *)a, kPage);
        if (k.r3)
            nk_call(kcPutFreePage, &k);
    }
    KRegs k = {0};
    k.r4 = 6;      /* read and write */
    k.r5 = hi - lo;
    k.r7 = 0x600C; /* at the base, 4 KB pages */
    k.r8 = lo;
    long err = nk_call(kcCreateArea, &k);
    if (err) {
        s->sparse_short = (unsigned long)err;
        return;
    }
    s->sparse_short = 0;
    kcb_set2(kKcbOpSparse, lo, hi);
    s->sparse_lo = lo;
    s->sparse_hi = hi;
    s->sparse_area = k.r9;
    s->pool->sparse_hi = hi; /* the _MemoryDispatch stub's range */
    s->pool->sparse_lo = lo;
}

/* A chunk for domain d in the sparse range, with room for size bytes, or
   0. Its zone covers whole pieces; only what is touched costs memory. */
static Chunk *sparse_chunk(State *s, int d, unsigned long size)
{
    unsigned long units = (size + kChunkSlack + kSparseUnit - 1) / kSparseUnit;
    int slot = -1;
    for (int i = 0; i < kMaxChunks && slot < 0; i++)
        if (!s->chunks[i].live)
            slot = i;
    if (!s->sparse_area || slot < 0 || units > kSparseMost ||
        !sparse_ensure(s, 16, 0))
        return 0;
    unsigned long lo = sparse_alloc(s, units);
    if (!lo)
        return 0;
    unsigned long hi = lo + units * kSparseUnit;
    THz z = (THz)(lo + 16);
    THz old = GetZone();
    InitZone(NULL, kChunkMasters, (Ptr)hi, z); /* makes z current */
    SetZone(old);
    unsigned long *sig = (unsigned long *)lo;
    sig[0] = kChunkSig;
    sig[1] = (unsigned long)z;

    Chunk *c = &s->chunks[slot];
    g_backed_at[kMaxParts + slot] = 0; /* not the last chunk's count */
    c->domain = d;
    c->block = lo;
    c->handle = (Handle)&c->block; /* chunk_is_sparse; the sweep's test
                                      always holds */
    c->lo = lo;
    c->hi = hi;
    c->zone = (unsigned long)z;
    c->handles = 0;
    c->keep = 0;
    mark(s, lo, hi, owner_code(s, d));
    c->live = 1;
    s->pc.chunks_made++;
    s->sparse_chunks++;
    return c;
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

/* A pointer of size bytes in chunk c, or 0. */
static Ptr chunk_alloc_ptr(Chunk *c, unsigned long size)
{
    THz z = (THz)c->zone;
    if ((unsigned long)z->zcbFree < size)
        return 0;
    THz old = GetZone();
    SetZone(z);
    Ptr p = NewPtr(size);
    SetZone(old);
    return p;
}

/* A new chunk for domain d with room for a handle of size bytes, or 0.
   Called by d's application, in TempNewHandle, so the Process Manager
   makes the chunk that application's temporary memory. With reserve, only
   if that much temporary memory stays free. */
static Chunk *new_chunk(State *s, int d, unsigned long size,
                        unsigned long reserve)
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
    /* While paging is unsafe the request fails, as in a full heap. */
    if (slot < 0 || csize < size || csize > 0x40000000UL ||
        paging_unsafe(s))
        return 0;
    if (csize < kChunkMin)
        csize = kChunkMin;
    if (reserve) {
        /* An overflow chunk is at least kOverChunk when there is room, so
           later blocks share it (there are kMaxChunks chunks in all). */
        unsigned long avail = (unsigned long)TempFreeMem();
        if (avail < reserve || avail - reserve < csize + kPage)
            return 0;
        if (csize < kOverChunk && avail - reserve >= kOverChunk + kPage)
            csize = kOverChunk;
    }
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
    mark(s, lo, hi, owner_code(s, d)); /* with application VSIDs, also
                                          read-only to the others */
    c->live = 1;
    if (d != s->cur && !by_vsid(s, lo, hi))
        protect(s, d, lo, hi - lo);
    s->pc.chunks_made++;
    return c;
}

/* Whether the caller is in system mode: the Process Manager then counts
   its files and temporary memory as the system's, not the current
   application's. System software makes memory that outlives the current
   application this way; the Process Manager puts such a TempNewHandle on
   its system list, which an application's quit leaves alone. */
static int system_mode(State *s)
{
    ProcessSerialNumber psn = {0, 0};
    long err = CallUniversalProc((UniversalProcPtr)&s->pool->rd_client,
                                 kPoolClientInfo, &psn);
    return err == noErr && psn.highLongOfPSN == 0 &&
           psn.lowLongOfPSN == kSystemProcess;
}

/* TempNewHandle, from the _OSDispatch stub. frame is the 68k stack at the
   stub: the return address, the selector (a word), the OSErr pointer, the
   size, then the function result. Returns 0 when it has stored the
   result (the stub then returns to the caller), or 1 to pass the call on
   to the Process Manager: always in system mode, where the memory is not
   the current application's. */
static long pool_new(unsigned long frame)
{
    State *s = g_state;
    if (!s || !s->running || !s->kcb || !s->pool)
        return 1;
    if (system_mode(s)) {
        s->pc.system++;
        return 1;
    }
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
        c = new_chunk(s, d, size, 0);
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

/* Overflow (engine.h): NewHandle (ptr 0) or NewPtr (ptr 1) has failed;
   trap is the trap word, size the request. Returns the block from one of
   the current application's chunks, or 0. Only for a request in the
   application's own zone: one made with another zone current (a chunk's,
   by the engine itself) fails as before. */
static long pool_over(long trap, long size, int ptr)
{
    State *s = g_state;
    if (!s || !s->running || !s->kcb || !s->pool || !s->pool->overflow ||
        s->pool->inside)
        return 0;
    unsigned long zone = current_zone();
    if (lowmem_long(0x118) != zone || size <= 0) /* TheZone */
        return 0;
    sweep(s);
    int d = domain_of_zone(s, zone);
    if (d < 0)
        d = add_current(s);
    if (d < 0 || d == s->sys_dom || s->domains[d].dying) {
        s->pc.over_none++;
        return 0;
    }
    /* A handle's master pointer, or a pointer's header, comes out of the
       chunk too. */
    unsigned long need = (unsigned long)size + 64;
    unsigned long b = 0;
    Chunk *c = 0;
    for (int i = 0; i <= kMaxChunks && !b; i++) {
        if (i == kMaxChunks) {
            /* A new chunk: in the sparse range when there is one, where
               it has room for later blocks and costs what is touched. */
            c = sparse_chunk(s, d, need);
            if (!c)
                c = new_chunk(s, d, need, kOverReserve);
            if (!c)
                break;
        } else {
            c = &s->chunks[i];
            if (!c->live || c->domain != d)
                continue;
        }
        /* The block's pages come out of the pool as they are touched:
           have them there first. */
        if (chunk_is_sparse(c) &&
            ((unsigned long)((THz)c->zone)->zcbFree < (unsigned long)size ||
             !sparse_ensure(s, (unsigned long)size / kPage + 2, 1)))
            continue;
        b = ptr ? (unsigned long)chunk_alloc_ptr(c, size)
                : (unsigned long)chunk_alloc(c, size);
    }
    if (!b) {
        s->pc.over_none++;
        return 0;
    }
    c->handles++;
    if (trap & 0x0200) /* NewHandleClear, NewPtrClear */
        memset(ptr ? (void *)b : (void *)*(Handle)b, 0, size);
    if (ptr)
        s->pc.over_ptrs++;
    else
        s->pc.over_handles++;
    *(volatile short *)lowmem(0x220) = noErr; /* MemErr */
    return (long)b;
}

/* GetProcessInformation has filled the record at info (engine.h): add the
   process's chunks to its size and their free bytes to its free memory.

   When another process asks (the Finder, for About This Computer), a
   sparse partition and a sparse chunk count as the memory behind them:
   the size is the pages the kernel has given, and the free memory is
   what of those the heap does not use. A process that asks about itself
   gets the partition as it is, so that one which sizes its work from the
   record does not see a few hundred KB. */
static void pool_info(unsigned long info, long psn)
{
    State *s = g_state;
    ProcessInfoRec *r = (ProcessInfoRec *)info;
    (void)psn; /* can be a constant; the record holds the number */
    if (!s || !s->running || !s->pool || !r || r->processInfoLength < 40 ||
        g_own_info)
        return;
    unsigned long loc = (unsigned long)r->processLocation;
    int other = current_zone() != loc;
    for (int i = 0; other && i < kMaxParts; i++) {
        SparsePart *p = &s->parts[i];
        if (!p->units || loc != p->lo + kPage)
            continue;
        long n = sparse_backed(i, p->lo, p->lo + p->units * kSparseUnit);
        if (n < 0)
            break;
        /* In use: the heap up to its end, less its free bytes. (The
           record's free memory counts only the zone's, and the zone ends
           far below the partition's top, so size less free memory would
           call the untouched rest used.) The stack is not counted. */
        unsigned long backed = (unsigned long)n * kPage;
        THz z = (THz)loc;
        unsigned long end = (unsigned long)z->bkLim;
        unsigned long heap = end > loc ? end - loc : 0;
        unsigned long zfree = (unsigned long)z->zcbFree;
        unsigned long used = heap > zfree ? heap - zfree : 0;
        r->processSize = backed;
        r->processFreeMem = backed > used ? backed - used : 0;
        break;
    }
    for (int d = 0; d < kMaxDomains; d++) {
        Domain *dom = &s->domains[d];
        if (!dom->live || dom->dying || d == s->sys_dom ||
            !same_psn(&dom->psn, &r->processNumber))
            continue;
        unsigned long size = 0, free = 0;
        for (int i = 0; i < kMaxChunks; i++) {
            Chunk *c = &s->chunks[i];
            if (!c->live || c->domain != d)
                continue;
            unsigned long csize = c->hi - c->lo;
            unsigned long cfree = (unsigned long)((THz)c->zone)->zcbFree;
            long n = other && chunk_is_sparse(c)
                         ? sparse_backed(kMaxParts + i, c->lo, c->hi) : -1;
            if (n >= 0) {
                unsigned long backed = (unsigned long)n * kPage;
                unsigned long used = csize > cfree ? csize - cfree : 0;
                csize = backed;
                cfree = backed > used ? backed - used : 0;
            }
            size += csize;
            free += cfree;
        }
        r->processSize += size;
        r->processFreeMem += free;
        return;
    }
}

static long pool_overh(long trap, long size)
{
    return pool_over(trap, size, 0);
}

static long pool_overp(long trap, long size)
{
    return pool_over(trap, size, 1);
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
        int sparse = chunk_is_sparse(c);
        drop_chunk(s, c);
        s->pc.chunks_freed++;
        if (!sparse)
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
    if (own_info(&d->psn, &info))
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

   _OSDispatch (on the stack: the return address, the selector, the
   arguments, the result):
     00 tst.w   enabled          ; off: pass
     06 beq.s   70
     08 tst.w   inside           ; the engine's own call: pass
     0E bne.s   70
     10 cmpi.w  #$001D,4(sp)     ; TempNewHandle
     16 beq.s   58
     18 cmpi.w  #$003A,4(sp)     ; not GetProcessInformation: pass
     1E bne.s   70
     20 subq.l  #2,sp            ; call the routine with the same arguments
     22 move.l  12(sp),-(sp)     ; the serial number
     26 move.l  12(sp),-(sp)     ; the record
     2A move.w  #$003A,-(sp)
     2E movea.l osd_old,a0
     34 jsr     (a0)
     36 move.w  (sp)+,d0
     38 move.w  d0,14(sp)        ; the result
     3C bne.s   50
     3E move.l  10(sp),-(sp)     ; pool_info(record, serial number)
     42 move.l  10(sp),-(sp)
     46 movea.l #rd_info,a0
     4C jsr     (a0)
     4E addq.l  #8,sp
     50 movea.l (sp)+,a0         ; drop the selector and arguments
     52 lea     10(sp),sp
     56 jmp     (a0)
     58 pea     (sp)             ; pool_new(frame)
     5A movea.l #rd_new,a0
     60 jsr     (a0)
     62 addq.l  #4,sp
     64 tst.l   d0
     66 bne.s   70
     68 movea.l (sp)+,a0         ; taken: drop the selector and arguments
     6A lea     10(sp),sp
     6E jmp     (a0)
     70 move.l  osd_old,-(sp)
     76 rts

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

   NewHandle and NewPtr (D0 the size, D1 the trap word; the result in A0
   and D0), with old the trap's routine and rd pool_overh or pool_overp:
     00 tst.w   enabled
     06 beq.s   38
     08 btst    #10,d1           ; in the system heap: pass
     0C bne.s   38
     0E move.l  d0,-(sp)         ; the size and the trap word
     10 move.l  d1,-(sp)
     12 movea.l old,a1
     18 jsr     (a1)
     1A tst.w   d0               ; it worked
     1C beq.s   32
     1E movea.l #rd,a1           ; pool_over(trap, size)
     24 jsr     (a1)
     26 movea.l d0,a0
     28 tst.l   d0
     2A bne.s   30
     2C moveq   #-108,d0         ; memFullErr
     2E bra.s   32
     30 moveq   #0,d0
     32 addq.l  #8,sp
     34 tst.w   d0
     36 rts
     38 move.l  old,-(sp)
     3E rts

   DisposePtr (A0 the pointer), as DisposeHandle; a nonrelocatable block's
   header holds its zone at -4:
     00 tst.w   enabled
     06 beq.s   48
     08 move.l  a0,d2
     0A beq.s   48
     0C movea.l d2,a1
     0E movea.l -4(a1),a1
     12 move.l  a1,d2
     14 beq.s   48
     16 cmpi.l  #'EnfT',-16(a1)
     1E bne.s   48
     20 cmpa.l  -12(a1),a1
     24 bne.s   48
     26 move.l  a1,-(sp)
     28 movea.l dp_old,a1
     2E jsr     (a1)
     30 move.l  d0,-(sp)
     32 move.l  4(sp),-(sp)
     36 movea.l #rd_disposed,a1
     3C jsr     (a1)
     3E addq.l  #4,sp
     40 move.l  (sp)+,d0
     42 addq.l  #4,sp
     44 tst.w   d0
     46 rts
     48 move.l  dp_old,-(sp)
     4E rts
*/
enum { kGNEFilter = 0x29A };

/* The stub above, with one more step when the routine worked: for a
   request of 16 KB or more made with TheZone in the sparse range (a
   sparse partition or chunk), pool_top(size) puts that much in the
   kernel's pool before the block is touched:
     1A tst.w   d0
     1C bne.s   52               ; it failed: overflow, as above
     1E cmpi.l  #$3FFF,4(sp)     ; the size
     26 bls.s   66
     28 move.l  $0118.w,d1       ; TheZone
     2C cmp.l   sparse_lo,d1
     32 bcs.s   66
     34 cmp.l   sparse_hi,d1
     3A bcc.s   66
     3C move.l  a0,-(sp)
     3E move.l  8(sp),-(sp)
     42 movea.l #rd_top,a0
     48 jsr     (a0)
     4A addq.l  #4,sp
     4C movea.l (sp)+,a0
     4E moveq   #0,d0
     50 bra.s   66
     52 ...                      ; pool_over, as above
     66 addq.l  #8,sp
     68 tst.w   d0
     6A rts
     6C move.l  old,-(sp)
     72 rts */
static void alloc_stub(unsigned short *dst, unsigned long en,
                       unsigned long old, unsigned long rd,
                       unsigned long lo, unsigned long hi, unsigned long rt)
{
    const unsigned short code[kPoolAlloc] = {
        0x4A79, HI16(en), LO16(en), 0x6764,
        0x0801, 0x000A, 0x665E,
        0x2F00, 0x2F01,
        0x2279, HI16(old), LO16(old), 0x4E91,
        0x4A40, 0x6634,
        0x0CAF, 0x0000, 0x3FFF, 0x0004, 0x633E,
        0x2238, 0x0118,
        0xB2B9, HI16(lo), LO16(lo), 0x6532,
        0xB2B9, HI16(hi), LO16(hi), 0x642A,
        0x2F08, 0x2F2F, 0x0008,
        0x207C, HI16(rt), LO16(rt), 0x4E90,
        0x588F, 0x205F, 0x7000, 0x6014,
        0x227C, HI16(rd), LO16(rd), 0x4E91,
        0x2040, 0x4A80, 0x6604,
        0x7094, 0x6002,
        0x7000,
        0x508F, 0x4A40, 0x4E75,
        0x2F39, HI16(old), LO16(old), 0x4E75};
    memcpy(dst, code, sizeof code);
}

/* SetHandleSize, ReallocateHandle and SetPtrSize (A0 the block, D0 the
   new size), with old the trap's routine:
     00 tst.w   enabled
     06 beq.s   3C
     08 cmpi.l  #$3FFF,d0
     0E bls.s   3C
     10 move.l  d1,-(sp)
     12 move.l  $02AA.w,d1       ; ApplZone
     16 cmp.l   sparse_lo,d1
     1C bcs.s   3A
     1E cmp.l   sparse_hi,d1
     24 bcc.s   3A
     26 movem.l d0/a0,-(sp)
     2A move.l  d0,-(sp)         ; pool_top(size)
     2C movea.l #rd_top,a0
     32 jsr     (a0)
     34 addq.l  #4,sp
     36 movem.l (sp)+,d0/a0
     3A move.l  (sp)+,d1
     3C move.l  old,-(sp)
     42 rts */
static void grow_stub(unsigned short *dst, unsigned long en,
                      unsigned long old, unsigned long lo, unsigned long hi,
                      unsigned long rt)
{
    const unsigned short code[kPoolGrow] = {
        0x4A79, HI16(en), LO16(en), 0x6734,
        0x0C80, 0x0000, 0x3FFF, 0x632C,
        0x2F01, 0x2238, 0x02AA,
        0xB2B9, HI16(lo), LO16(lo), 0x651C,
        0xB2B9, HI16(hi), LO16(hi), 0x6414,
        0x48E7, 0x8080, 0x2F00,
        0x207C, HI16(rt), LO16(rt), 0x4E90,
        0x588F, 0x4CDF, 0x0101,
        0x221F,
        0x2F39, HI16(old), LO16(old), 0x4E75};
    memcpy(dst, code, sizeof code);
}

/* From the stubs on _NewHandle and _NewPtr: a block of size bytes was
   made in a sparse zone. */
static void pool_top(long size)
{
    State *s = g_state;
    if (s && s->running && s->sparse_area && size > 0 &&
        CurrentExecutionLevel() == kTaskLevel)
        sparse_ensure(s, (unsigned long)size / kPage + 2, 1);
}

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
    UniversalProcPtr uc =
        p ? NewRoutineDescriptor((ProcPtr)p->client, kPoolClientInfo,
                                 kM68kISA)
          : 0;
    UniversalProcPtr uoh = NewRoutineDescriptor((ProcPtr)pool_overh,
                                                kPoolOverInfo, kPowerPCISA);
    UniversalProcPtr uop = NewRoutineDescriptor((ProcPtr)pool_overp,
                                                kPoolOverInfo, kPowerPCISA);
    UniversalProcPtr ui = NewRoutineDescriptor((ProcPtr)pool_info,
                                               kPoolDisposedInfo, kPowerPCISA);
    if (!p || !un || !ud || !uq || !ul || !uc || !uoh || !uop || !ui) {
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
    BlockMoveData(uc, &t.rd_client, sizeof(RoutineDescriptor));
    BlockMoveData(uoh, &t.rd_overh, sizeof(RoutineDescriptor));
    BlockMoveData(uop, &t.rd_overp, sizeof(RoutineDescriptor));
    DisposeRoutineDescriptor(uoh);
    DisposeRoutineDescriptor(uop);
    BlockMoveData(ui, &t.rd_info, sizeof(RoutineDescriptor));
    DisposeRoutineDescriptor(ui);
    t.overflow = 1;
    DisposeRoutineDescriptor(un);
    DisposeRoutineDescriptor(ud);
    DisposeRoutineDescriptor(uq);
    DisposeRoutineDescriptor(ul);
    DisposeRoutineDescriptor(uc);
    /* GetSystemClientProcess: CLR.W -(A7) (the OSErr); MOVE.L 6(A7),-(A7)
       (the ProcessSerialNumber); MOVE.W #$3F,-(A7); _OSDispatch; MOVE.W
       (A7)+,D0; EXT.L D0; RTS. */
    const unsigned short client[kPoolClient] = {
        0x4267, 0x2F2F, 0x0006, 0x3F3C, 0x003F, 0xA88F, 0x301F, 0x48C0,
        0x4E75};
    memcpy(t.client, client, sizeof client);
    /* A selector-only call: CLR.W -(A7) (the OSErr); MOVE.W 8(A7),-(A7)
       (the selector); _OSDispatch; MOVE.W (A7)+,D0; EXT.L D0; RTS. */
    const unsigned short selcall[kPoolSel] = {
        0x4267, 0x3F2F, 0x0008, 0xA88F, 0x301F, 0x48C0, 0x4E75};
    memcpy(t.selcall, selcall, sizeof selcall);
    UniversalProcPtr us = NewRoutineDescriptor((ProcPtr)p->selcall,
                                               kPoolClientInfo, kM68kISA);
    if (us) {
        BlockMoveData(us, &t.rd_sel, sizeof(RoutineDescriptor));
        DisposeRoutineDescriptor(us);
    }
    /* _MemoryDispatch (D0 the selector, A0 and A1 its arguments; D0 the
       result, A0 the pairs GetPhysical filled):
         00 tst.w   enabled
         06 beq.s   54
         08 cmpi.w  #10,d0           ; LockMemoryForOutput, which the disk
         0C beq.s   14               ; driver calls for a write with
         0E cmpi.w  #5,d0            ; virtual memory on; else selectors
         12 bhi.s   54               ; 0-5 only
         14 move.l  d1,-(sp)
         16 move.l  a0,d1            ; the address,
         18 cmpi.w  #5,d0
         1C bne.s   20
         1E move.l  (a0),d1          ; or GetPhysical's table's
         20 cmp.l   sparse_lo,d1
         26 bcs.s   52
         28 cmp.l   sparse_hi,d1
         2E bcc.s   52
         30 move.l  a1,-(sp)         ; pool_md(selector, a0, a1)
         32 move.l  a0,-(sp)
         34 ext.l   d0
         36 move.l  d0,-(sp)
         38 movea.l #rd_md,a0
         3E jsr     (a0)
         40 lea     12(sp),sp
         44 move.l  d0,d1
         46 swap    d1
         48 movea.w d1,a0            ; the pairs
         4A ext.l   d0               ; the error
         4C move.l  (sp)+,d1
         4E tst.w   d0
         50 rts
         52 move.l  (sp)+,d1
         54 move.l  md_old,-(sp)
         5A rts */
    UniversalProcPtr um = NewRoutineDescriptor((ProcPtr)pool_md, 0xFF1,
                                               kPowerPCISA);
    if (um) {
        BlockMoveData(um, &t.rd_md, sizeof(RoutineDescriptor));
        DisposeRoutineDescriptor(um);
    }
    volatile unsigned long *md_e = trap_entry(0xA05C);
    t.md_old = *md_e;
    {
        unsigned long en = (unsigned long)&p->enabled;
        unsigned long lo = (unsigned long)&p->sparse_lo;
        unsigned long hi = (unsigned long)&p->sparse_hi;
        unsigned long rm = (unsigned long)&p->rd_md;
        unsigned long mo = (unsigned long)&p->md_old;
        const unsigned short md[kPoolMd] = {
            0x4A79, HI16(en), LO16(en), 0x674C,
            0x0C40, 0x000A, 0x6706,
            0x0C40, 0x0005, 0x6240,
            0x2F01, 0x2208, 0x0C40, 0x0005, 0x6602, 0x2210,
            0xB2B9, HI16(lo), LO16(lo), 0x652A,
            0xB2B9, HI16(hi), LO16(hi), 0x6422,
            0x2F09, 0x2F08, 0x48C0, 0x2F00,
            0x207C, HI16(rm), LO16(rm), 0x4E90,
            0x4FEF, 0x000C,
            0x2200, 0x4841, 0x3041, 0x48C0, 0x221F, 0x4A40, 0x4E75,
            0x221F,
            0x2F39, HI16(mo), LO16(mo), 0x4E75};
        memcpy(t.md, md, sizeof md);
    }
    /* The trap table must be where the engine writes it. */
    volatile unsigned long *osd_e = trap_entry(_OSDispatch);
    volatile unsigned long *dh_e = trap_entry(_DisposeHandle);
    volatile unsigned long *ets_e = trap_entry(_ExitToShell);
    volatile unsigned long *gne_e = lowmem(kGNEFilter);
    t.osd_old = *osd_e;
    t.dh_old = *dh_e;
    t.ets_old = *ets_e;
    t.gne_old = *gne_e;
    volatile unsigned long *nh_e = trap_entry(_NewHandle);
    volatile unsigned long *np_e = trap_entry(_NewPtr);
    volatile unsigned long *dp_e = trap_entry(_DisposePtr);
    t.nh_old = *nh_e;
    t.np_old = *np_e;
    t.dp_old = *dp_e;
    if (t.nh_old != (unsigned long)NGetTrapAddress(_NewHandle, kOSTrapType) ||
        t.np_old != (unsigned long)NGetTrapAddress(_NewPtr, kOSTrapType) ||
        t.dp_old != (unsigned long)NGetTrapAddress(_DisposePtr,
                                                   kOSTrapType) ||
        t.osd_old != (unsigned long)NGetTrapAddress(_OSDispatch,
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
    unsigned long ri = (unsigned long)&p->rd_info;
    const unsigned short osd[kPoolOsd] = {
        0x4A79, HI16(en), LO16(en), 0x6768,
        0x4A79, HI16(in), LO16(in), 0x6660,
        0x0C6F, 0x001D, 0x0004, 0x6740,
        0x0C6F, 0x003A, 0x0004, 0x6650,
        0x558F, 0x2F2F, 0x000C, 0x2F2F, 0x000C, 0x3F3C, 0x003A,
        0x2079, HI16(oo), LO16(oo), 0x4E90,
        0x301F, 0x3F40, 0x000E, 0x6612,
        0x2F2F, 0x000A, 0x2F2F, 0x000A,
        0x207C, HI16(ri), LO16(ri), 0x4E90, 0x508F,
        0x205F, 0x4FEF, 0x000A, 0x4ED0,
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
    UniversalProcPtr ut = NewRoutineDescriptor((ProcPtr)pool_top, 0xC1,
                                               kPowerPCISA);
    if (ut) {
        BlockMoveData(ut, &t.rd_top, sizeof(RoutineDescriptor));
        DisposeRoutineDescriptor(ut);
    }
    alloc_stub(t.nh, en, (unsigned long)&p->nh_old,
               (unsigned long)&p->rd_overh, (unsigned long)&p->sparse_lo,
               (unsigned long)&p->sparse_hi, (unsigned long)&p->rd_top);
    alloc_stub(t.np, en, (unsigned long)&p->np_old,
               (unsigned long)&p->rd_overp, (unsigned long)&p->sparse_lo,
               (unsigned long)&p->sparse_hi, (unsigned long)&p->rd_top);
    volatile unsigned long *sh_e = trap_entry(0xA024); /* SetHandleSize */
    volatile unsigned long *rh_e = trap_entry(0xA027); /* ReallocateHandle */
    volatile unsigned long *sp_e = trap_entry(0xA020); /* SetPtrSize */
    t.sh_old = *sh_e;
    t.rh_old = *rh_e;
    t.sp_old = *sp_e;
    grow_stub(t.sh, en, (unsigned long)&p->sh_old,
              (unsigned long)&p->sparse_lo, (unsigned long)&p->sparse_hi,
              (unsigned long)&p->rd_top);
    grow_stub(t.rh, en, (unsigned long)&p->rh_old,
              (unsigned long)&p->sparse_lo, (unsigned long)&p->sparse_hi,
              (unsigned long)&p->rd_top);
    grow_stub(t.spz, en, (unsigned long)&p->sp_old,
              (unsigned long)&p->sparse_lo, (unsigned long)&p->sparse_hi,
              (unsigned long)&p->rd_top);
    unsigned long po = (unsigned long)&p->dp_old;
    const unsigned short dp[kPoolDp] = {
        0x4A79, HI16(en), LO16(en), 0x6740,
        0x2408, 0x673C, 0x2242, 0x2269, 0xFFFC,
        0x2409, 0x6732,
        0x0CA9, 0x456E, 0x6654, 0xFFF0, 0x6628,
        0xB3E9, 0xFFF4, 0x6622,
        0x2F09, 0x2279, HI16(po), LO16(po), 0x4E91,
        0x2F00, 0x2F2F, 0x0004, 0x227C, HI16(rd), LO16(rd), 0x4E91,
        0x588F, 0x201F, 0x588F, 0x4A40, 0x4E75,
        0x2F39, HI16(po), LO16(po), 0x4E75};
    memcpy(t.dp, dp, sizeof dp);
    BlockMove(&t, p, sizeof t); /* flushes the code caches */
    *nh_e = (unsigned long)p->nh;
    *np_e = (unsigned long)p->np;
    *dp_e = (unsigned long)p->dp;
    if (um && us)
        *md_e = (unsigned long)p->md;
    if (ut) {
        *sh_e = (unsigned long)p->sh;
        *rh_e = (unsigned long)p->rh;
        *sp_e = (unsigned long)p->spz;
    }
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
    keep_one(s, trap_entry(_NewHandle), p->nh_old, p->nh);
    keep_one(s, trap_entry(_NewPtr), p->np_old, p->np);
    keep_one(s, trap_entry(_DisposePtr), p->dp_old, p->dp);
    keep_one(s, trap_entry(0xA05C), p->md_old, p->md);
    keep_one(s, trap_entry(0xA024), p->sh_old, p->sh);
    keep_one(s, trap_entry(0xA027), p->rh_old, p->rh);
    keep_one(s, trap_entry(0xA020), p->sp_old, p->spz);
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
    remove_one(trap_entry(0xA020), p->sp_old, p->spz);
    remove_one(trap_entry(0xA027), p->rh_old, p->rh);
    remove_one(trap_entry(0xA024), p->sh_old, p->sh);
    remove_one(trap_entry(0xA05C), p->md_old, p->md);
    remove_one(trap_entry(_DisposePtr), p->dp_old, p->dp);
    remove_one(trap_entry(_NewPtr), p->np_old, p->np);
    remove_one(trap_entry(_NewHandle), p->nh_old, p->nh);
    remove_one(lowmem(kGNEFilter), p->gne_old, p->gne);
    remove_one(trap_entry(_ExitToShell), p->ets_old, p->ets);
    remove_one(trap_entry(_DisposeHandle), p->dh_old, p->dh);
    remove_one(trap_entry(_OSDispatch), p->osd_old, p->osd);
}

static void preempt_set(State *s, int on);

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
        /* Application VSIDs off before the kernel's mode, so that no store
           meets a page read-only by owner in kKcbPass. After mode is off,
           the timer protects nothing, so every page opened here stays
           open. Off is without preemption too. */
        preempt_set(s, 0);
        spaces_set(s, 0);
        set_deny(s, kModeOff);
        s->mode = kModeOff;
        for (int i = 0; i < kMaxDomains; i++)
            if (s->domains[i].live)
                unprotect_domain(s, i);
        return;
    }
    set_deny(s, mode);
    s->mode = mode;
    if (old == kModeOff) {
        spaces_set(s, 1);
        protect_others(s);
        preempt_set(s, 1);
    }
}

/* Make code written at [p, p + len) the code the CPU runs. */
static void flush_code(volatile void *p, unsigned long len)
{
    unsigned long a = (unsigned long)p & ~31UL, end = (unsigned long)p + len;
    for (unsigned long x = a; x < end; x += 32)
        __asm__ volatile("dcbst 0,%0" : : "r"(x) : "memory");
    __asm__ volatile("sync");
    for (unsigned long x = a; x < end; x += 32)
        __asm__ volatile("icbi 0,%0" : : "r"(x) : "memory");
    __asm__ volatile("sync\n\tisync");
}

/* Where the Process Manager switches processes, in 9.2.2. The code moves
   with each boot, so look for it in the system heap.

   The switch first loads the next process's low memory from its PCB
   (r31), still on the previous process's stack:
       stw  r29,0x2AA(0)     ; ApplZone, on one path
       ...
       stw  r29,0x110(0)
       lwz  r3,0x30(r31)     ; on the other
       stw  r3,0x2AA(0)
   S:  lwz  r3,0x2C(r31)     ; both paths go on here
       stw  r3,0x130(0)      ; ApplLimit
   Then its native context switch loads the next process's registers from
   the context at r4:
       lwz  r1,524(r4)
       lwz  r0,516(r4)
       mtlr r0
       lwz  r2,520(r4)
       lmw  r13,388(r4)
       ...                   ; FPSCR, XER, f14-f31
       lfd  f31,168(r4)
   B:  blr
   At B the next process's stack and ApplZone are both in place, so B is
   the better site: at S the Process Manager still stores into the
   previous process's stack, which would fault. r0 is dead at both. */
static int pm_site_at(const volatile unsigned long *w)
{
    return w[0] == kPmSite && w[-1] == 0x906002AA && w[-2] == 0x807F0030 &&
           w[1] == 0x90600130 && w[-3] == 0x93A00110 && w[-8] == 0x93A002AA;
}

static int pm_switch_at(const volatile unsigned long *w)
{
    return w[0] == 0x4E800020 && w[-1] == 0xCBE400A8 &&
           w[-27] == 0x8024020C && w[-26] == 0x80040204 &&
           w[-24] == 0x80440208 && w[-23] == 0xB9A40184;
}

/* B, or else S; 0 when neither is found exactly once. */
static unsigned long pm_find(void)
{
    unsigned long lo = lowmem_long(0x2A6); /* SysZone */
    unsigned long hi = *(volatile unsigned long *)lo;
    unsigned long site = 0, sw = 0;
    int nsite = 0, nsw = 0;
    const volatile unsigned long *w =
        (const volatile unsigned long *)((lo + 128) & ~3UL);
    const volatile unsigned long *end =
        (const volatile unsigned long *)((hi & ~3UL) - 8);
    for (; w < end; w++) {
        if (*w == kPmSite && pm_site_at(w)) {
            site = (unsigned long)w;
            nsite++;
        } else if (*w == 0x4E800020 && pm_switch_at(w)) {
            sw = (unsigned long)w;
            nsw++;
        }
    }
    return nsw == 1 ? sw : nsite == 1 ? site : 0;
}

/* The site becomes a branch to a stub in the system heap that saves what
   MP call 109 changes (kcb.h, nk_call) but r0, counts the call, passes
   the new ApplZone to kKcbOpSwitch, restores, and runs the replaced
   instruction: at B, the blr itself (the site is a b); at S, the lwz,
   then a blr (the site is a bl: LR is free there). With no control block
   registered (the engine gone without unpatching), the call fails and
   nothing changes. */
static unsigned long *pm_stub_new(unsigned long insn)
{
    unsigned long *p = (unsigned long *)NewPtrSysClear(kStubLongs * 4);
    if (!p)
        return 0;
    unsigned long save = (unsigned long)(p + kStubSave);
    unsigned long lis = 0x3C600000 | save >> 16;    /* lis r3,save@h */
    unsigned long ori = 0x60630000 | (save & 0xFFFF); /* ori r3,r3,save@l */
    unsigned long *c = p + kStubCode;
    p[kStubInsn] = insn;
    *c++ = 0x3C000000 | save >> 16;    /* lis r0,save@h */
    *c++ = 0x60000000 | (save & 0xFFFF); /* ori r0,r0,save@l */
    *c++ = 0x7C60012E;                 /* stwx r3,0,r0 */
    *c++ = 0x7C030378;                 /* mr r3,r0 */
    for (unsigned long r = 4; r <= 12; r++)
        *c++ = 0x90030000 | r << 21 | (r - 3) * 4; /* stw rN,4(N-3)(r3) */
    *c++ = 0x7C0802A6; /* mflr r0 */
    *c++ = 0x90030028; /* stw r0,40(r3) */
    *c++ = 0x7C0902A6; /* mfctr r0 */
    *c++ = 0x9003002C; /* stw r0,44(r3) */
    *c++ = 0x7C0102A6; /* mfxer r0 */
    *c++ = 0x90030030; /* stw r0,48(r3) */
    *c++ = 0x7C000026; /* mfcr r0 */
    *c++ = 0x90030034; /* stw r0,52(r3) */
    *c++ = 0x80830000 | kStubCalls * 4; /* lwz r4,calls(r3) */
    *c++ = 0x38840001;                  /* addi r4,r4,1 */
    *c++ = 0x90830000 | kStubCalls * 4; /* stw r4,calls(r3) */
    *c++ = 0xA08002AA; /* lhz r4,0x2AA(0): ApplZone, 2-byte aligned */
    *c++ = 0xA0A002AC; /* lhz r5,0x2AC(0) */
    *c++ = 0x5085801E; /* rlwimi r5,r4,16,0,15 */
    *c++ = 0x7CA42B78; /* mr r4,r5 */
    *c++ = 0x38000000 | kMPEnforcer; /* li r0,109 */
    *c++ = 0x38600000 | kKcbOpSwitch; /* li r3,5 */
    *c++ = 0x44000002; /* sc */
    *c++ = lis;
    *c++ = ori;
    *c++ = 0x80030034; /* lwz r0,52(r3) */
    *c++ = 0x7C0FF120; /* mtcr r0 */
    *c++ = 0x80030030; /* lwz r0,48(r3) */
    *c++ = 0x7C0103A6; /* mtxer r0 */
    *c++ = 0x8003002C; /* lwz r0,44(r3) */
    *c++ = 0x7C0903A6; /* mtctr r0 */
    *c++ = 0x80030028; /* lwz r0,40(r3) */
    *c++ = 0x7C0803A6; /* mtlr r0 */
    for (unsigned long r = 4; r <= 12; r++)
        *c++ = 0x80030000 | r << 21 | (r - 3) * 4; /* lwz rN,4(N-3)(r3) */
    *c++ = 0x80630000; /* lwz r3,0(r3) */
    *c++ = insn;
    if (insn != 0x4E800020)
        *c++ = 0x4E800020; /* blr */
    flush_code(p + kStubCode, (unsigned long)(c - (p + kStubCode)) * 4);
    return p;
}

/* Install the hook (task level, before the system heap is watched). A
   branch reaches 32 MB either way, or the lowest 32 MB absolutely. */
static void pm_hook(State *s)
{
    unsigned long site = pm_find();
    if (!site)
        return;
    unsigned long old = *(volatile unsigned long *)site;
    unsigned long *stub = pm_stub_new(old);
    if (!stub)
        return;
    unsigned long to = (unsigned long)(stub + kStubCode);
    unsigned long link = old == 0x4E800020 ? 0 : 1; /* b at B, bl at S */
    long off = (long)(to - site);
    unsigned long insn;
    if (off >= -0x2000000L && off < 0x2000000L)
        insn = 0x48000000 | ((unsigned long)off & 0x03FFFFFC) | link;
    else if (to < 0x2000000UL)
        insn = 0x48000002 | to | link; /* absolute */
    else {
        DisposePtr((Ptr)stub);
        return;
    }
    hold(s, stub, kStubLongs * 4, 1);
    s->pm_stub = stub;
    s->pm_site = site;
    *(volatile unsigned long *)site = insn;
    flush_code((volatile void *)site, 4);
}

/* Launch sizes (engine.h). The Process Manager's routine that reads a
   'SIZE' resource into a record at r31 (flags, the preferred size at +2,
   the minimum at +6) ends, in 9.2.2:
       lwz  r3,802(0)        ; the default stack, added to both sizes
       cmpwi r3,8192
       ...
       stw  r7,6(r31)
   Z:  mr   r3,r29           ; the result: whether there was a resource
       addi r1,r1,80
   Z becomes a branch to a stub that raises the preferred size, runs Z and
   returns to the instruction after it. The volatile registers and CTR are
   dead there. The stub:
       lis   r5,stub@h ; ori r5,r5,stub@l
       lwz   r6,0(r5)        ; the cap; 0: off
       lwz   r4,2(r31)       ; the preferred size
       cmplwi r6,0 ; beq done
       rlwinm. r7,r4,0,0,1 ; bne done   ; 1 GB or more: leave it
       slwi  r7,r4,2         ; kSizeFactor times
       cmplw r7,r6 ; ble +8 ; mr r7,r6  ; no more than the cap
       cmplw r7,r4 ; ble done           ; and no less than asked
       stw   r7,2(r31)
       ...                   ; count it; keep the two sizes
   done: mr  r3,r29
       lis   r5,back@h ; ori r5,r5,back@l ; mtctr r5 ; bctr */
static int size_site_at(const volatile unsigned long *w)
{
    return w[0] == kSizeInsn && w[1] == 0x38210050 && w[-1] == 0x90FF0006 &&
           w[-11] == 0x2C032000 && w[-12] == 0x80600322;
}

/* The stub, returning to the instruction after site (0: not known yet). */
static void size_stub_aim(unsigned long *p, unsigned long site)
{
    unsigned long base = (unsigned long)p, back = site + 4;
    unsigned long *c = p + kSzCode;
    *c++ = 0x3CA00000 | base >> 16;      /* lis r5,base@h */
    *c++ = 0x60A50000 | (base & 0xFFFF); /* ori r5,r5,base@l */
    *c++ = 0x809F0002;                   /* lwz r4,2(r31) */
    /* A sparse partition (kSzSparse not 0): that size, for an application
       that asks for 640 KB or more and less than it. */
    *c++ = 0x80C50000 | kSzSparse * 4;   /* lwz r6,sparse(r5) */
    *c++ = 0x28060000;                   /* cmplwi r6,0 */
    *c++ = 0x41820020;                   /* beq normal */
    *c++ = 0x3CE0000A;                   /* lis r7,0x000A */
    *c++ = 0x7C043840;                   /* cmplw r4,r7 */
    *c++ = 0x41800014;                   /* blt normal */
    *c++ = 0x7C062040;                   /* cmplw r6,r4 */
    *c++ = 0x40810050;                   /* ble done */
    *c++ = 0x7CC73378;                   /* mr r7,r6 */
    *c++ = 0x48000030;                   /* b store */
    *c++ = 0x80C50000 | kSzCap * 4;      /* normal: lwz r6,cap(r5) */
    *c++ = 0x28060000;                   /* cmplwi r6,0 */
    *c++ = 0x4182003C;                   /* beq done */
    *c++ = 0x54870003;                   /* rlwinm. r7,r4,0,0,1 */
    *c++ = 0x40820034;                   /* bne done */
    *c++ = 0x5487103A;                   /* slwi r7,r4,2 */
    *c++ = 0x7C073040;                   /* cmplw r7,r6 */
    *c++ = 0x40810008;                   /* ble +8 */
    *c++ = 0x7CC73378;                   /* mr r7,r6 */
    *c++ = 0x7C072040;                   /* cmplw r7,r4 */
    *c++ = 0x4081001C;                   /* ble done */
    *c++ = 0x90FF0002;                   /* store: stw r7,2(r31) */
    *c++ = 0x81050000 | kSzRaised * 4;   /* lwz r8,raised(r5) */
    *c++ = 0x39080001;                   /* addi r8,r8,1 */
    *c++ = 0x91050000 | kSzRaised * 4;   /* stw r8,raised(r5) */
    *c++ = 0x90850000 | kSzFrom * 4;     /* stw r4,from(r5) */
    *c++ = 0x90E50000 | kSzTo * 4;       /* stw r7,to(r5) */
    *c++ = kSizeInsn;                    /* done: mr r3,r29 */
    *c++ = 0x3CA00000 | back >> 16;      /* lis r5,back@h */
    *c++ = 0x60A50000 | (back & 0xFFFF); /* ori r5,r5,back@l */
    *c++ = 0x7CA903A6;                   /* mtctr r5 */
    *c++ = 0x4E800420;                   /* bctr */
    flush_code(p + kSzCode, (unsigned long)(c - (p + kSzCode)) * 4);
}

/* The branch from site to the stub's code, or 0 when it does not reach. */
static unsigned long size_branch(State *s, unsigned long site)
{
    unsigned long to = (unsigned long)(s->size_stub + kSzCode);
    long off = (long)(to - site);
    if (off >= -0x2000000L && off < 0x2000000L)
        return 0x48000000 | ((unsigned long)off & 0x03FFFFFC); /* b */
    if (to < 0x2000000UL)
        return 0x48000002 | to; /* ba */
    return 0;
}

/* Make the stub (engine_start, before the system heap is watched) and, with
   the Process Manager's code in the system heap (virtual memory off), hook
   the site. */
static void size_hook(State *s)
{
    unsigned long *p = (unsigned long *)NewPtrSysClear(kSzLongs * 4);
    if (!p)
        return;
    size_stub_aim(p, 0);
    hold(s, p, kSzLongs * 4, 1);
    s->size_stub = p;
    unsigned long lo = lowmem_long(0x2A6); /* SysZone */
    unsigned long hi = *(volatile unsigned long *)lo;
    unsigned long site = 0;
    int n = 0;
    for (const volatile unsigned long *w =
             (const volatile unsigned long *)((lo + 128) & ~3UL);
         (unsigned long)w < (hi & ~3UL) - 8; w++)
        if (*w == kSizeInsn && size_site_at(w)) {
            site = (unsigned long)w;
            n++;
        }
    unsigned long insn = n == 1 ? size_branch(s, site) : 0;
    if (!insn)
        return;
    size_stub_aim(p, site);
    s->size_site = site;
    *(volatile unsigned long *)site = insn;
    flush_code((volatile void *)site, 4);
}

/* With virtual memory on: the site, found by pm_hook_mapped in the
   file-mapped code, patched through the kernel on a held page. */
static void size_hook_mapped(State *s, unsigned long site)
{
    if (!s->size_stub || s->size_site)
        return;
    unsigned long insn = size_branch(s, site);
    if (!insn)
        return;
    size_stub_aim(s->size_stub, site);
    HoldMemory((void *)(site & kPageMask), kPage);
    if (*(volatile unsigned long *)site != kSizeInsn ||
        kcb_patch(site, kSizeInsn, insn)) {
        UnholdMemory((void *)(site & kPageMask), kPage);
        s->c.kernel_errors++;
        return;
    }
    s->size_site = site;
    s->size_mapped = 1;
}

/* Put the instruction back and free the stub, as pm_unhook. */
static void size_unhook(State *s)
{
    if (!s->size_stub)
        return;
    s->size_stub[kSzCap] = 0;
    if (s->size_site && s->size_mapped) {
        volatile unsigned long *w = (volatile unsigned long *)s->size_site;
        if (!s->kcb || kcb_patch(s->size_site, *w, kSizeInsn)) {
            s->c.kernel_errors++;
            s->size_site = 0;
            s->size_stub = 0;
            s->size_mapped = 0;
            return;
        }
        UnholdMemory((void *)(s->size_site & kPageMask), kPage);
        s->size_mapped = 0;
    } else if (s->size_site) {
        *(volatile unsigned long *)s->size_site = kSizeInsn;
        flush_code((volatile void *)s->size_site, 4);
    }
    hold(s, s->size_stub, kSzLongs * 4, 0);
    DisposePtr((Ptr)s->size_stub);
    s->size_site = 0;
    s->size_stub = 0;
}

/* Sparse partitions (engine.h). Three sites in the Process Manager's
   fragment, found from the size hook's site Z (the fragment's code is the
   same every boot) and checked by their instructions:

   P1, Z - 0x14798, where it allocates a new process's partition (r3 the
   size, r31 the process entry; the handle goes to entry + 48 and its
   block's address to entry + 44):
       bl    alloc          ; its allocator in its own zone
       nop
   P2, Z - 0x134DC, where it frees the partition at quit and after a
   failed launch:
       lwz   r3,48(r31)
       bl    DisposeHandle  ; import glue
       lwz   r2,20(r1)
   P3, Z - 0x8CD0, in the quit code that makes the partition's block the
   scrap when the scrap is in the dying heap (it shrinks the block and
   keeps it):
       cmpwi r0,0           ; a flag that is 0
       beq   take           ; Z - 0x8C20: take the partition
                            ; else copy the scrap to a new handle

   The block the stubs live in (system heap, never freed), in longs:
   kPtLo and kPtHi the sparse range; kPtNew the engine's part_new (its
   transition vector), 0 once the engine stops; kPtAlloc and kPtDispose
   the routines P1 and P2 called; kPtCopy and kPtTake P3's two ways on;
   kPtMp the master pointers of the handles part_new returns; then code.

   P1's stub calls part_new(size): a handle, 0 (no memory: the Process
   Manager then tries a smaller size), or -1 for "not sparse", and then
   calls the Process Manager's allocator. P2's stub needs no engine: for a
   handle among kPtMp it clears the master pointer and returns; part_reap
   gives the memory back. P3's stub takes the copy way for a partition in
   the range.

   The Process Manager calls HLock on the handle at launch and
   GetHandleSize at quit (to clear the Notification and Time Manager
   queues of entries in the partition), so the block has a Memory Manager
   header: a locked relocatable block of the size, its master pointer
   given as an offset from the Process Manager's zone. */
enum {
    kPtLo = 0,
    kPtHi = 1,
    kPtNew = 2,
    kPtAlloc = 3,
    kPtDispose = 4,
    kPtCopy = 5,
    kPtTake = 6,
    kPtMp = 8,
    kPtCode = kPtMp + kMaxParts,
    kPtLongs = kPtCode + 72,
    kP1Delta = 0x14798,
    kP2Delta = 0x134DC,
    kP3Delta = 0x8CD0
};

/* Whether a sparse partition can be made now. */
static int part_room(State *s)
{
    if (!s->part || !s->sparse_area || s->mode == kModeOff || !s->mem_on)
        return 0;
    int slot = 0;
    for (int i = 0; i < kMaxParts; i++)
        if (!s->parts[i].units)
            slot = 1;
    unsigned long total = (s->sparse_hi - s->sparse_lo) / kSparseUnit;
    unsigned long mask = (1UL << kPartUnits) - 1;
    for (unsigned long i = 0; slot && i + kPartUnits <= total; i++)
        if (!(s->sparse_used & (mask << i)))
            return 1;
    return 0;
}

/* From P1's stub, in the launching application's context, with the
   Process Manager's zone current: the partition's handle, 0, or -1. */
static long part_new(long size)
{
    State *s = g_state;
    if (!s || !s->running || !s->part || !s->sparse_area ||
        s->mode == kModeOff || !s->mem_on || !s->pm_zone ||
        (unsigned long)size < kPartMin)
        return -1;
    unsigned long units =
        ((unsigned long)size + kPage + kSparseUnit - 1) / kSparseUnit;
    int slot = -1;
    for (int i = 0; i < kMaxParts && slot < 0; i++)
        if (!s->parts[i].units)
            slot = i;
    if (slot < 0 || units > kSparseMost || !sparse_ensure(s, 64, 0))
        return -1;
    unsigned long lo = sparse_alloc(s, units);
    if (!lo)
        return -1;
    unsigned long p = lo + kPage, end = lo + units * kSparseUnit;
    unsigned long *mp = &s->part[kPtMp + slot];
    unsigned long *h = (unsigned long *)(p - 16);
    h[0] = 0;
    h[1] = 0xAB000004UL;                /* relocatable; 4 bytes of padding */
    h[2] = (unsigned long)size + 16;
    h[3] = (unsigned long)mp - s->pm_zone;
    *mp = p;
    /* The Memory Manager takes a handle only below its top of memory. */
    volatile unsigned long *top = lowmem(0x1E10);
    if (*top < end)
        *top = end;
    s->parts[slot].lo = lo;
    s->parts[slot].units = units;
    g_backed_at[slot] = 0; /* not the last partition's count */
    s->part_made++;
    return (long)mp;
}

/* Give back the partitions P2's stub has freed (task level). */
static void part_reap(State *s)
{
    if (!s->part)
        return;
    for (int i = 0; i < kMaxParts; i++)
        if (s->parts[i].units && !s->part[kPtMp + i]) {
            sparse_release(s, s->parts[i].lo, s->parts[i].units);
            s->parts[i].units = 0;
            s->part_freed++;
        }
}

static unsigned long *emit_base(unsigned long *c, unsigned reg,
                                unsigned long base)
{
    *c++ = 0x3C000000 | reg << 21 | base >> 16;                /* lis */
    *c++ = 0x60000000 | reg << 21 | reg << 16 | (base & 0xFFFF); /* ori */
    return c;
}

/* Replace the word old at a site with insn. In file-mapped code (virtual
   memory on) the store goes through the kernel, on a page part_hook
   holds, as for the size hook. Returns 0 when it is done. */
static int part_put(State *s, volatile unsigned long *site,
                    unsigned long old, unsigned long insn)
{
    if (s->size_mapped)
        return *site != old || kcb_patch((unsigned long)site, old, insn);
    *site = insn;
    flush_code(site, 4);
    return 0;
}

/* Hook the three sites: from engine_start, after size_hook and
   sparse_start, with the fragment in the system heap (virtual memory
   off); from pm_hook_mapped, once the size hook's site is found in the
   file-mapped code (virtual memory on; task level, it reads code in). */
static void part_hook(State *s)
{
    if (!s->size_site || !s->sparse_area || s->part)
        return;
    volatile unsigned long *p1 =
        (volatile unsigned long *)(s->size_site - kP1Delta);
    volatile unsigned long *p2 =
        (volatile unsigned long *)(s->size_site - kP2Delta);
    volatile unsigned long *p3 =
        (volatile unsigned long *)(s->size_site - kP3Delta);
    /* Held from here on, so that the pages keep the change; a page with
       nothing behind it cannot be held. */
    if (s->size_mapped) {
        if (HoldMemory((void *)(p1 - 1), 12))
            return;
        if (HoldMemory((void *)(p2 - 1), 12)) {
            UnholdMemory((void *)(p1 - 1), 12);
            return;
        }
        if (HoldMemory((void *)(p3 - 1), 12)) {
            UnholdMemory((void *)(p1 - 1), 12);
            UnholdMemory((void *)(p2 - 1), 12);
            return;
        }
    }
    int ok = 0;
    unsigned long *b = 0;
    if (p1[0] != 0x4800E109 || p1[1] != 0x60000000 || p1[-1] != 0x807F03E2 ||
        p2[0] != 0x48014B45 || p2[-1] != 0x807F0030 || p2[1] != 0x80410014 ||
        p3[0] != 0x418200B0 || p3[-1] != 0x2C000000 || p3[1] != 0x809B0000)
        goto out;
    b = (unsigned long *)NewPtrSysClear(kPtLongs * 4);
    if (!b || (unsigned long)b + kPtLongs * 4 >= 0x2000000UL) {
        if (b)
            DisposePtr((Ptr)b);
        goto out;
    }
    hold(s, b, kPtLongs * 4, 1);
    unsigned long base = (unsigned long)b;
    b[kPtLo] = s->sparse_lo;
    b[kPtHi] = s->sparse_hi;
    b[kPtNew] = (unsigned long)part_new; /* its transition vector */
    b[kPtAlloc] = (unsigned long)p1 + 0xE108;
    b[kPtDispose] = (unsigned long)p2 + 0x14B44;
    b[kPtCopy] = (unsigned long)p3 + 4;
    b[kPtTake] = (unsigned long)p3 + 0xB0;
    unsigned long *c = b + kPtCode;

    unsigned long *s1 = c; /* P1: r3 the size */
    *c++ = 0x7C0802A6;                 /* mflr r0 */
    *c++ = 0x90010008;                 /* stw r0,8(r1) */
    *c++ = 0x9421FFC0;                 /* stwu r1,-64(r1) */
    *c++ = 0x90410014;                 /* stw r2,20(r1) */
    *c++ = 0x90610038;                 /* stw r3,56(r1) */
    c = emit_base(c, 12, base);
    *c++ = 0x818C0000 | kPtNew * 4;    /* lwz r12,new(r12) */
    *c++ = 0x280C0000;                 /* cmplwi r12,0 */
    *c++ = 0x41820020;                 /* beq orig */
    *c++ = 0x800C0000;                 /* lwz r0,0(r12) */
    *c++ = 0x804C0004;                 /* lwz r2,4(r12) */
    *c++ = 0x7C0903A6;                 /* mtctr r0 */
    *c++ = 0x4E800421;                 /* bctrl: part_new(size) */
    *c++ = 0x80410014;                 /* lwz r2,20(r1) */
    *c++ = 0x2C03FFFF;                 /* cmpwi r3,-1 */
    *c++ = 0x40820020;                 /* bne done */
    *c++ = 0x80610038;                 /* orig: lwz r3,56(r1) */
    c = emit_base(c, 12, base);
    *c++ = 0x818C0000 | kPtAlloc * 4;  /* lwz r12,alloc(r12) */
    *c++ = 0x7D8903A6;                 /* mtctr r12 */
    *c++ = 0x4E800421;                 /* bctrl */
    *c++ = 0x80410014;                 /* lwz r2,20(r1) */
    *c++ = 0x38210040;                 /* done: addi r1,r1,64 */
    *c++ = 0x80010008;                 /* lwz r0,8(r1) */
    *c++ = 0x7C0803A6;                 /* mtlr r0 */
    *c++ = 0x4E800020;                 /* blr */

    unsigned long *s2 = c; /* P2: r3 the handle, in place of the glue */
    *c++ = 0x90410014;                 /* stw r2,20(r1), as the glue does */
    c = emit_base(c, 12, base);
    *c++ = 0x396C0000 | kPtMp * 4;     /* addi r11,r12,mp */
    *c++ = 0x7C035840;                 /* cmplw r3,r11 */
    *c++ = 0x41800020;                 /* blt orig */
    *c++ = 0x396C0000 | kPtCode * 4;   /* addi r11,r12,mp end */
    *c++ = 0x7C035840;                 /* cmplw r3,r11 */
    *c++ = 0x40800014;                 /* bge orig */
    *c++ = 0x38000000;                 /* li r0,0 */
    *c++ = 0x90030000;                 /* stw r0,0(r3): freed */
    *c++ = 0x38600000;                 /* li r3,0 */
    *c++ = 0x4E800020;                 /* blr */
    *c++ = 0x816C0000 | kPtDispose * 4; /* orig: lwz r11,dispose(r12) */
    *c++ = 0x7D6903A6;                 /* mtctr r11 */
    *c++ = 0x4E800420;                 /* bctr */

    unsigned long *s3 = c; /* P3: cr0 from the flag; r27 -> the entry */
    *c++ = 0x4082002C;                 /* bne copy */
    *c++ = 0x819B0000;                 /* lwz r12,0(r27) */
    *c++ = 0x818C002C;                 /* lwz r12,44(r12): the partition */
    c = emit_base(c, 11, base);
    *c++ = 0x800B0000 | kPtLo * 4;     /* lwz r0,lo(r11) */
    *c++ = 0x7C0C0040;                 /* cmplw r12,r0 */
    *c++ = 0x41800024;                 /* blt take */
    *c++ = 0x800B0000 | kPtHi * 4;     /* lwz r0,hi(r11) */
    *c++ = 0x7C0C0040;                 /* cmplw r12,r0 */
    *c++ = 0x40800018;                 /* bge take */
    c = emit_base(c, 11, base);        /* copy: */
    *c++ = 0x816B0000 | kPtCopy * 4;   /* lwz r11,copy(r11) */
    *c++ = 0x7D6903A6;                 /* mtctr r11 */
    *c++ = 0x4E800420;                 /* bctr */
    c = emit_base(c, 11, base);        /* take: */
    *c++ = 0x816B0000 | kPtTake * 4;   /* lwz r11,take(r11) */
    *c++ = 0x7D6903A6;                 /* mtctr r11 */
    *c++ = 0x4E800420;                 /* bctr */
    flush_code(b + kPtCode, (unsigned long)(c - (b + kPtCode)) * 4);

    /* P2 and P3 first: they change nothing until P1 makes a partition. */
    if (part_put(s, p2, 0x48014B45, 0x48000003 | (unsigned long)s2) ||
        part_put(s, p3, 0x418200B0, 0x48000002 | (unsigned long)s3) ||
        part_put(s, p1, 0x4800E109, 0x48000003 | (unsigned long)s1)) {
        /* Kernel errors only (mapped code): put back what went in. The
           block stays, a site may still branch to it. */
        if (*p2 != 0x48014B45)
            kcb_patch((unsigned long)p2, *p2, 0x48014B45);
        if (*p3 != 0x418200B0)
            kcb_patch((unsigned long)p3, *p3, 0x418200B0);
        s->c.kernel_errors++;
        goto out;
    }
    s->part = b;
    ok = 1;
out:
    if (s->size_mapped && !ok) {
        UnholdMemory((void *)(p1 - 1), 12);
        UnholdMemory((void *)(p2 - 1), 12);
        UnholdMemory((void *)(p3 - 1), 12);
    }
}

/* The engine is stopping: P1 goes back to the Process Manager's
   allocator. With no sparse partition left, the sites get their
   instructions back; otherwise the stubs stay, and P2 and P3 still work
   without the engine. */
static void part_unhook(State *s)
{
    if (!s->part)
        return;
    s->part[kPtNew] = 0;
    part_reap(s);
    int live = 0;
    for (int i = 0; i < kMaxParts; i++)
        live |= s->parts[i].units != 0;
    if (!live && s->size_site) {
        volatile unsigned long *p1 =
            (volatile unsigned long *)(s->size_site - kP1Delta);
        volatile unsigned long *p2 =
            (volatile unsigned long *)(s->size_site - kP2Delta);
        volatile unsigned long *p3 =
            (volatile unsigned long *)(s->size_site - kP3Delta);
        if (!part_put(s, p1, *p1, 0x4800E109) &&
            !part_put(s, p2, *p2, 0x48014B45) &&
            !part_put(s, p3, *p3, 0x418200B0) && s->size_mapped) {
            UnholdMemory((void *)(p1 - 1), 12);
            UnholdMemory((void *)(p2 - 1), 12);
            UnholdMemory((void *)(p3 - 1), 12);
        }
    }
    s->part = 0; /* the block stays */
}

/* The cap for the next launch, and the stub's counts (task level). */
static void size_follow(State *s)
{
    unsigned long *p = s->size_stub;
    if (!p)
        return;
    unsigned long cap = 0;
    /* With virtual memory off the partition reserves RAM, so the share is
       smaller. */
    if (s->mode != kModeOff && s->mem_on && s->pm_zone)
        cap = ((unsigned long)((THz)s->pm_zone)->zcbFree /
               (s->vm ? kSizeShare : kSizeShareReal)) & ~0xFFFUL;
    p[kSzCap] = cap;
    p[kSzSparse] = cap && part_room(s) ? kPartSize : 0;
    s->size_cap = cap;
    s->size_raised = p[kSzRaised];
    s->size_from = p[kSzFrom];
    s->size_to = p[kSzTo];
    part_reap(s);
}

void engine_set_mem(State *s, int on)
{
    s->mem_on = s->want_mem = on != 0;
    if (s->pool)
        s->pool->overflow = on != 0;
    size_follow(s);
}

/* Whether the page at addr is in the kernel area area and can be read:
   the area for file-mapped code is larger than what is mapped in it, and
   a read of a page with nothing behind it is a bus error. HoldMemory
   brings a page in, and fails for such a page. */
static int page_readable(unsigned long addr, unsigned long area)
{
    unsigned long a = 0;
    if (nk_area_from_address(addr, &a) || a != area ||
        HoldMemory((void *)addr, kPage))
        return 0;
    UnholdMemory((void *)addr, kPage);
    return 1;
}

/* With virtual memory on, the Process Manager's code is file-mapped above
   logical RAM, so pm_hook does not find B. The kernel records the PC of
   the store at its last lazy switch (KernelCB.lazy_pc): with no hook, that
   is the Process Manager's first store on the next process's stack, in
   the code that holds B. Look for B in the readable pages around it
   (page_readable), and patch
   it through the kernel: kcb_patch stores through the physical address,
   so the Virtual Memory Manager sees the page clean, and the page is held
   so that it keeps the change. Task level (it reads in code from disk);
   at most kPmTries PCs. */
enum { kPmReach = 0x24000, kPmTries = 8 };

static void pm_hook_mapped(State *s)
{
    unsigned long pc = s->kcb->lazy_pc & ~3UL, area = 0;
    s->pm_tried = s->kcb->lazy_pc;
    s->pm_tries++;
    unsigned long page = pc & kPageMask, lo = page, hi = page + kPage;
    if (nk_area_from_address(pc, &area) || !page_readable(page, area))
        return;
    while (page - lo < kPmReach && page_readable(lo - kPage, area))
        lo -= kPage;
    while (hi - page < kPmReach && page_readable(hi, area))
        hi += kPage;
    unsigned long site = 0, zsite = 0;
    int n = 0, nz = 0;
    for (const volatile unsigned long *w =
             (const volatile unsigned long *)(lo + 128);
         (unsigned long)w < hi - 8; w++)
        if (*w == 0x4E800020 && pm_switch_at(w)) {
            site = (unsigned long)w;
            n++;
        } else if (*w == kSizeInsn && size_site_at(w)) {
            zsite = (unsigned long)w;
            nz++;
        }
    if (nz == 1) {
        size_hook_mapped(s, zsite);
        if (s->size_mapped)
            part_hook(s);
    }
    if (n != 1)
        return;
    /* The stub engine_start made for B, before the system heap was
       watched. */
    unsigned long to = (unsigned long)(s->pm_stub + kStubCode);
    long off = (long)(to - site);
    unsigned long insn;
    if (off >= -0x2000000L && off < 0x2000000L)
        insn = 0x48000000 | ((unsigned long)off & 0x03FFFFFC); /* b */
    else if (to < 0x2000000UL)
        insn = 0x48000002 | to; /* ba */
    else
        return;
    HoldMemory((void *)(site & kPageMask), kPage);
    if (*(volatile unsigned long *)site != 0x4E800020 || /* a PTE for it */
        kcb_patch(site, 0x4E800020, insn)) {
        UnholdMemory((void *)(site & kPageMask), kPage);
        s->c.kernel_errors++;
        return;
    }
    s->pm_site = site;
    s->pm_mapped = 1;
}

/* Put the replaced instruction back and free the stub (task level, after
   the system heap's pages are open again). If the kernel cannot put it
   back, the site still branches to the stub, so the stub stays: it does
   nothing with no control block registered. */
static void pm_unhook(State *s)
{
    if (!s->pm_stub)
        return;
    if (s->pm_site && s->pm_mapped) {
        volatile unsigned long *w = (volatile unsigned long *)s->pm_site;
        if (!s->kcb || kcb_patch(s->pm_site, *w, s->pm_stub[kStubInsn])) {
            s->c.kernel_errors++;
            s->pm_site = 0;
            s->pm_stub = 0;
            s->pm_mapped = 0;
            return;
        }
        UnholdMemory((void *)(s->pm_site & kPageMask), kPage);
        s->pm_mapped = 0;
    } else if (s->pm_site) {
        *(volatile unsigned long *)s->pm_site = s->pm_stub[kStubInsn];
        flush_code((volatile void *)s->pm_site, 4);
    }
    hold(s, s->pm_stub, kStubLongs * 4, 0);
    DisposePtr((Ptr)s->pm_stub);
    s->pm_site = 0;
    s->pm_stub = 0;
}

/* Preemption at safe points (kernel patch 6, common/hyield.s). The kernel
   sends the current application's native code to the yield stub when it
   has run kSliceMs without a switch; yield_check decides whether the stub
   calls WaitNextEvent for it. */
enum {
    kSliceMs = 20,
    kYieldRetryMs = 2,        /* the next try after a check that said no */
    kYieldStack = 0x8000,     /* free stack WaitNextEvent is given */
    kYieldFrames = 1024       /* the longest chain followed */
};

#ifndef gestaltPowerPCProcessorFeatures
#define gestaltPowerPCProcessorFeatures 'ppcf'
#endif
#ifndef gestaltPowerPCHasVectorInstructions
#define gestaltPowerPCHasVectorInstructions 4
#endif

/* The owner of the code at addr: an application's byte without
   kOwnerCode, kOwnerSystem, or 0. */
static unsigned owner_at(State *s, unsigned long addr)
{
    unsigned long page = addr >> 12;
    if (page >= s->kcb->entries)
        return 0;
    return s->owners[page] & ~kOwnerCode;
}

/* From the yield stub, in the preempted application's context and at the
   level it was preempted at, with r1 below the application's frames.
   Returns 1 when the application can call WaitNextEvent now:
   - it has given up the processor before (KernelCB.yielded), so it has
     set up the Toolbox;
   - at task level (not a deferred task or an interrupt handler);
   - r1 on the application's stack (between ApplLimit and CurStackBase,
     not a thread's), with kYieldStack free below it;
   - no Toolbox call in progress: the stack's chain ends at its base (0,
     or a link at or past CurStackBase), and its return addresses are the
     application's code until the first that is not, and none after it
     is. A chain that ends on any other link leaves its callers unknown:
     a 68k caller's switch frame (Scribble U) ends it so. A Toolbox
     routine that called back into the
     application leaves the application's own frames below its own; the
     frames that launched the application are the system's, with none of
     the application's below them. The first frame's return address is
     left out (a function that has called nothing yet has not written
     it), and so is the LR, which compilers use as a scratch register
     once they have saved it.
   Writes only the stub's block: the program running the engine may be
   another application's memory here. */
static long yield_check(unsigned long sp, unsigned long pc, unsigned long lr,
                        volatile YieldData *y)
{
    State *s = g_state;
    long ok = 0;
    unsigned long n = 0;
    y->last_sp = sp;
    y->last_pc = pc;
    y->last_lr = lr;
    y->last_base = 0;
    if (!s || !s->kcb)
        return 0;
    if (CurrentExecutionLevel() != kTaskLevel) {
        y->not_task++;
        goto no;
    }
    unsigned long zone = current_zone();
    unsigned owner = owner_at(s, zone);
    y->last_owner = owner;
    if (owner < kOwnerApp || !s->kcb->yielded[owner]) {
        y->starting++;
        goto no;
    }
    if (owner_at(s, pc) != owner) {
        y->in_call++;
        goto no;
    }
    unsigned long base = lowmem_long(0x908);  /* CurStackBase */
    unsigned long limit = lowmem_long(0x130); /* ApplLimit */
    y->last_base = base;
    if (sp >= base || sp < limit) {
        y->other_stack++;
        goto no;
    }
    if (sp - limit < kYieldStack) {
        y->low_stack++;
        goto no;
    }
    {
        unsigned long prev = sp, f = *(volatile unsigned long *)sp;
        int sys = 0;
        while (f > prev && f < base && !(f & 3)) {
            unsigned long ret = ((volatile unsigned long *)f)[2];
            if (n < 11)
                y->chain[n] = ret;
            n++;
            if (owner_at(s, ret) != owner)
                sys = 1;
            else if (sys) {
                y->in_call++;
                goto no;
            }
            if (n >= kYieldFrames)
                break;
            prev = f;
            f = *(volatile unsigned long *)f;
        }
        if (n < 11)
            y->chain[n] = f; /* where it ended */
        /* It must end at the stack's base. A chain that ends on another
           link (a Mixed Mode switch frame, where 68k code called the
           native code above it) hides who called. */
        if (f && f < base) {
            y->broken++;
            goto no;
        }
    }
    y->safe++;
    ok = 1;
no:
    y->last_n = n;
    if (!ok)
        kcb_set(kKcbOpYield, kYieldRetryMs * s->c.tb_per_ms);
    return ok;
}

/* The yield stub, in the system heap, held. It is never freed: an
   application preempted in WaitNextEvent comes back into it, maybe after
   the engine stopped. */
static void preempt_new(State *s)
{
    if (!s->kcb)
        return;
    YieldData *y = (YieldData *)NewPtrSysClear(sizeof hyield_code);
    if (!y)
        return;
    memcpy(y, hyield_code, sizeof hyield_code);
    if (y->magic != kYieldMagic) {
        DisposePtr((Ptr)y);
        return;
    }
    y->check = yield_check;
    y->wne = (void *)WaitNextEvent;
    long f = 0;
    if (Gestalt(gestaltPowerPCProcessorFeatures, &f) == noErr &&
        (f & (1L << gestaltPowerPCHasVectorInstructions)))
        *(unsigned long *)((char *)y + y->vecbr) = 0x60000000; /* nop */
    flush_code(y, sizeof hyield_code);
    hold(s, y, sizeof hyield_code, 1);
    s->yield = y;
    s->slice_ms = kSliceMs;
}

/* On with protection (engine_set_mode), off before the engine stops. */
static void preempt_set(State *s, int on)
{
    if (!s->yield || !s->kcb || on == (int)s->preempt_on)
        return;
    unsigned long entry = (unsigned long)s->yield + s->yield->entry;
    if (kcb_set2(kKcbOpPreempt, on ? s->slice_ms * s->c.tb_per_ms : 0,
                 entry)) {
        s->c.kernel_errors++; /* a kernel without patch 6 */
        return;
    }
    s->preempt_on = on;
}

/* The kernel's owner check (common/kcb.h): allocate the control block and
   the owner table in the system heap, where they outlive a program that
   stops without cleaning up, and register the block. On any failure the
   engine runs on its hooks alone, with the reason in kcb_err. Call with
   no domain marked yet. */
/* The low-memory global ApplZone, which the Process Manager sets on every
   switch. */
static void *applzone_ptr(void)
{
    return (void *)hook_ptr(0x2AA);
}

/* ApplZone's physical address, for the kernel to find the current
   application at each store it decides, or 0: the kernel then uses
   KernelCB.current as the engine sets it on its tick. Low memory never
   moves or pages out, but with virtual memory on GetPhysical translates
   only locked memory (notLockedErr); kcb_stop unlocks it. */
static unsigned long applzone_physical(State *s)
{
    if (s->vm && LockMemory(applzone_ptr(), 4))
        return 0;
    unsigned long p = kcb_physical(applzone_ptr(), 4);
    if (!p && s->vm)
        UnlockMemory(applzone_ptr(), 4);
    return p;
}

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
    /* Sparse memory: the longest run of kSparseUnit pieces above logical
       RAM, in the segments application VSIDs cover (0-5), where the
       kernel has no area. With virtual memory on the logical area goes on
       past logical RAM (mapped code files), and Mac OS keeps an area of
       one page at 0x5FFFE000, so every page is asked about. The owner
       table covers the range. */
    s->sparse_want_lo = s->sparse_want_hi = 0;
    if (lram > 0) {
        unsigned long best = 0, best_n = 0, run = 0, run_n = 0;
        for (unsigned long u = ((unsigned long)lram + kSparseUnit - 1) &
                               ~(unsigned long)(kSparseUnit - 1);
             u < 0x60000000UL; u += kSparseUnit) {
            int clear = 1;
            for (unsigned long a = u; clear && a < u + kSparseUnit;
                 a += kPage) {
                unsigned long area = 0;
                if (!nk_area_from_address(a, &area) && area)
                    clear = 0;
            }
            if (!clear) {
                run_n = 0;
                continue;
            }
            if (!run_n)
                run = u;
            if (++run_n > best_n && run_n <= kSparseUnits) {
                best = run;
                best_n = run_n;
            }
        }
        if (best_n) {
            s->sparse_want_lo = best;
            s->sparse_want_hi = best + best_n * kSparseUnit;
            if (ram < s->sparse_want_hi)
                ram = s->sparse_want_hi;
        }
    }
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
    k->applzone = applzone_physical(s);
    err = kcb_register(phys);
    if (err)
        goto unlock;
    s->kcb_raw = kraw;
    s->owners_raw = traw;
    s->owners = t;
    s->owners_size = tsize;
    s->kseen = 0;
    s->kcb = k;
    /* Mac OS cannot write them from here on: the kernel drops every store
       into a page with owner kOwnerTables. */
    set_owners(s, (unsigned long)k, (unsigned long)k + kKcbSize, kOwnerTables,
               -1);
    set_owners(s, (unsigned long)t, (unsigned long)t + tsize, kOwnerTables, -1);
    if (nk_protect(s->area, (unsigned long)k, kKcbSize) ||
        nk_protect(s->area, (unsigned long)t, tsize))
        s->c.kernel_errors++;
    s->h68k->kseen = 0;
    s->hnat->kseen = 0;
    s->h68k->kdeny = k->deny_68k;
    s->hnat->kdeny = k->deny_native;
    s->kcb_err = 0;
    return;
unlock:
    if (s->vm) {
        UnlockMemory(k, kKcbSize);
        UnlockMemory(t, tsize);
        if (k->applzone)
            UnlockMemory(applzone_ptr(), 4);
    }
fail:
    if (kraw)
        DisposePtr(kraw);
    if (traw)
        DisposePtr(traw);
    s->kcb_err = err;
}

/* Unregister and free the block, with nothing else protected. */
static void kcb_stop(State *s)
{
    if (!s->kcb)
        return;
    kcb_set(kKcbOpMode, kKcbPass);
    kcb_register(0);
    s->h68k->kdeny = s->h68k->knone;
    s->hnat->kdeny = s->hnat->knone;
    if (nk_unprotect(s->area, (unsigned long)s->kcb, kKcbSize) ||
        nk_unprotect(s->area, (unsigned long)s->owners, s->owners_size))
        s->c.kernel_errors++;
    if (s->vm) {
        UnlockMemory((void *)s->kcb, kKcbSize);
        UnlockMemory((void *)s->owners, s->owners_size);
        if (s->kcb->applzone)
            UnlockMemory(applzone_ptr(), 4);
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
    s->mem_on = s->want_mem = 1;
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
    s->c.tb_per_ms = timebase_per_ms();
    {
        /* The Process Manager's zone, from a handle of its own. */
        OSErr e = noErr;
        Handle t = TempNewHandle(4, &e);
        if (t) {
            s->pm_zone = (unsigned long)HandleZone(t);
            DisposeHandle(t);
        }
    }
    /* Application VSIDs need the kernel's block (patch 5's, as kcb_start
       registers only that version). They come on with protection
       (engine_set_mode), in the segments marked application memory has
       (spaces_cover). */
    s->spaces = s->kcb != 0;
    if (s->spaces) {
        size_hook(s);
        pm_hook(s);
        /* Not found (virtual memory on): a stub for B now, so that the
           system heap's watch leaves it out; pm_hook_mapped installs it
           later. */
        if (!s->pm_site) {
            s->pm_stub = pm_stub_new(0x4E800020);
            hold(s, s->pm_stub, kStubLongs * 4, 1);
        }
    }
    /* Preemption comes on with protection, too. */
    preempt_new(s);
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
    sparse_start(s);
    part_hook(s);
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
    if (s->want_mem != s->mem_on)
        engine_set_mem(s, s->want_mem);
    sys_follow(s);
    size_follow(s);
    sparse_balance(s);
    if (s->pm_stub && !s->pm_site && s->kcb && s->kcb->lazy_pc &&
        s->kcb->lazy_pc != s->pm_tried && s->pm_tries < kPmTries)
        pm_hook_mapped(s);
}

void engine_stop(State *s)
{
    if (!s->running)
        return;
    /* No more chunks. Those made stay with their applications, as
       ordinary temporary memory. */
    pool_remove(s);
    /* Stop denying and protecting, stop the tick, take what the rings
       hold, then open everything before the hooks stop managing it.
       Preemption and application VSIDs go first, as in engine_set_mode;
       the yield stub stays (preempt_new). */
    preempt_set(s, 0);
    spaces_set(s, 0);
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
    part_unhook(s);
    size_unhook(s);
    pm_unhook(s);
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
