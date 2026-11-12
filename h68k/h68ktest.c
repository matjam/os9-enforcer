/*
 * H68KTest: test the write hooks (common/h68k.s for 68k code, common/hnat.s
 * for native code).
 *
 * First run: installs both hooks, then
 *   A  68k store to an unprotected page: lands;
 *   B  68k store to a protected page in the 68k hook's range: the hook opens
 *      the page, the store lands, and the ring records it;
 *   C  native store to a protected page in the native hook's range: the
 *      same, through the native hook;
 *   D  as C, with the page marked as the current application's: counted,
 *      not recorded;
 *   E  native stores to this application's own stack page while it is
 *      protected: the native hook opens it (the Exception Manager would
 *      fault again saving registers on that stack);
 *   F  after a pause, 68k store to a protected page outside the range: the
 *      68k hook passes it on, this application is killed with error type 1
 *      and the system stays up.
 *
 * Second run: finds the hooks still installed, prints their counters and
 * removes them.
 */

#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <MixedMode.h>

#include "nk.h"
#include "h68k.h"
#include "hnat.h"

static const unsigned short code68k[] = {
    0x206F, 0x0004,         /* movea.l 4(sp),a0 */
    0x20BC, 0xCAFE, 0xBABE, /* move.l #$CAFEBABE,(a0) */
    0x4E75                  /* rts */
};

static void show(const char *name, volatile HookData *d)
{
    if (!d) {
        printf("%s: not installed\n", name);
        return;
    }
    printf("%s: entered %lu, recorded %lu, own %lu, passed on %lu, kernel "
           "status %ld\n", name, d->count_all, d->count_handled,
           d->count_self, d->count_passed, (long)d->kstatus);
    for (int i = 0; i < kHookRing && d->ring; i++) {
        volatile HookEvent *e = &d->ring[(d->ring_idx + i) % kHookRing];
        if (e->seq)
            printf("  #%lu pc %08lX addr %08lX r24/r3 %08lX lr %08lX zone "
                   "%08lX r4 %08lX sp %08lX\n", e->seq, e->pc, e->addr,
                   e->r24, e->lr, e->zone, e->r4, e->sp);
    }
    if (d->count_passed)
        printf("  last passed on: pc %08lX addr %08lX r24/r3 %08lX zone "
               "%08lX\n", d->passed.pc, d->passed.addr, d->passed.r24,
               d->passed.zone);
}

static volatile HookData *h68k_or_null(void)
{
    return h68k_installed() ? h68k_data() : 0;
}

/* The middle page of a fresh three-page block: no other block shares it. */
static unsigned long middle_page(Ptr block)
{
    return ((unsigned long)block + kPage - 1) & ~(unsigned long)(kPage - 1);
}

/* Writes a few words to its own frame, so the stack below the caller's
   frame is written. */
static __attribute__((noinline)) unsigned long touch_stack(int n)
{
    volatile unsigned long buf[64];
    for (int i = 0; i < 64; i++)
        buf[i] = i * n;
    return buf[n & 63];
}

