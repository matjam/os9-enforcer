/*
 * probe: test whether a Mac OS 9 application can write-protect one page of
 * its own memory with the NanoKernel's area calls, what exception a store to
 * that page raises, and whether execution can continue after it.
 *
 * Kernel call numbers and register use come from the v2 NanoKernel source
 * (refs/powermac-rom/NanoKernel/NKAddressSpaces.s). A positive r0 with `sc`
 * goes straight to the kernel's MP call dispatcher; arguments are in r3-r7,
 * the status comes back in r3 and results in r4-r10.
 */

#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <MacMemory.h>
#include <Gestalt.h>
#include <MixedMode.h>

enum {
    kcSetAreaAccess = 76,      /* r3 area, r4 values, r5 mask, r6 start, r7 len */
    kcAreaInfo = 78,           /* r3 area, r4 1, r5 selector -> r6..r9 */
    kcGetAreaFromAddress = 80, /* r3 space (0 = current), r4 addr -> r5 area */
    kcGetAreaAccess = 123      /* r3 area, r4 addr -> r5 access bits */
};

/* Access bits as the kernel converts them to a PTE's PP field. */
enum {
    kAccessRead = 0x01,
    kAccessWrite = 0x02,
    kAccessSupRWUserRO = 0x08
};

enum { kPage = 4096 };

typedef struct {
    unsigned long r3, r4, r5, r6, r7, r8, r9, r10;
} KRegs;

static long kcall(long sel, KRegs *k)
{
    register long r0 __asm__("r0") = sel;
    register unsigned long r3 __asm__("r3") = k->r3;
    register unsigned long r4 __asm__("r4") = k->r4;
    register unsigned long r5 __asm__("r5") = k->r5;
    register unsigned long r6 __asm__("r6") = k->r6;
    register unsigned long r7 __asm__("r7") = k->r7;
    register unsigned long r8 __asm__("r8") = k->r8;
    register unsigned long r9 __asm__("r9") = k->r9;
    register unsigned long r10 __asm__("r10") = k->r10;

    __asm__ volatile("sc"
                     : "+r"(r0), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6),
                       "+r"(r7), "+r"(r8), "+r"(r9), "+r"(r10)
                     :
                     : "memory", "r11", "r12", "ctr", "lr", "xer", "cr0",
                       "cr1", "cr5", "cr6", "cr7");

    k->r3 = r3; k->r4 = r4; k->r5 = r5; k->r6 = r6;
    k->r7 = r7; k->r8 = r8; k->r9 = r9; k->r10 = r10;
    return (long)r3;
}

static long area_from_address(unsigned long addr, unsigned long *area)
{
    KRegs k = {0};
    k.r4 = addr;
    long err = kcall(kcGetAreaFromAddress, &k);
    *area = k.r5;
    return err;
}

static long get_access(unsigned long area, unsigned long addr,
                       unsigned long *bits)
{
    KRegs k = {0};
    k.r3 = area;
    k.r4 = addr;
    long err = kcall(kcGetAreaAccess, &k);
    *bits = k.r5;
    return err;
}

/* new = (old & ~mask) | (values & mask), for each page in the range */
static long set_access(unsigned long area, unsigned long values,
                       unsigned long mask, unsigned long start,
                       unsigned long len)
{
    KRegs k = {0};
    k.r3 = area;
    k.r4 = values;
    k.r5 = mask;
    k.r6 = start;
    k.r7 = len;
    return kcall(kcSetAreaAccess, &k);
}

static long area_info(unsigned long area, unsigned long sel,
                      unsigned long out[4])
{
    KRegs k = {0};
    k.r3 = area;
    k.r4 = 1;
    k.r5 = sel;
    long err = kcall(kcAreaInfo, &k);
    out[0] = k.r6; out[1] = k.r7; out[2] = k.r8; out[3] = k.r9;
    return err;
}

/* Exception Manager (MachineExceptions.h), declared here because the
   Multiversal Interfaces do not include it. */

typedef struct {
    unsigned long hi, lo;
} Wide;

typedef struct {
    Wide CTR, LR, PC;
    unsigned long CR, XER, MSR, MQ, ExceptionID, DAR;
} MachineInfo;

typedef struct {
    unsigned long theArea;
    unsigned long theAddress;
    long theError;
    unsigned long theReference;
} MemoryExceptionInfo;

