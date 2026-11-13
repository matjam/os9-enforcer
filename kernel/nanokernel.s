# The NanoKernel the test machine boots: the stock NanoKernel 2.28
# (kernel/stock/NanoKernel-v02.28.bin, from kernel/extract.sh) with the
# patches below. kernel/build.sh assembles it.
#
# `stock N` copies the stock kernel's bytes from the current offset up to
# offset N; each patch then assembles its instructions at its site, so the
# sites must come in offset order. Labels are offsets in the kernel, so
# branches between stock and added code resolve here. Added code goes
# after the stock kernel (STOCK_END). The kernel must end by offset
# 0x50000, where the 68k emulator starts in the ROM image; build.sh checks
# that (an .if here cannot, once data follows the code). Names in comments
# are from os9-protect's refs/powermac-rom/NanoKernel and
# Internal/NKOpaque.a.

        .set    STOCK_END, 0x19B40

        .text
kernel:

        .macro  stock to
        .incbin "NanoKernel-v02.28.bin", . - kernel, \to - (. - kernel)
        .endm

# Stock code the added code branches to.
        .set    AcquireLock, kernel + 0x2700    # r8 the lock; uses r9
        .set    MRD_next, kernel + 0x3634       # MemRetryDSI, after patch 3's site
        .set    MRD_fault, kernel + 0x3680      # MemRetryDSI_0xe0: report the fault
        .set    MRD_retry, kernel + 0x373C      # MemRetryDSI_0x19c: rfi, run it again
        .set    FDP_011c, kernel + 0x591C       # end of an emulated access
        .set    SAA_pageloop, kernel + 0x10B68  # MPSetAreaAccess: @pageloop
        .set    SAA_done, kernel + 0x10BFC      # after the loop: unlock, return
        .set    GetPTEFromPLE, kernel + 0x124F8 # r30 PLE -> r16/r17 PTE, r18 &PTE

# Kernel structures.
        .set    EWA_CoherenceGroup, -0x338      # EWA.CPUBase + CPU.LLL.Freeform
        .set    EWA_PA_CurTask, -0x008
        .set    EWA_SavedR8, -0x0E0             # MemRetryDSI saves these for a
        .set    EWA_SavedR9, -0x0DC             # user-mode access: r8, r9,
        .set    EWA_SavedR16, -0x0D8            # r16 (the flags), r17-r19, CR
        .set    EWA_SavedR17, -0x0D4
        .set    EWA_SavedR18, -0x0D0
        .set    EWA_SavedR19, -0x0CC
        .set    EWA_SavedCR, -0x0C8
        .set    KDP_HTABLock, -0xB90            # PSA.HTABLock
        .set    FLAG_BLUE, 0x0020               # EWA.kFlagBlue (bit 10), oris
        .set    FLAG_SIGP, 0x8000               # EWA.kFlagSIGP (bit 16), ori
        .set    CG_LA_CpuPlugin, 0x28
        .set    CG_CpuPluginSize, 0x30
        .set    CG_CpuPluginSpacePtr, 0x4C
        .set    Area_PageSize, 0x68
        .set    Area_AddressSpacePtr, 0x6C
        .set    Task_ID, 0x00
        .set    kAccessWrite, 0x02

# --- Patch sites, in offset order ------------------------------------------

        stock   0x3630                          # MemRetryDSI, read-only page
        b       task_write                      # was: beq cr7,MRD_fault

        stock   0x5EC8                          # FDP_06c0, after its stwcx.
        b       stwcx_probe                     # was: isync

        stock   0x10B68                         # MPSetAreaAccess @pageloop
        b       cpup_keep_writable              # was: mr r8,r29

        stock   STOCK_END

# --- Added code ------------------------------------------------------------

# Patch 1: a stwcx. to a write-protected page.
#
# The kernel takes a data fault and runs the faulting instruction itself,
# with the caller's translation (IntDSI, then the FDP routines). A store
# that faults again in there reaches the blue task as a write violation,
# which is what the write hooks catch. For stwcx. (FDP_06c0) the kernel's
# own stwcx. finds no reservation, since the caller's lwarx reservation is
# gone by then, so it fails without touching memory and without a fault.
# The caller's loop runs lwarx and stwcx. again, faults again, and the
# machine hangs.
#
# When the kernel's stwcx. fails, the patch takes a reservation and stores
# the word's current value back. On a write-protected page that store
# faults like any other emulated store and reaches the blue task. On a
# writable page it leaves memory as it was. Either way the caller sees its
# stwcx. fail and tries again, which a stwcx. caller must allow for.
#
# r19 is the effective address + 4, r23 is -4, r21 is the caller's value.
# r20 is free here (only 8-byte stores use it; the routine after a stwcx.,
# FDP_015C, restores every register from the EWA) and survives a page
# fault taken inside the emulation (MemRetryDSI).

stwcx_probe:
        beq+    1f                              # the store happened
        lwarx   20,23,19
        stwcx.  20,23,19                        # faults if write-protected
        crxor   2,2,2                           # report failure (cr0.eq = 0)
1:      isync
        mfcr    23                              # as FDP_06c0: cr0 into the
        rlwimi  13,23,0,0,3                     # caller's saved CR
        b       FDP_011c

# Patch 2: keep the CPU plugin's pages writable.
#
# The CPU plugin (registered with MPRegisterCpuPlugin) keeps its code, data
# and stack in the system heap. The kernel runs it in SIGP mode, and a
# write fault there does not reach the blue task: the kernel abandons the
# plugin call and returns an error (IntReturnFromSIGP), which stops
# interrupt handling. So MPSetAreaAccess leaves alone each page of the
# plugin's range when a call would remove write access, in the address
# space the plugin runs in. Other pages, and calls that do not remove write
# access, go through as before.
#
# At @pageloop: r29 is the page, r28 the last byte of the range, r31 the
# Area, r4 the access values and r5 the mask. r8, r9 and r16-r19 are free
# (the loop sets them before use). The kernel runs with translation off,
# and the CoherenceGroup pointer in the EWA is physical.

