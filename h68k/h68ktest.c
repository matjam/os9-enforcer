/*
 * H68KTest: test the 68k write-fault hook (common/h68k.s).
 *
 * First run: installs the hook, has 68k code store to an unprotected page,
 * then to a protected page inside the hook's range (the hook should make it
 * writable and the store should land), then, after a pause, to a protected
 * page outside the range (the hook should pass it on: this application is
 * killed with error type 1 and the system stays up).
 *
 * Second run: finds the hook still installed, prints its counters and
 * removes it.
 */

#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <MixedMode.h>

#include "nk.h"
#include "h68k.h"

static const unsigned short code68k[] = {
    0x206F, 0x0004,         /* movea.l 4(sp),a0 */
    0x20BC, 0xCAFE, 0xBABE, /* move.l #$CAFEBABE,(a0) */
    0x4E75                  /* rts */
};

static void show(void)
{
    volatile H68kData *d = h68k_data();
    printf("hook: installed %d, entered %lu, handled %lu, passed on %lu, "
           "last kernel status %ld\n",
           h68k_installed(), d->count_all, d->count_handled, d->count_passed,
           (long)d->kstatus);
    for (int i = 0; i < 16; i++) {
        volatile H68kEvent *e = &d->ring[i];
        if (e->addr)
            printf("  [%d] pc %08lX addr %08lX r24 %08lX lr %08lX\n", i, e->pc,
                   e->addr, e->r24, e->lr);
    }
}

/* The middle page of a fresh three-page block: no other block shares it. */
static unsigned long middle_page(Ptr block)
{
    return ((unsigned long)block + kPage - 1) & ~(unsigned long)(kPage - 1);
}

int main(void)
{
    volatile unsigned long *slot = h68k_ptr(kH68kSlot);
    printf("H68KTest: ECB handler %08lX\n", *slot);

    if (h68k_installed()) {
        printf("The hook is installed (left by an earlier run):\n");
        show();
        printf("\n(Press Return to remove it)\n");
        getchar();
        h68k_uninstall();
        printf("removed; ECB handler %08lX\n", *slot);
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

    err = h68k_install(area);
    printf("h68k_install: %ld; ECB handler now %08lX\n", err, *slot);
    if (err) {
        getchar();
        return 1;
    }
    show();
    printf("\nThe hook is passing everything on. (Press Return to test)\n");
    getchar();
    show();

    Ptr c = NewPtr(sizeof code68k);
    BlockMoveData(code68k, c, sizeof code68k);
    ProcInfoType pi = kCStackBased | STACK_ROUTINE_PARAMETER(1, kFourByteCode);
    UniversalProcPtr upp = NewRoutineDescriptor((ProcPtr)c, pi, kM68kISA);
    volatile unsigned long *target = (volatile unsigned long *)(page + 128);

    printf("\nA: 68k store, page unprotected\n");
    CallUniversalProc(upp, pi, (long)target);
    printf("  word %08lX (CAFEBABE expected)\n", *target);
    *target = 0;

    printf("\nB: 68k store, page protected and in the hook's range\n");
    h68k_set_range(page, page + kPage);
    err = nk_protect(area, page, kPage);
    unsigned long bits = 0;
    nk_get_access(area, page, &bits);
    printf("  protect err %ld, access %02lX\n", err, bits);
    CallUniversalProc(upp, pi, (long)target);
    nk_get_access(area, page, &bits);
    printf("  word %08lX (CAFEBABE expected), access now %02lX\n", *target,
           bits);
    show();
    h68k_set_range(0, 0);
    nk_unprotect(area, page, kPage);

    printf("\n(Press Return for C: 68k store, protected page outside the\n"
           "range. Expected: this app is killed with error type 1 and the\n"
           "system stays up. Then run H68KTest again to see the counts.)\n");
    getchar();
    /* The system heap page is leaked on purpose: it stays protected after
       this app dies, and nothing else uses it. */
    nk_protect(area, syspage, kPage);
    CallUniversalProc(upp, pi, (long)(syspage + 128));

    printf("not killed: word %08lX\n", *(volatile unsigned long *)(syspage + 128));
    show();
    getchar();
    return 0;
}
