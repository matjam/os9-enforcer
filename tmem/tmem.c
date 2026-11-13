/*
 * tmem: find out how temporary memory works on Mac OS 9.2.2, for giving it
 * owners (README, step 4).
 *
 * It patches _OSDispatch (0xA88F) and the Memory Manager traps
 * DisposeHandle, SetHandleSize, HLock and HUnlock with 68k stubs that count
 * their calls, then calls TempNewHandle and those routines from this
 * program's native code. A count of 0 means native callers do not go
 * through the trap table, so a trap patch would not see them. It prints
 * the temporary block's header and master pointer before and after each
 * step, this application's partition block, and what GetPhysical says
 * about the master pointer.
 */

#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <Processes.h>
#include <Gestalt.h>
#include <Traps.h>

#include "kcb.h"

#define HI16(x) ((unsigned short)((unsigned long)(x) >> 16))
#define LO16(x) ((unsigned short)((unsigned long)(x) & 0xFFFF))

/* A 68k stub that counts its calls and goes on to the old routine. */
typedef struct {
    unsigned short code[24];
    unsigned long count, count_tnh, old, last_ret;
    unsigned short last_sel;
    unsigned short trap;
    TrapType type;
} Stub;

enum { kStubs = 5 };

static Stub *g_stubs;

static void make_stub(Stub *st, unsigned short trap, TrapType type, int osd)
{
    unsigned long c = (unsigned long)&st->count;
    unsigned long t = (unsigned long)&st->count_tnh;
    unsigned long o = (unsigned long)&st->old;
    unsigned long s = (unsigned long)&st->last_sel;
    unsigned long r = (unsigned long)&st->last_ret;
    int n = 0;
    st->code[n++] = 0x52B9; /* addq.l #1,count */
    st->code[n++] = HI16(c);
    st->code[n++] = LO16(c);
    st->code[n++] = 0x23D7; /* move.l (sp),last_ret */
    st->code[n++] = HI16(r);
    st->code[n++] = LO16(r);
    if (osd) {
        st->code[n++] = 0x33EF; /* move.w 4(sp),last_sel */
        st->code[n++] = 0x0004;
        st->code[n++] = HI16(s);
        st->code[n++] = LO16(s);
        st->code[n++] = 0x0C6F; /* cmpi.w #$001D,4(sp): TempNewHandle */
        st->code[n++] = 0x001D;
        st->code[n++] = 0x0004;
        st->code[n++] = 0x6606; /* bne.s past the next instruction */
        st->code[n++] = 0x52B9; /* addq.l #1,count_tnh */
        st->code[n++] = HI16(t);
        st->code[n++] = LO16(t);
    }
    st->code[n++] = 0x2F39; /* move.l old,-(sp) */
    st->code[n++] = HI16(o);
    st->code[n++] = LO16(o);
    st->code[n++] = 0x4E75; /* rts */
    st->trap = trap;
    st->type = type;
    st->old = (unsigned long)NGetTrapAddress(trap, type);
}

static void install(void)
{
    g_stubs = (Stub *)NewPtrSysClear(sizeof(Stub) * kStubs);
    if (!g_stubs)
        return;
    make_stub(&g_stubs[0], _OSDispatch, kToolboxTrapType, 1);
    make_stub(&g_stubs[1], _DisposeHandle, kOSTrapType, 0);
    make_stub(&g_stubs[2], _SetHandleSize, kOSTrapType, 0);
    make_stub(&g_stubs[3], _HLock, kOSTrapType, 0);
    make_stub(&g_stubs[4], _HUnlock, kOSTrapType, 0);
    /* BlockMove flushes the code caches for the copied range. */
    Stub tmp[kStubs];
    memcpy(tmp, g_stubs, sizeof tmp);
    BlockMove(tmp, g_stubs, sizeof tmp);
    for (int i = 0; i < kStubs; i++)
        NSetTrapAddress((ProcPtr)g_stubs[i].code, g_stubs[i].trap,
                        g_stubs[i].type);
}

static void uninstall(void)
{
    if (!g_stubs)
        return;
    for (int i = kStubs - 1; i >= 0; i--)
        NSetTrapAddress((ProcPtr)g_stubs[i].old, g_stubs[i].trap,
                        g_stubs[i].type);
}

static const char *const kNames[kStubs] = {
    "_OSDispatch", "DisposeHandle", "SetHandleSize", "HLock", "HUnlock"};

static unsigned long g_seen[kStubs], g_tnh;

/* Take the counts before a step, with nothing printed in between. */
static void mark(void)
{
    if (!g_stubs)
        return;
    for (int i = 0; i < kStubs; i++)
        g_seen[i] = g_stubs[i].count;
    g_tnh = g_stubs[0].count_tnh;
}

/* Print what each stub counted since mark(), and the last caller. */
static void counts(const char *step)
{
    unsigned long n[kStubs], tnh = 0;
    if (g_stubs) {
        for (int i = 0; i < kStubs; i++)
            n[i] = g_stubs[i].count - g_seen[i];
        tnh = g_stubs[0].count_tnh - g_tnh;
    }
    printf("  %-22s", step);
    if (!g_stubs) {
        printf(" (no stubs)\n");
        return;
    }
    for (int i = 0; i < kStubs; i++)
        if (n[i])
            printf(" %s %lu (from %08lX)", kNames[i], n[i],
                   g_stubs[i].last_ret);
    if (tnh)
        printf(" TempNewHandle %lu", tnh);
    printf("\n");
}

static unsigned long rd(unsigned long a)
{
    return *(volatile unsigned long *)a;
}