typedef struct {
    unsigned long theKind;
    MachineInfo *machineState;
    void *registerImage;
    void *FPUImage;
    MemoryExceptionInfo *memoryInfo;
    void *vectorImage;
} ExceptionInfo;

typedef long (*ExceptionHandler)(ExceptionInfo *);
extern ExceptionHandler InstallExceptionHandler(ExceptionHandler handler);

static const char *kind_name(unsigned long k)
{
    static const char *names[] = {
        "Unknown", "IllegalInstruction", "Trap", "Access",
        "UnmappedMemory", "ExcludedMemory", "ReadOnlyMemory",
        "UnresolvablePageFault", "PrivilegeViolation", "Trace",
        "InstructionBreakpoint", "DataBreakpoint", "Integer",
        "FloatingPoint", "StackOverflow", "TaskTermination",
        "TaskCreation", "DataAlignment"};
    return k < sizeof names / sizeof names[0] ? names[k] : "?";
}

enum { kRetry, kSkip };

static struct {
    int fired;
    unsigned long kind, pc, lr, dar, exid, msr;
    int have_mem;
    unsigned long m_area, m_addr, m_ref;
    long m_err;
    long fix_err;
    int skipped;
    unsigned long info_ptr, mach_ptr, mem_ptr;
    int nframes;
    unsigned long frame[12], saved_lr[12];
    int order;
} rec;

/* Shared with the handlers, which run in the middle of other code, so the
   compiler must not keep these in registers or drop stores to them. */
static volatile int g_seq;  /* orders calls between the two handlers */
static volatile int g_walk; /* walk the stack in the per-process handler */

/* Breadcrumbs in the system heap, readable over QMP after a crash:
   [0] 'PRB!', [1] system handler calls, [2] process handler calls,
   [3] last kind, [4] last PC, [5] last fault address. */
static volatile unsigned long *g_crumbs;

static void crumb(int which, unsigned long kind, unsigned long pc,
                  unsigned long addr)
{
    if (!g_crumbs)
        return;
    g_crumbs[which]++;
    g_crumbs[3] = kind;
    g_crumbs[4] = pc;
    g_crumbs[5] = addr;
}

/* Walk the AIX-style back chain from the handler's own frame: 0(r1) is the
   caller's frame, 8(r1) the LR saved there. Frames must go up in small
   steps, so a broken chain stops the walk instead of faulting. */
static void walk_stack(void)
{
    unsigned long *fp = __builtin_frame_address(0);
    int n = 0;
    while (n < 12 && fp && !((unsigned long)fp & 7)) {
        rec.frame[n] = (unsigned long)fp;
        rec.saved_lr[n] = fp[2];
        n++;
        unsigned long *next = (unsigned long *)fp[0];
        if (next <= fp || (unsigned long)next - (unsigned long)fp > 0x10000)
            break;
        fp = next;
    }
    rec.nframes = n;
}

static volatile int g_armed;
static volatile int g_mode;
static unsigned long g_area, g_page;
static unsigned long g_store_pc; /* PC of the faulting store, from the retry test */
static ExceptionHandler g_prev;

static long handler(ExceptionInfo *e)
{
    if (!g_armed)
        return g_prev ? g_prev(e) : -1;
    g_armed = 0;

    MachineInfo *m = e->machineState;
    crumb(2, e->theKind, m->PC.lo,
          e->memoryInfo ? e->memoryInfo->theAddress : 0);
    rec.fired++;
    rec.order = ++g_seq;
    rec.info_ptr = (unsigned long)e;
    rec.mach_ptr = (unsigned long)m;
    rec.mem_ptr = (unsigned long)e->memoryInfo;
    if (g_walk)
        walk_stack();
    rec.kind = e->theKind;
    rec.pc = m->PC.lo;
    rec.lr = m->LR.lo;
    rec.dar = m->DAR;
    rec.exid = m->ExceptionID;
    rec.msr = m->MSR;
    if (e->memoryInfo) {
        rec.have_mem = 1;
        rec.m_area = e->memoryInfo->theArea;
        rec.m_addr = e->memoryInfo->theAddress;
        rec.m_err = e->memoryInfo->theError;
        rec.m_ref = e->memoryInfo->theReference;
    }

    /* Skip the store only when the PC is the one seen in the retry test,
       so a wrong struct layout cannot send execution somewhere random. */
    if (g_mode == kSkip && g_store_pc && m->PC.lo == g_store_pc) {
        m->PC.lo += 4;
        rec.skipped = 1;
        return 0;
    }
    rec.fix_err = set_access(g_area, kAccessWrite, kAccessWrite, g_page, kPage);
    return 0;
}