cpup_keep_writable:
        andi.   8,5,kAccessWrite                # changes write access?
        beq     2f
        andi.   8,4,kAccessWrite                # and removes it?
        bne     2f
        mfsprg  9,0                             # EWA
        lwz     9,EWA_CoherenceGroup(9)
        lwz     16,CG_CpuPluginSpacePtr(9)
        lwz     17,Area_AddressSpacePtr(31)
        cmpw    16,17
        bne     2f
        lwz     16,CG_LA_CpuPlugin(9)
        lwz     17,CG_CpuPluginSize(9)          # 0 before registration
        subf    8,16,29
        cmplw   8,17
        blt     3f                              # page - base < size
2:      mr      8,29                            # as @pageloop
        b       SAA_pageloop + 4
3:      lwz     19,Area_PageSize(31)            # skip the page, as the
        add     29,29,19                        # end of the loop does
        subf.   8,29,28
        blt     4f                              # past the end
        b       SAA_pageloop                    # (bge cannot reach it)
4:      b       SAA_done

# Patch 3: a store by a Multiprocessing Services task to a read-only page.
#
# A write fault in a task other than the blue task does not reach the blue
# task's hooks: the kernel stops the task (UnhandledDataFault). MP tasks
# keep stacks and data in the system heap, and run the ROM's MP library,
# so protecting the system heap stopped them (and with them, likely, the
# USB keyboard and mouse). Until the kernel has page owners (plan step 3),
# a store by any task other than the blue task or the CPU plugin is
# allowed: the kernel makes the page writable and runs the store again.
#
# The kernel runs a faulting store itself, in user mode, and a fault there
# comes to MemRetryDSI. At the patch site that store is a user-mode one
# and the page's PLE says read-only (cr7.eq). The patch checks the flags
# from before the fault, the DSISR (a store, a protection violation) and
# that the page is in the HTAB, then sets the PTE's PP bits to 10
# (read/write) and returns to the store, as MemRetryDSI does after
# filling the HTAB. The PLE still says read-only, so the page is write-
# protected again when its PTE leaves the HTAB or when MPSetAreaAccess
# protects it again. Until then, every task can write to it, the blue task
# included.
#
# Each open is counted in task_log, with the task's PC, the address, the
# task's ID and the new PTE in a ring of 16 (read it from the host at
# physical 0x00F10000 + its offset; the build prints the offset).
#
# At the site: r27 is the address, r30 the PLE, r1 the KDP. MemRetryDSI
# saved r8, r9, r16-r19 and CR in the EWA; r28-r31 and LR are free (it
# restores LR from SPRG2).

task_write:
        beq     cr7, 1f                         # read-only page
        b       MRD_next
1:      mfsprg  29, 0                           # EWA
        lwz     28, EWA_SavedR16(29)            # the flags
        andis.  9, 28, FLAG_BLUE
        bne     8f                              # the blue task: its hooks
        andi.   9, 28, FLAG_SIGP
        bne     8f                              # the CPU plugin
        mfdsisr 28
        rlwinm  28, 28, 0, 4, 6
        lis     9, 0x0A00                       # protection violation, store
        cmpw    28, 9
        bne     8f
        bl      GetPTEFromPLE
        bns     cr7, 8f                         # no physical page
        bge     cr5, 8f                         # not in the HTAB
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
        rlwinm  16, 16, 0, 1, 31                # clear V, as InvalPTE
        stw     16, 0(18)
        sync
        tlbie   27
        sync
        tlbsync
        sync
        isync
        li      9, 2
        rlwimi  17, 9, 0, 30, 31                # PP 10: read/write
        stw     17, 4(18)                       # as SetPTE
        eieio
        oris    16, 16, 0x8000                  # V
        stw     16, 0(18)
        sync
        li      9, 0
        stw     9, KDP_HTABLock(1)              # release
        bl      2f
2:      mflr    29
        addi    29, 29, task_log - 2b
        lwz     9, 0(29)                        # count
        addi    8, 9, 1
        stw     8, 0(29)
        rlwinm  9, 9, 4, 24, 27                 # (count mod 16) * 16
        add     9, 9, 29
        stw     10, 16(9)                       # the task's PC
        stw     27, 20(9)                       # the address
        mfsprg  8, 0
        lwz     8, EWA_PA_CurTask(8)
        lwz     8, Task_ID(8)
        stw     8, 24(9)                        # the task
        stw     17, 28(9)                       # the new PTE, lower word
        mfsprg  30, 0                           # as MemRetryDSI_0x158,
        lwz     8, EWA_SavedCR(30)              # without leaving user mode
        mtcr    8
        lwz     9, EWA_SavedR9(30)
        lwz     16, EWA_SavedR16(30)
        lwz     17, EWA_SavedR17(30)
        lwz     18, EWA_SavedR18(30)
        lwz     19, EWA_SavedR19(30)
        lwz     8, EWA_SavedR8(30)
        mfsprg  28, 2
        mtlr    28
        b       MRD_retry
8:      mfsprg  30, 0                           # report the fault, with CR
        lwz     8, EWA_SavedCR(30)              # as it was
        mtcr    8
        b       MRD_fault

        .globl  task_log                        # for build.sh's nm
task_log:
        .long   0, 0, 0, 0                      # opens, 0, 0, 0
        .rept   16
        .long   0, 0, 0, 0                      # pc, address, task, PTE
        .endr
