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
        .set    MPCallTable, kernel + 0xACB8    # entry n: routine - kernel - 4n
        .set    MPReturn, kernel + 0xB124       # CommonMPCallReturnPath, r3
        .set    SAA_pageloop, kernel + 0x10B68  # MPSetAreaAccess: @pageloop
        .set    SAA_done, kernel + 0x10BFC      # after the loop: unlock, return
        .set    GetPTEFromPLE, kernel + 0x124F8 # r30 PLE -> r16/r17 PTE, r18 &PTE

# Kernel structures.
        .set    EWA_CoherenceGroup, -0x338      # EWA.CPUBase + CPU.LLL.Freeform
        .set    EWA_PA_CurTask, -0x008
        .set    EWA_R1, 0x004                   # IntDSI saves the task's r0-r31
        .set    EWA_R24, 0x060                  # at EWA + 4n
        .set    EWA_R25, 0x064                  # (the emulator's: SR in bits 29-31)
        .set    KDP_PA_ECB, 0x658               # the emulator's context block,
        .set    ECB_R25, 0x1CC                  # physical, and its saved r25
        .set    EWA_SavedR8, -0x0E0             # MemRetryDSI saves these for a
        .set    EWA_SavedR9, -0x0DC             # user-mode access: r8, r9,
        .set    EWA_SavedR16, -0x0D8            # r16 (the flags), r17-r19, CR
        .set    EWA_SavedR17, -0x0D4
        .set    EWA_SavedR18, -0x0D0
        .set    EWA_SavedR19, -0x0CC
        .set    EWA_SavedCR, -0x0C8
        .set    KDP_HTABLock, -0xB90            # PSA.HTABLock
        .set    FLAG_EMU, 0x0080                # EWA.kFlagEmu (bit 8), oris
        .set    FLAG_BLUE, 0x0020               # EWA.kFlagBlue (bit 10), oris
        .set    FLAG_SIGP, 0x8000               # EWA.kFlagSIGP (bit 16), ori
        .set    CG_LA_CpuPlugin, 0x28
        .set    CG_CpuPluginSize, 0x30
        .set    CG_CpuPluginSpacePtr, 0x4C
        .set    Area_PageSize, 0x68
        .set    Area_AddressSpacePtr, 0x6C
        .set    Task_ID, 0x00
        .set    kAccessWrite, 0x02

# The owner check's control block (patch 4): 16 KB, physically contiguous
# and page aligned, that the program running the engine allocates and
# registers with MP call 109, by its physical address. Keep common/kcb.h in
# step.
        .set    CB_MAGIC, 0x000                 # 'ENFK'
        .set    CB_VERSION, 0x004               # 2
        .set    VERSION, 2                      # the layout below
        .set    CB_MODE, 0x008                  # MODE_*
        .set    CB_CURRENT, 0x00C               # the current application's owner
        .set    CB_ENTRIES, 0x010               # pages the owner table covers
        .set    CB_DENIED_PAGE, 0x014           # the page of the last denial
        .set    CB_SEQ, 0x018                   # events recorded
        .set    CB_FRAME, 0x01C                 # the emulator's frame code, or 0
        .set    CB_ALLOWED, 0x020               # counts: allowed and recorded,
        .set    CB_LOGGED, 0x024                # allowed offenses,
        .set    CB_DENIED, 0x028                # denied,
        .set    CB_BAIL, 0x02C                  # allowed but left to the hooks,
        .set    CB_SELF, 0x030                  # the current application's own,
        .set    CB_SKIPPED, 0x034               # dropped
        .set    CB_PAGES, 0x100                 # owner table pages, physical
                                                # (128 of them: 2 GB)
        .set    CB_RING, 0x300                  # RING_N events of 32 bytes
        .set    RING_N, 488                     # to the block's end
        .set    FRAME_LEN, 0x70                 # as common/hook.h kHookFrameLen
        .set    EV_SEQ, 0x00
        .set    EV_PC, 0x04
        .set    EV_ADDR, 0x08
        .set    EV_R24, 0x0C
        .set    EV_LR, 0x10
        .set    EV_SP, 0x14
        .set    EV_INFO, 0x18                   # owner, writer, current, flags
        .set    EV_TASK, 0x1C
        .set    EVF_68K, 1                      # the 68k emulator's store
        .set    EVF_TASK, 2                     # not the blue task
        .set    EVF_OFFENSE, 4                  # an application's code, not
        .set    EVF_DENIED, 8                   # its own page; and denied,
        .set    EVF_SKIPPED, 16                 # or dropped
        .set    MODE_PASS, 0                    # as without the table
        .set    MODE_LOG, 1
        .set    MODE_ENFORCE, 2
        .set    OWNER_SYSTEM, 1                 # 0 none, 2 reserved,
        .set    OWNER_APP, 3                    # 3 and up applications
        .set    MP_ENFORCER, 109                # an unused MP call