/* The Exception Manager keeps one system-wide handler that it calls before
   the current process's handler (ROM 0xFFCF0CD0 on 9.2.2). Its installer
   has no public name; its TVector sits right after InstallExceptionHandler's
   in the Exception Manager's data section, so it is found there and its code
   is checked before use. */
typedef ExceptionHandler (*InstallFn)(ExceptionHandler);

static InstallFn find_sys_installer(void)
{
    static const unsigned long expect[] = {
        0x39820098, /* addi r12,r2,0x98 */
        0x800C0000, /* lwz  r0,0(r12) */
        0x906C0000, /* stw  r3,0(r12) */
        0x7C030378, /* mr   r3,r0 */
        0x4E800020  /* blr */
    };
    unsigned long *ieh = (unsigned long *)InstallExceptionHandler;
    unsigned long *tv = ieh + 2;
    if (tv[1] != ieh[1])
        return 0;
    unsigned long *code = (unsigned long *)tv[0];
    for (int i = 0; i < 5; i++)
        if (code[i] != expect[i])
            return 0;
    return (InstallFn)tv;
}

enum { kSysDecline, kSysRetry };

static struct {
    int fired, order;
    unsigned long kind, pc, addr;
    long fix_err;
} srec;

static volatile int g_sys_mode;
static ExceptionHandler g_sys_prev;

static long sys_handler(ExceptionInfo *e)
{
    MemoryExceptionInfo *mi = e->memoryInfo;
    crumb(1, e->theKind, e->machineState->PC.lo, mi ? mi->theAddress : 0);
    if (!mi || mi->theAddress < g_page || mi->theAddress >= g_page + kPage)
        return g_sys_prev ? g_sys_prev(e) : -1;
    srec.fired++;
    srec.order = ++g_seq;
    srec.kind = e->theKind;
    srec.pc = e->machineState->PC.lo;
    srec.addr = mi->theAddress;
    if (g_sys_mode == kSysDecline)
        return -1;
    srec.fix_err = set_access(g_area, kAccessWrite, kAccessWrite, g_page, kPage);
    return 0;
}

static void print_srec(void)
{
    printf("  system handler calls: %d", srec.fired);
    if (srec.fired)
        printf(" (order %d) kind %lu PC %08lX addr %08lX fix %ld", srec.order,
               srec.kind, srec.pc, srec.addr, srec.fix_err);
    printf("\n  process handler calls: %d", rec.fired);
    if (rec.fired)
        printf(" (order %d) kind %lu PC %08lX", rec.order, rec.kind, rec.pc);
    printf("\n");
}

/* The barrier tells the compiler that this call can change any memory, as
   the exception handlers do while it runs. */
static __attribute__((noinline)) void do_store(volatile unsigned long *p,
                                               unsigned long v)
{
    *p = v;
    __asm__ volatile("" ::: "memory");
}

static void print_rec(void)
{
    printf("  handler calls: %d\n", rec.fired);
    if (!rec.fired)
        return;
    printf("  kind %lu (%s), ExceptionID %lu\n", rec.kind,
           kind_name(rec.kind), rec.exid);
    printf("  PC %08lX LR %08lX DAR %08lX MSR %08lX\n", rec.pc, rec.lr,
           rec.dar, rec.msr);
    if (rec.have_mem)
        printf("  memoryInfo: area %08lX addr %08lX err %ld ref %lu\n",
               rec.m_area, rec.m_addr, rec.m_err, rec.m_ref);
    printf("  info %08lX machine %08lX memory %08lX\n", rec.info_ptr,
           rec.mach_ptr, rec.mem_ptr);
    for (int i = 0; i < rec.nframes; i += 3) {
        printf("  ");
        for (int j = i; j < i + 3 && j < rec.nframes; j++)
            printf(" %08lX:%08lX", rec.frame[j], rec.saved_lr[j]);
        printf("\n");
    }
    if (rec.skipped)
        printf("  handler skipped the store\n");
    else
        printf("  handler restored write access: err %ld\n", rec.fix_err);
}