int main(void)
{
    volatile unsigned long *slot68 = hook_ptr(kH68kSlot);
    volatile unsigned long *slotnat = hook_ptr(kHnatSlot);
    printf("H68KTest: ECB handler %08lX, NCB handler %08lX\n", *slot68,
           *slotnat);

    if (h68k_installed() || hnat_installed()) {
        printf("Hooks left by an earlier run:\n");
        show("68k hook", h68k_or_null());
        show("native hook", hnat_data());
        printf("\n(Press Return to remove them)\n");
        getchar();
        h68k_uninstall();
        hnat_uninstall();
        printf("removed; ECB handler %08lX, NCB handler %08lX\n", *slot68,
               *slotnat);
        printf("\n(Press Return)\n");
        getchar();
        return 0;
    }

    Ptr block = NewPtrClear(3 * kPage);
    Ptr sysblock = NewPtrSysClear(3 * kPage);
    if (!block || !sysblock) {
        printf("out of memory\n");
        getchar();
        return 1;
    }
    unsigned long page = middle_page(block);
    unsigned long syspage = middle_page(sysblock);
    unsigned long area;
    long err = nk_area_from_address(page, &area);
    printf("page %08lX, system heap page %08lX, area %08lX err %ld\n", page,
           syspage, area, err);
    if (err) {
        getchar();
        return 1;
    }

    long e68 = h68k_install(area), enat = hnat_install(area);
    printf("h68k_install %ld, hnat_install %ld; ECB handler %08lX, NCB "
           "handler %08lX\n", e68, enat, *slot68, *slotnat);
    if (e68 || enat) {
        h68k_uninstall();
        hnat_uninstall();
        getchar();
        return 1;
    }
    volatile HookData *d68 = h68k_data(), *dnat = hnat_data();
    printf("\nBoth hooks are passing everything on. (Press Return to test)\n");
    getchar();
    show("68k hook", d68);
    show("native hook", dnat);

    Ptr c = NewPtr(sizeof code68k);
    BlockMoveData(code68k, c, sizeof code68k);
    ProcInfoType pi = kCStackBased | STACK_ROUTINE_PARAMETER(1, kFourByteCode);
    UniversalProcPtr upp = NewRoutineDescriptor((ProcPtr)c, pi, kM68kISA);
    volatile unsigned long *target = (volatile unsigned long *)(page + 128);
    unsigned long bits = 0;

    printf("\nA: 68k store, page unprotected\n");
    CallUniversalProc(upp, pi, (long)target);
    printf("  word %08lX (CAFEBABE expected)\n", *target);
    *target = 0;

    printf("B: 68k store, page protected, in the 68k hook's range "
           "(68k code at %08lX)\n", (unsigned long)c);
    hook_set_range(d68, page, page + kPage);
    nk_protect(area, page, kPage);
    CallUniversalProc(upp, pi, (long)target);
    nk_get_access(area, page, &bits);
    printf("  word %08lX (CAFEBABE expected), access now %02lX\n", *target,
           bits);
    hook_set_range(d68, 0, 0);

    printf("C: native store, page protected, in the native hook's range\n");
    volatile unsigned long *ntarget = (volatile unsigned long *)(page + 256);
    hook_set_range(dnat, page, page + kPage);
    nk_protect(area, page, kPage);
    *ntarget = 0xFEEDFACE;
    nk_get_access(area, page, &bits);
    printf("  word %08lX (FEEDFACE expected), access now %02lX\n", *ntarget,
           bits);

    printf("D: as C, page marked as the current application's\n");
    hook_set_current(dnat, page, page + kPage);
    nk_protect(area, page, kPage);
    *ntarget = 0xD00DFEED;
    printf("  word %08lX (D00DFEED expected)\n", *ntarget);
    hook_set_current(dnat, 0, 0);
    hook_set_range(dnat, 0, 0);

    printf("E: native stores to this application's own protected stack\n");
    unsigned long sp = (unsigned long)__builtin_frame_address(0);
    unsigned long stack_lo = (sp - 2 * kPage) & ~(unsigned long)(kPage - 1);
    unsigned long stack_hi = (sp & ~(unsigned long)(kPage - 1)) + kPage;
    hook_set_range(dnat, stack_lo, stack_hi);
    nk_protect(area, stack_lo, stack_hi - stack_lo);
    unsigned long r = touch_stack(7);
    hook_set_range(dnat, 0, 0);
    nk_unprotect(area, stack_lo, stack_hi - stack_lo);
    printf("  survived; sp %08lX, pages %08lX-%08lX, result %lu (49 "
           "expected)\n", sp, stack_lo, stack_hi, r);
    show("68k hook", d68);
    show("native hook", dnat);

    printf("\n(Press Return for F: 68k store, protected page outside the\n"
           "range. Expected: this app is killed with error type 1 and the\n"
           "system stays up. Then run H68KTest again to see the counts.)\n");
    getchar();
    /* The system heap page is leaked on purpose: it stays protected after
       this app dies, and nothing else uses it. */
    nk_protect(area, syspage, kPage);
    CallUniversalProc(upp, pi, (long)(syspage + 128));

    printf("not killed: word %08lX\n", *(volatile unsigned long *)(syspage + 128));
    getchar();
    return 0;
}
