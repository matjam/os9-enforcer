/*
 * nk.h: NanoKernel area calls and Exception Manager hooks for Mac OS 9.2.2
 * PowerPC code built with Retro68.
 *
 * Kernel call numbers and register use come from the v2 NanoKernel source
 * (refs/powermac-rom/NanoKernel/NKAddressSpaces.s). A positive r0 with `sc`
 * goes straight to the kernel's MP call dispatcher; arguments are in r3-r7,
 * the status comes back in r3 and results in r4-r10. None of these calls
 * check privilege: any application can use them on any page of the blue
 * task's memory.
 */

#ifndef NK_H
#define NK_H

enum {
    kcCreateArea = 72,         /* r3 space (0 = current), r4 access (6: read
                                  and write), r5 length, r6 separation, r7
                                  flags and alignment (0x600C: at the base,
                                  4 KB pages), r8 base -> r8 base, r9 area.
                                  The pages have no memory until mapped. */
    kcDeleteArea = 74,         /* r3 area, with nothing mapped */
    kcPutFreePage = 84,        /* r3 a physical page, for the kernel's own
                                  free list (area page tables come from it) */
    kcGetFreePageCount = 100,  /* -> r4 */
    kcSetAreaAccess = 76,      /* r3 area, r4 values, r5 mask, r6 start, r7 len */
    kcAreaInfo = 78,           /* r3 area, r4 1, r5 selector -> r6..r9 */
    kcGetAreaFromAddress = 80, /* r3 space (0 = current), r4 addr -> r5 area */
    kcGetAreaAccess = 123      /* r3 area, r4 addr -> r5 access bits */
};

/* Access bits, as the kernel turns them into a PTE's PP field. */
enum {
    kAccessRead = 0x01,
    kAccessWrite = 0x02,
    kAccessSupRWUserRO = 0x08
};

enum { kPage = 4096 };

typedef struct {
    unsigned long r3, r4, r5, r6, r7, r8, r9, r10;
} KRegs;

static inline long nk_call(long sel, KRegs *k)
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

static inline long nk_area_from_address(unsigned long addr, unsigned long *area)
{
    KRegs k = {0};
    k.r4 = addr;
    long err = nk_call(kcGetAreaFromAddress, &k);
    *area = k.r5;
    return err;
}

static inline long nk_get_access(unsigned long area, unsigned long addr,
                                 unsigned long *bits)
{
    KRegs k = {0};
    k.r3 = area;
    k.r4 = addr;
    long err = nk_call(kcGetAreaAccess, &k);
    *bits = k.r5;
    return err;
}

/* For each page in the range: new = (old & ~mask) | (values & mask). */
static inline long nk_set_access(unsigned long area, unsigned long values,
                                 unsigned long mask, unsigned long start,
                                 unsigned long len)
{
    KRegs k = {0};
    k.r3 = area;
    k.r4 = values;
    k.r5 = mask;
    k.r6 = start;
    k.r7 = len;
    return nk_call(kcSetAreaAccess, &k);
}

/* Protect: read-only (PTE PP 11), for the NanoKernel too. Unprotect:
   read/write (PP 10). kAccessSupRWUserRO (PP 01, read-only for user mode
   only) does not stop the blue task's stores on 9.2.2 (tested: no faults),
   so Mac OS code cannot be told from the kernel by page protection. */
static inline long nk_protect(unsigned long area, unsigned long start,
                              unsigned long len)
{
    return nk_set_access(area, 0, kAccessWrite, start, len);
}

static inline long nk_unprotect(unsigned long area, unsigned long start,
                                unsigned long len)
{
    return nk_set_access(area, kAccessWrite, kAccessWrite, start, len);
}

/* A page that is plain read/write: one this code may protect and open. */
static inline int nk_access_rw(unsigned long bits)
{
    return (bits & (kAccessWrite | kAccessSupRWUserRO)) == kAccessWrite;
}

/* Exception Manager (MachineExceptions.h), declared here because the
   Multiversal Interfaces do not include it. Only theAddress and
   theReference of MemoryExceptionInfo are filled in on 9.2.2. */

typedef struct {
    unsigned long hi, lo;
} NKWide;

typedef struct {
    NKWide CTR, LR, PC;
    unsigned long CR, XER, MSR, MQ, ExceptionID, DAR;
} MachineInfo;

typedef struct {
    unsigned long theArea;    /* not filled in */
    unsigned long theAddress;
    long theError;            /* not filled in */
    unsigned long theReference; /* 0 = write */
} MemoryExceptionInfo;

typedef struct {
    NKWide R[32];
} RegisterInfo;

typedef struct {
    unsigned long theKind;
    MachineInfo *machineState;
    RegisterInfo *registerImage;
    void *FPUImage;
    MemoryExceptionInfo *memoryInfo;
    void *vectorImage;
} ExceptionInfo;

enum { kReadOnlyMemoryException = 6 };

typedef long (*ExceptionHandler)(ExceptionInfo *);
extern ExceptionHandler InstallExceptionHandler(ExceptionHandler handler);

/* The Exception Manager keeps one system-wide handler that it calls before
   the current process's handler, for exceptions in every process (ROM
   0xFFCF0CD0 on 9.2.2). Its installer has no public name; its TVector sits
   right after InstallExceptionHandler's in the Exception Manager's data
   section, so it is found there and its code is checked before use.
   Returns 0 if the code does not match. */
typedef ExceptionHandler (*InstallFn)(ExceptionHandler);

static inline InstallFn nk_find_system_installer(void)
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

#endif