static void header(const char *what, unsigned long block)
{
    printf("  %s block %08lX: header %08lX %08lX %08lX\n", what, block,
           rd(block - 12), rd(block - 8), rd(block - 4));
}

int main(void)
{
    long vm = 0, lram = 0, ram = 0;
    Gestalt(gestaltVMAttr, &vm);
    Gestalt(gestaltLogicalRAMSize, &lram);
    Gestalt(gestaltPhysicalRAMSize, &ram);
    printf("tmem: temporary memory on this system\n");
    printf("VM attr %08lX, logical RAM %08lX, physical RAM %08lX\n", vm,
           lram, ram);

    unsigned long osd = (unsigned long)NGetTrapAddress(_OSDispatch,
                                                       kToolboxTrapType);
    printf("_OSDispatch at %08lX, first word %04X\n", osd,
           *(unsigned short *)osd);

    ProcessSerialNumber me;
    ProcessInfoRec info;
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info;
    GetCurrentProcess(&me);
    GetProcessInformation(&me, &info);
    unsigned long loc = (unsigned long)info.processLocation;
    printf("partition %08lX+%08lX\n", loc, (unsigned long)info.processSize);
    header("partition", loc);

    install();
    printf("stubs %s\n", g_stubs ? "installed" : "NOT installed");

    /* Part 1: do native calls go through the trap table? Counted with
       nothing printed in between. */
    OSErr err = 0;
    mark();
    Handle h = TempNewHandle(0x10000, &err);
    counts("TempNewHandle 64 KB");
    if (!h) {
        printf("TempNewHandle failed: %d\n", err);
        uninstall();
        return 1;
    }
    unsigned long block = rd((unsigned long)h);
    THz pmz = HandleZone(h);
    printf("  handle %08lX -> %08lX, zone %08lX, state %02X\n",
           (unsigned long)h, block, (unsigned long)pmz, HGetState(h));
    header("temp", block);
    mark();
    HLock(h);
    counts("HLock");
    printf("  state %02X\n", HGetState(h));
    header("temp", block);
    mark();
    DisposeHandle(h);
    counts("DisposeHandle");

    /* Part 2: a pool. A chunk of temporary memory, moved high and locked,
       with a zone on whole pages inside it, and handles in that zone. */
    enum { kChunk = 0x20000, kPg = 4096 };
    mark();
    Handle chunk = TempNewHandle(kChunk + kPg, &err);
    counts("TempNewHandle chunk");
    if (!chunk) {
        printf("chunk failed: %d\n", err);
        uninstall();
        return 1;
    }
    MoveHHi(chunk);
    HLock(chunk);
    unsigned long cb = rd((unsigned long)chunk);
    unsigned long start = (cb + kPg - 1) & ~(unsigned long)(kPg - 1);
    unsigned long limit = (cb + kChunk + kPg) & ~(unsigned long)(kPg - 1);
    unsigned long *sig = (unsigned long *)start;
    THz z = (THz)(start + 16);
    sig[0] = 'EnfT';
    sig[1] = (unsigned long)z;
    THz old = GetZone();
    InitZone(NULL, 32, (Ptr)limit, z);
    THz after_init = GetZone();
    SetZone(z);
    Handle a = NewHandle(1000);
    OSErr ea = MemError();
    Handle b = NewHandle(20000);
    OSErr eb = MemError();
    SetZone(old);
    printf("chunk %08lX (state %02X) -> pages %08lX-%08lX, zone %08lX, "
           "current after InitZone %08lX\n", cb, HGetState(chunk), start,
           limit, (unsigned long)z, (unsigned long)after_init);
    printf("  a %08lX -> %08lX (%d), b %08lX -> %08lX (%d), free %ld\n",
           (unsigned long)a, a ? rd((unsigned long)a) : 0, ea,
           (unsigned long)b, b ? rd((unsigned long)b) : 0, eb, z->zcbFree);
    if (!a || !b) {
        uninstall();
        return 1;
    }
    printf("  HandleZone(a) %08lX, zone + rel(a) %08lX\n",
           (unsigned long)HandleZone(a),
           (unsigned long)z + rd(rd((unsigned long)a) - 4));
    header("a", rd((unsigned long)a));
    printf("  RecoverHandle(*a) %08lX, GetHandleSize(a) %ld\n",
           (unsigned long)RecoverHandle(*a), GetHandleSize(a));
    mark();
    HLock(a);
    counts("HLock(a)");
    printf("  state %02X\n", HGetState(a));
    SetHandleSize(b, 40000);
    printf("  SetHandleSize(b, 40000): %d, b -> %08lX\n", MemError(),
           rd((unsigned long)b));
    SetHandleSize(b, 200000);
    printf("  SetHandleSize(b, 200000): %d, size %ld\n", MemError(),
           GetHandleSize(b));
    mark();
    TempHUnlock(a, &err);
    counts("TempHUnlock(a)");
    printf("  result %d, state %02X\n", err, HGetState(a));
    mark();
    TempHLock(a, &err);
    counts("TempHLock(a)");
    printf("  result %d, state %02X\n", err, HGetState(a));
    mark();
    TempDisposeHandle(a, &err);
    counts("TempDisposeHandle(a)");
    printf("  result %d, a -> %08lX\n", err, rd((unsigned long)a));
    if (err) {
        DisposeHandle(a);
        printf("  DisposeHandle(a): %d\n", MemError());
    }
    mark();
    DisposeHandle(b);
    counts("DisposeHandle(b)");
    printf("  result %d, free %ld\n", MemError(), z->zcbFree);
    DisposeHandle(chunk);
    printf("DisposeHandle(chunk): %d\n", MemError());

    uninstall();
    printf("\npress return to quit\n");
    getchar();
    return 0;
}
