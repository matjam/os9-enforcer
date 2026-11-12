/*
 * Scribble: write into the Finder's memory, to test Guard (xlog/).
 *
 * Each test stores into a word in the middle of the Finder's partition the
 * value that is already there, so nothing changes if the store is allowed.
 *   n  a store from this application's native code: Guard denies it;
 *   6  a store from 68k code in this application's heap: Guard denies it;
 *   b  BlockMoveData, system code, called by this application: allowed
 *      (Guard does not look at who called the code doing the store);
 *   o  a store into this application's own heap: allowed.
 * A denied store stops this application; run it again for the next test.
 */

#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <MixedMode.h>
#include <Processes.h>

/* store(addr, value), 68k, C calling convention. */
static const unsigned short code68k[] = {
    0x206F, 0x0004, /* movea.l 4(sp),a0 */
    0x20AF, 0x0008, /* move.l 8(sp),(a0) */
    0x4E75          /* rts */
};

static int find_finder(unsigned long *loc, unsigned long *size)
{
    ProcessSerialNumber psn = {0, kNoProcess};
    ProcessInfoRec info;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof info);
        info.processInfoLength = sizeof info;
        if (GetProcessInformation(&psn, &info) == noErr &&
            info.processSignature == 'MACS') {
            *loc = (unsigned long)info.processLocation;
            *size = info.processSize;
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    unsigned long loc, size;
    printf("Scribble: write into the Finder's memory\n");
    if (!find_finder(&loc, &size)) {
        printf("Finder not found\n");
        getchar();
        return 1;
    }
    volatile unsigned long *target =
        (volatile unsigned long *)(((loc + size / 2) & ~0xFFFUL) + 0x100);
    Ptr own = NewPtrClear(16);
    Ptr c = NewPtr(sizeof code68k);
    if (!own || !c) {
        printf("out of memory\n");
        getchar();
        return 1;
    }
    BlockMoveData(code68k, c, sizeof code68k);
    ProcInfoType pi = kCStackBased |
                      STACK_ROUTINE_PARAMETER(1, kFourByteCode) |
                      STACK_ROUTINE_PARAMETER(2, kFourByteCode);
    UniversalProcPtr upp = NewRoutineDescriptor((ProcPtr)c, pi, kM68kISA);

    printf("Finder partition %08lX+%08lX, target %08lX\n", loc, size,
           (unsigned long)target);
    /* A PowerPC function pointer points at a transition vector, whose
       first word is the code address. */
    printf("main's code %08lX, 68k code %08lX\n",
           *(unsigned long *)(void *)&main, (unsigned long)c);
    for (;;) {
        printf("\nn native store, 6 68k store, b BlockMoveData, o own "
               "heap, q quit; then Return\n");
        int ch = getchar();
        while (ch != '\n' && getchar() != '\n')
            ;
        unsigned long v = *target;
        switch (ch) {
        case 'n':
            printf("native store of %08lX to %08lX\n", v, (unsigned long)target);
            *target = v;
            break;
        case '6':
            printf("68k store of %08lX to %08lX\n", v, (unsigned long)target);
            CallUniversalProc(upp, pi, (long)target, (long)v);
            break;
        case 'b':
            printf("BlockMoveData of %08lX to %08lX\n", v, (unsigned long)target);
            BlockMoveData(&v, (Ptr)target, sizeof v);
            break;
        case 'o':
            printf("store into own heap at %08lX\n", (unsigned long)own);
            *(volatile unsigned long *)own = v;
            break;
        case 'q':
            return 0;
        default:
            continue;
        }
        printf("the store landed (not denied)\n");
    }
}