static void show_access(const char *when)
{
    unsigned long bits = 0;
    long err = get_access(g_area, g_page, &bits);
    printf("%s: access err %ld bits %02lX\n", when, err, bits);
}

int main(void)
{
    long vm = 0;
    OSErr gerr = Gestalt('vm  ', &vm);
    printf("probe: NanoKernel page protection\n");
    printf("Gestalt vm: err %d value %08lX\n", gerr, (unsigned long)vm);
    printf("handler fn %08lX, word0 %08lX (TVector if word0 is code)\n",
           (unsigned long)handler, *(unsigned long *)handler);
    printf("InstallExceptionHandler TVector %08lX: code %08lX toc %08lX\n",
           (unsigned long)InstallExceptionHandler,
           ((unsigned long *)InstallExceptionHandler)[0],
           ((unsigned long *)InstallExceptionHandler)[1]);

    Ptr block = NewPtr(3 * kPage);
    if (!block) {
        printf("NewPtr failed\n");
        getchar();
        return 1;
    }
    memset(block, 0x11, 3 * kPage);
    g_page = ((unsigned long)block + kPage - 1) & ~(unsigned long)(kPage - 1);
    volatile unsigned long *word = (volatile unsigned long *)g_page;
    printf("block %08lX, page %08lX\n", (unsigned long)block, g_page);

    long err = area_from_address(g_page, &g_area);
    printf("GetAreaFromAddress: err %ld area %08lX\n", err, g_area);
    if (err)
        goto done;

    unsigned long info[4];
    for (unsigned long sel = 0; sel <= 0x20; sel += 0x10) {
        err = area_info(g_area, sel, info);
        printf("AreaInfo %02lX: err %ld  %08lX %08lX %08lX %08lX\n", sel,
               err, info[0], info[1], info[2], info[3]);
    }
    /* Selector 0x10 gives the area flags in the third word. With bit
       0x8000 set, SetAreaAccess changes the whole area at once. */
    err = area_info(g_area, 0x10, info);
    if (err == 0 && (info[2] & 0x8000)) {
        printf("area is not paged (flags %08lX); not touching it\n", info[2]);
        goto done;
    }

    show_access("before");
    printf("\n(Press Return to run the tests)\n");
    getchar();

    g_prev = InstallExceptionHandler(handler);
    printf("InstallExceptionHandler: previous %08lX\n", (unsigned long)g_prev);

    /* Test 1: protect, read, store; the handler restores write access and
       the store runs again. */
    *word = 0xAAAAAAAA;
    err = set_access(g_area, 0, kAccessWrite, g_page, kPage);
    printf("\nTest 1 (retry): SetAreaAccess clear write: err %ld\n", err);
    show_access("protected");
    printf("  read while protected: %08lX\n", *word);
    memset(&rec, 0, sizeof rec);
    g_mode = kRetry;
    g_walk = 1;
    g_armed = 1;
    do_store(word, 0x12345678);
    g_armed = 0;
    g_walk = 0;
    print_rec();
    printf("  word now %08lX (12345678 if the store ran)\n", *word);
    g_store_pc = rec.fired ? rec.pc : 0;
    show_access("after");

    /* Test 2: protect, store; the handler steps over the store. */
    err = set_access(g_area, 0, kAccessWrite, g_page, kPage);
    printf("\nTest 2 (skip): SetAreaAccess clear write: err %ld\n", err);
    memset(&rec, 0, sizeof rec);
    g_mode = kSkip;
    g_armed = 1;
    do_store(word, 0xDEADBEEF);
    g_armed = 0;
    print_rec();
    printf("  word now %08lX (12345678 if the store was skipped)\n", *word);

    printf("\n(Press Return for test 3)\n");
    getchar();

    /* Test 3: a Toolbox routine writes the protected page. */
    set_access(g_area, kAccessWrite, kAccessWrite, g_page, kPage);
    err = set_access(g_area, 0, kAccessWrite, g_page, kPage);
    printf("\nTest 3 (BlockMoveData into the page): err %ld\n", err);
    static const char src[16] = "written by ROM";
    memset(&rec, 0, sizeof rec);
    g_mode = kRetry;
    g_armed = 1;
    BlockMoveData(src, (Ptr)(g_page + 64), sizeof src);
    g_armed = 0;
    print_rec();
    printf("  page+64 now \"%.15s\"\n", (char *)(g_page + 64));

    printf("\n(Press Return for tests 4 and 5)\n");
    getchar();

    /* Test 4: the system-wide handler. In 4a it declines and the process
       handler should run after it; in 4b it handles the fault itself. */
    InstallFn sys_install = find_sys_installer();
    printf("\nTest 4: system handler installer %08lX\n",
           (unsigned long)sys_install);
    if (sys_install) {
        g_sys_prev = sys_install(sys_handler);
        printf("  previous system handler %08lX\n", (unsigned long)g_sys_prev);

        set_access(g_area, kAccessWrite, kAccessWrite, g_page, kPage);
        set_access(g_area, 0, kAccessWrite, g_page, kPage);
        printf("4a (system declines):\n");
        memset(&rec, 0, sizeof rec);
        memset(&srec, 0, sizeof srec);
        g_seq = 0;
        g_sys_mode = kSysDecline;
        g_mode = kRetry;
        g_armed = 1;
        do_store(word, 0x4A4A4A4A);
        g_armed = 0;
        print_srec();
        printf("  word now %08lX\n", *word);

        set_access(g_area, 0, kAccessWrite, g_page, kPage);
        printf("4b (system handles):\n");
        memset(&rec, 0, sizeof rec);
        memset(&srec, 0, sizeof srec);
        g_seq = 0;
        g_sys_mode = kSysRetry;
        g_armed = 1;
        do_store(word, 0x4B4B4B4B);
        g_armed = 0;
        print_srec();
        printf("  word now %08lX\n", *word);

        printf("\n(Press Return for test 5)\n");
        getchar();

        /* Test 5: 68k code stores to the protected page. The emulator runs
           it, so the fault may take a different path from native code. */
        g_crumbs = (volatile unsigned long *)NewPtrSysClear(64);
        if (g_crumbs)
            g_crumbs[0] = 'PRB!';
        printf("breadcrumbs at %08lX\n", (unsigned long)g_crumbs);
        static const unsigned short code68k[] = {
            0x206F, 0x0004,         /* movea.l 4(sp),a0 */
            0x20BC, 0xCAFE, 0xBABE, /* move.l #$CAFEBABE,(a0) */
            0x4E75                  /* rts */
        };
        Ptr c = NewPtr(sizeof code68k);
        BlockMoveData(code68k, c, sizeof code68k);
        ProcInfoType pi =
            kCStackBased | STACK_ROUTINE_PARAMETER(1, kFourByteCode);
        UniversalProcPtr upp = NewRoutineDescriptor((ProcPtr)c, pi, kM68kISA);

        printf("\nTest 5 (68k store): running 68k code unprotected first\n");
        set_access(g_area, kAccessWrite, kAccessWrite, g_page, kPage);
        CallUniversalProc(upp, pi, (long)(g_page + 128));
        printf("  page+128 now %08lX\n", *(unsigned long *)(g_page + 128));
        *(unsigned long *)(g_page + 128) = 0;
        printf("\n(Press Return for the protected 68k store)\n");
        getchar();

        set_access(g_area, 0, kAccessWrite, g_page, kPage);
        printf("  now protected; if the system hangs or crashes here, a 68k\n"
               "  write fault is not delivered to these handlers\n");
        memset(&rec, 0, sizeof rec);
        memset(&srec, 0, sizeof srec);
        g_seq = 0;
        g_sys_mode = kSysRetry;
        g_armed = 1;
        CallUniversalProc(upp, pi, (long)(g_page + 128));
        g_armed = 0;
        print_srec();
        printf("  page+128 now %08lX\n", *(unsigned long *)(g_page + 128));

        sys_install(g_sys_prev);
        DisposeRoutineDescriptor(upp);
        DisposePtr(c);
    }

    err = set_access(g_area, kAccessWrite, kAccessWrite, g_page, kPage);
    printf("\nrestore write: err %ld\n", err);
    show_access("final");
    InstallExceptionHandler(g_prev);

done:
    DisposePtr(block);
    printf("\n(Press Return)\n");
    getchar();
    return 0;
}