# --- Patch sites, in offset order ------------------------------------------

        stock   0x3630                          # MemRetryDSI, read-only page
        b       task_write                      # was: beq cr7,MRD_fault

        stock   0x5EC8                          # FDP_06c0, after its stwcx.
        b       stwcx_probe                     # was: isync

        stock   0xACB8 + 4 * MP_ENFORCER        # MP call table (patch 4)
        .4byte  enf_register - kernel - 4 * MP_ENFORCER # was: MPCallBad
                                                # (.long would stop `stock`
                                                # from resolving . - kernel)
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
probe_store:
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

# Patches 3 and 4: a store to a read-only page, decided by the kernel.
#
# The kernel runs a faulting store itself, in user mode, and a fault there
# comes to MemRetryDSI. At the patch site the store is a user-mode one and
# the page's PLE says read-only (cr7.eq). What follows handles a store's
# protection violation (DSISR) outside the CPU plugin; anything else goes
# on as before.
#
# With no control block registered, every store goes on to the fault path
# as in the stock kernel.
#
# Patch 3, in MODE_PASS (the engine is off, or turning off): a store into a
# page the engine owns (an owner byte that is not 0) is allowed in a task
# other than the blue task; the blue task's goes on to its hooks. A write
# fault in a task other than the blue task does not reach the hooks (the
# kernel stops the task, UnhandledDataFault), and the engine may still
# have such pages protected while it opens them. A page with no owner
# (the ROM image, another program's protection) faults as in the stock
# kernel. Each open is counted in task_log: a count, three zero words, then
# a ring of 16 entries (the task's PC, the address, the task's ID and the
# new PTE's lower word).
#
# Patch 4, with a control block registered (MP call 109, enf_register) in
# MODE_LOG or MODE_ENFORCE: the kernel decides for every task. It looks up
# the owner of the page in the owner table, one byte per 4 KB page of
# logical memory: 0 none, 1 the system heap, 3 and up an application. A
# page with no owner is not the engine's, and its store goes on to the
# fault path as in the stock kernel. Then it looks up the owner of the code
# doing the store: the PC, or for the 68k emulator its r24 - 2 (the 68k
# instruction after the store), except in the emulator's code that fills a
# call frame (CB_FRAME), which is the system's. Code nobody owns counts as
# the system's.
#
# A store by an application's code into a page that is not its own is an
# offense. In MODE_ENFORCE, an offense into another application's page is
# decided by who is current (CB_CURRENT):
# - into the current application's page: allowed and marked. The code can
#   be another application's that the current one called (a Gestalt
#   function or a callback in that application's partition), storing its
#   results.
# - by the current application in the blue task, with the 68k interrupt
#   mask at 0: denied. It is the one application the engine can stop
#   cleanly. (Stopping it from interrupt-time code would leave that
#   interrupt's dispatcher, such as the Time Manager's, unfinished: the
#   Time Manager then runs no task again.) The mask is in the emulator's
#   r25 (bits 29-31): the task's own r25 when the emulator is running, or
#   the one saved in its context block when native code is.
# - any other (an application's interrupt-time code, or an MP task):
#   dropped. The kernel skips the
#   store, so memory is not changed, and the code goes on as if it had
#   stored. The exception is stwcx_probe's store: skipping it would make the
#   caller's lwarx and stwcx. loop run forever, so that store is allowed.
# Offenses into the system heap, and offenses in MODE_LOG, are allowed and
# marked. A store into the current application's page that is not an
# offense is only counted (CB_SELF); every other store is recorded in the
# block's ring, and counted. A denied store goes on to the fault path with
# its page in CB_DENIED_PAGE, where the blue task's hooks deny it and stop
# the application.
#
# To drop a store, the kernel sets SRR0 to the next instruction of its own
# emulation of the store (MemRetryDSI's SRR0 is that instruction, run with
# instruction translation off) and returns to it, as MemRetryDSI_0x100 does
# for the case it skips. The emulation then finishes as after the store.
#
# To allow a store, the kernel sets the page's PTE to read/write (PP 10)
# and runs the store again, as MemRetryDSI does after filling the HTAB. The
# PLE still says read-only, so the page is write-protected again when its
# PTE leaves the HTAB or when MPSetAreaAccess protects it again (the engine
# does, from the ring, on its next tick). Until then every task can write
# to it.
#
# At the site: r27 is the address, r30 the PLE, r10 the task's PC, r12 its
# LR, r1 the KDP. MemRetryDSI saved r8, r9, r16-r19 and CR in the EWA, and
# IntDSI the task's registers at EWA + 4n. r26, r28-r31 and LR are free:
# MemRetryDSI sets r26 again and restores LR from SPRG2 before its rfi.

task_write:
        beq     cr7, 1f                         # read-only page
        b       MRD_next
1:      mfsprg  29, 0                           # EWA
        lwz     28, EWA_SavedR16(29)            # the flags
        andi.   9, 28, FLAG_SIGP
        bne     fault                           # the CPU plugin
        mfdsisr 9
        rlwinm  9, 9, 0, 4, 6
        lis     8, 0x0A00                       # protection violation, store
        cmpw    9, 8
        bne     fault
        bl      tw_base
tw_base:
        mflr    26                              # r26: tw_base, for the data
        lwz     31, enf_cb - tw_base(26)        # the control block, or 0
        cmpwi   31, 0
        beq     fault                           # none: as the stock kernel
        lwz     9, CB_MODE(31)
        cmpwi   9, MODE_PASS
        bne     check
        mr      8, 27                           # patch 3: the page's owner
        bl      owner_of
        cmpwi   8, 0
        beq     fault                           # not the engine's page
        li      31, 0
        andis.  9, 28, FLAG_BLUE
        bne     fault                           # the blue task: its hooks
        b       open                            # another task: allowed

check:  mr      8, 27                           # the page's owner
        bl      owner_of
        mr.     18, 8
        beq     fault                           # not the engine's page
        li      16, 0                           # the event's flags
        andis.  9, 28, FLAG_BLUE
        bne     3f
        ori     16, 16, EVF_TASK
3:      andis.  9, 28, FLAG_EMU                 # the store's code: the PC,
        mr      8, 10                           # or the 68k instruction
        beq     4f                              # after the store
        ori     16, 16, EVF_68K
        lwz     9, CB_FRAME(31)
        subf    9, 9, 10
        cmplwi  9, FRAME_LEN
        li      8, 0                            # the emulator's frame code
        blt     5f
        lwz     8, EWA_R24(29)
        addi    8, 8, -2
4:      bl      owner_of
5:      cmpwi   8, 0
        bne     6f
        li      8, OWNER_SYSTEM                 # nobody's: the system's
6:      mr      19, 8                           # its owner
        cmplwi  19, OWNER_APP
        blt     7f                              # system code
        cmpw    19, 18
        beq     7f                              # the application's own page
        ori     16, 16, EVF_OFFENSE
        cmplwi  18, OWNER_APP
        blt     record                          # the system heap: allowed
        lwz     9, CB_MODE(31)
        cmpwi   9, MODE_ENFORCE
        bne     record
        lwz     9, CB_CURRENT(31)
        cmpw    9, 18
        beq     record                          # into the current application
        andi.   17, 16, EVF_TASK
        bne     8f                              # another task
        cmpw    9, 19
        bne     8f                              # not the current application
        andis.  9, 28, FLAG_EMU                 # the 68k interrupt mask:
        lwz     9, EWA_R25(29)                  # the emulator's r25, live
        bne     5f
        lwz     9, KDP_PA_ECB(1)                # or saved
        lwz     9, ECB_R25(9)
5:      andi.   9, 9, 7
        bne     8f                              # interrupt time
        ori     16, 16, EVF_DENIED
        b       record
8:      mfsrr0  17                              # drop it, unless it is
        addi    9, 26, probe_store - tw_base    # stwcx_probe's store
        cmplw   17, 9
        beq     record
        ori     16, 16, EVF_SKIPPED
        b       record
7:      lwz     9, CB_CURRENT(31)               # not an offense: into the
        cmpw    9, 18                           # current application's page?
        bne     record
        li      8, CB_SELF
        bl      count
        b       open

record: lwz     9, CB_SEQ(31)
        addi    9, 9, 1                         # this event's number
        addi    8, 9, -1
        li      17, RING_N
        divwu   17, 8, 17
        mulli   17, 17, RING_N
        subf    8, 17, 8                        # (number - 1) mod RING_N
        slwi    8, 8, 5
        add     8, 8, 31                        # the entry - CB_RING
        stw     10, CB_RING + EV_PC(8)
        stw     27, CB_RING + EV_ADDR(8)
        lwz     17, EWA_R24(29)
        stw     17, CB_RING + EV_R24(8)
        stw     12, CB_RING + EV_LR(8)
        lwz     17, EWA_R1(29)
        stw     17, CB_RING + EV_SP(8)
        lwz     17, CB_CURRENT(31)
        rlwimi  16, 17, 8, 16, 23
        rlwimi  16, 19, 16, 8, 15
        rlwimi  16, 18, 24, 0, 7
        stw     16, CB_RING + EV_INFO(8)
        lwz     17, EWA_PA_CurTask(29)
        lwz     17, Task_ID(17)
        stw     17, CB_RING + EV_TASK(8)
        stw     9, CB_RING + EV_SEQ(8)          # the entry is complete
        stw     9, CB_SEQ(31)
        andi.   9, 16, EVF_DENIED
        beq     6f
        rlwinm  9, 27, 0, 0, 19
        stw     9, CB_DENIED_PAGE(31)
        li      8, CB_DENIED
        bl      count
        b       fault
6:      andi.   9, 16, EVF_SKIPPED
        beq     7f
        li      8, CB_SKIPPED
        bl      count
        mfsrr0  9                               # past the emulated store
        addi    9, 9, 4
        mtsrr0  9
        b       retry
7:      andi.   9, 16, EVF_OFFENSE
        li      8, CB_ALLOWED
        beq     8f
        li      8, CB_LOGGED
8:      bl      count

open:   bl      GetPTEFromPLE                   # r31: the block, or 0
        bns     cr7, bail                       # no physical page
        bge     cr5, bail                       # not in the HTAB
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
        cmpwi   31, 0
        bne     retry                           # patch 4 recorded it
        addi    29, 26, task_log - tw_base
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
retry:  mfsprg  30, 0                           # as MemRetryDSI_0x158,
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
bail:   cmpwi   31, 0                           # allowed, but the PTE is not
        beq     fault                           # there to change
        li      8, CB_BAIL
        bl      count
fault:  mfsprg  30, 0                           # report the fault, with CR
        lwz     8, EWA_SavedCR(30)              # as it was
        mtcr    8
        b       MRD_fault

# count: add 1 to the word at r31 + r8. Uses r9.
count:  lwzx    9, 31, 8
        addi    9, 9, 1
        stwx    9, 31, 8
        blr

# owner_of: r8 an address -> r8 its page's owner, from the table of the
# control block at r31 (0 past the table's end). Uses r9.
owner_of:
        srwi    8, 8, 12                        # the page number
        lwz     9, CB_ENTRIES(31)
        cmplw   8, 9
        bge     1f
        rlwinm  9, 8, 22, 10, 29                # (page >> 12) * 4
        addi    9, 9, CB_PAGES
        lwzx    9, 31, 9                        # that table page, physical
        rlwinm  8, 8, 0, 20, 31                 # page & 0xFFF
        lbzx    8, 9, 8
        blr
1:      li      8, 0
        blr

# MP call 109 (patch 4): r3 the control block's physical address, page
# aligned, with CB_MAGIC and CB_VERSION set; or 0 to unregister. Returns
# 0, or -50 (paramErr) for a block it does not take. The kernel uses the
# block (16 KB, physically contiguous) and the table pages it names with
# translation off, so they must stay where they are while registered.
#
# At entry: r3-r5 the arguments, r6 the ContextBlock, r1 the KDP; r14-r31
# and LR are free.
enf_register:
        cmpwi   3, 0
        beq     2f
        andi.   16, 3, 0xFFF
        bne     1f                              # not page aligned
        lwz     16, CB_MAGIC(3)
        lis     17, 0x454E                      # 'EN'
        ori     17, 17, 0x464B                  # 'FK'
        cmpw    16, 17
        bne     1f
        lwz     16, CB_VERSION(3)
        cmpwi   16, VERSION
        beq     2f
1:      li      3, -50
        b       MPReturn
2:      bl      3f
3:      mflr    16
        stw     3, enf_cb - 3b(16)
        li      3, 0
        b       MPReturn

        .globl  task_log                        # for build.sh's nm
task_log:
        .long   0, 0, 0, 0                      # opens, 0, 0, 0
        .rept   16
        .long   0, 0, 0, 0                      # pc, address, task, PTE
        .endr
        .globl  enf_cb
enf_cb: .long   0                               # the control block, physical
