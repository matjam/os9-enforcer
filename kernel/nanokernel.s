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
# are from refs/powermac-rom/NanoKernel and
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
        .set    PF1_paged, kernel + 0x4D78      # PagingFunc1 @_154 + 4
        .set    PF1_paged_rest, kernel + 0x4DB8 # its PLE is not "present, no PTE"
        .set    PF1_build, kernel + 0x4E28      # @_208: hash, build the PTE
        .set    PF1_list, kernel + 0x4F98       # @_368 + 0x10, after its lwz r31
        .set    PF1_list_rest, kernel + 0x4FD0
        .set    FindAreaAbove, kernel + 0x103EC # r8 space, r9 address -> r8
                                                # area; uses r16, r17
        .set    SpaceGetPagePLE, kernel + 0x1242C # r8 address, r31 area -> r30
                                                # &PLE, cr0.eq none; r16-r18
        .set    InvalPTE, kernel + 0x12528      # r8 page, r16/r17 PTE, r18
                                                # &PTE, r30 &PLE; uses r14
        .set    DeletePTE, kernel + 0x12590     # r17 PTE lower, r18 &PTE, r30
                                                # &PLE; uses r14, r16

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
        .set    KDP_PageAttributeInit, 0x688    # a new PTE's lower word
        .set    KDP_HTABORG, 0x6A4
        .set    EWA_PA_CurAddressSpace, -0x01C  # physical
        .set    Space_SRs, 0x030                # AddressSpace.SRs[16]
        .set    Area_Flags, 0x020               # 0x8000: contiguous, no PLEs
        .set    Area_LogicalBase, 0x024
        .set    Area_LogicalEnd, 0x028          # its last byte
        .set    PLE_InHTAB, 0x800               # bits 0-19 then index the PTE
        .set    PLE_HasPage, 0x001              # bits 0-19 are its page
        .set    Area_BytesMapped, 0x038
        .set    EWA_SpecialAreaPtr, -0x0E4      # PagingFunc1's area
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
        .set    CB_VERSION, 0x004               # 5
        .set    VERSION, 5                      # the layout below
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
        .set    CB_APPLZONE, 0x038              # ApplZone (low memory 0x2AA),
                                                # physical, or 0
        .set    CB_DENY_NATIVE, 0x03C           # the last denied native store:
                                                # its page, then its event
        .set    CB_DENY_68K, 0x044              # the same for a 68k store
        .set    CB_TABLES, 0x04C                # stores into the tables,
                                                # dropped
        .set    CB_SPACES, 0x050                # 1: a VSID for each
                                                # application (patch 5)
        .set    CB_REBUILT, 0x054               # PTEs built again for
                                                # another VSID
        .set    CB_VSWITCH, 0x058               # VSID switches (OP_SWITCH)
        .set    CB_LAZY, 0x05C                  # VSID switches at a store
        .set    CB_LAZY_PC, 0x060               # and the PC of the last
        .set    CB_SLICE, 0x064                 # preemption (patch 6): the
                                                # slice in timebase ticks, 0
                                                # for off;
        .set    CB_NEXT, 0x068                  # no redirect before this
                                                # timebase (low word);
        .set    CB_STUB, 0x06C                  # the yield stub;
        .set    CB_YIELD_PC, 0x070              # the PC a redirect replaced,
                                                # 0 when none is pending;
        .set    CB_PREEMPTS, 0x074              # redirects made;
        .set    CB_ZONE, 0x078                  # ApplZone at the last slice
        .set    CB_YIELDED, 0x080               # 128 bytes, one per owner:
                                                # 1 once ApplZone left it
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
        .set    OWNER_SYSTEM, 1                 # 0 none,
        .set    OWNER_TABLES, 2                 # the block and its table,
        .set    OWNER_APP, 3                    # 3 and up applications;
        .set    OWNER_CODE, 0x80                # with this, its code only
        .set    MP_ENFORCER, 109                # an unused MP call
        .set    OP_OWNERS, 1                    # its operations (r3)
        .set    OP_MODE, 2
        .set    OP_CURRENT, 3
        .set    OP_SPACES, 4
        .set    OP_SWITCH, 5
        .set    OP_FLUSH, 6
        .set    OP_PATCH, 7
        .set    OP_PREEMPT, 8
        .set    OP_YIELD, 9
        .set    OP_RESUME, 10
        .set    OP_FRESH, 11
        .set    OP_SPARSE, 12                   # patch 7
        .set    OP_POOL_PUT, 13
        .set    OP_POOL_GET, 14
        .set    OP_SPARSE_FREE, 15
        .set    OP_SPARSE_INFO, 16
        .set    VSID_APP, 0xFFF00               # application VSIDs: segment
                                                # << 20 | VSID_APP + owner
        .set    CTX_MSR, 0x0A4                  # ContextBlock: MSR,
        .set    CTX_CODEPTR, 0x0FC              # the PC,
        .set    CTX_R0, 0x104                   # r0,
        .set    CTX_R6, 0x134                   # r6 and r7 (an MP call's 4th
        .set    CTX_R7, 0x13C                   # and 5th arguments)
        .set    MSR_PR, 0x4000                  # user mode
        .set    RB_PC, 0x00                     # OP_RESUME's register block
        .set    RB_LR, 0x04
        .set    RB_CTR, 0x08
        .set    RB_CR, 0x0C
        .set    RB_XER, 0x10
        .set    RB_R0, 0x14
        .set    RB_R1, 0x18
        .set    RB_R3, 0x1C
        .set    RB_R4, 0x20

# --- Patch sites, in offset order ------------------------------------------

        stock   0x3630                          # MemRetryDSI, read-only page
        b       task_write                      # was: beq cr7,MRD_fault

        stock   0x4D74                          # PagingFunc1 @_154 (patch 5)
        b       pf1_paged                       # was: lwz r31,0x688(r1)
        stock   0x4DB4                          # a PTE to build
        b       pf1_paged_pp                    # was: beq @_208
        stock   0x4F94                          # @_368, page-list PMDTs
        b       pf1_list                        # was: lwz r31,0x688(r1)
        stock   0x4FCC
        b       pf1_list_pp                     # was: beq @_208

        stock   0x5EC8                          # FDP_06c0, after its stwcx.
        b       stwcx_probe                     # was: isync

        stock   0xACB8 + 4 * MP_ENFORCER        # MP call table (patch 4)
        .4byte  enf_register - kernel - 4 * MP_ENFORCER # was: MPCallBad
                                                # (.long would stop `stock`
                                                # from resolving . - kernel)
        stock   0x10B68                         # MPSetAreaAccess @pageloop
        b       cpup_keep_writable              # was: mr r8,r29

        stock   0x146D0                         # the blue task's 68k interrupt
        bl      preempt                         # from native code (patch 6)
                                                # was: lwz r6,KDP.PA_ECB(r1)

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
# decided by who is current (CB_CURRENT). The kernel finds that out at each
# store: the owner of the page ApplZone points into, read through
# CB_APPLZONE, since the Process Manager sets ApplZone on every switch. (The
# engine sees a switch only on its next tick.) Without CB_APPLZONE, the
# engine's value stands.
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
# its page and event number in CB_DENY_NATIVE or CB_DENY_68K, where the
# blue task's hook for that kind of store denies it and stops the
# application; each hook remembers the last number it took, since it
# cannot write the block.
#
# The control block and the owner table have owner OWNER_TABLES, and the
# engine protects them: every store into them, by any code in any mode
# (patch 3's too), is dropped and counted (CB_TABLES). The kernel writes
# them only with translation off, and the engine changes them through the
# operations of MP call 109.
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
        bl      ro_by_owner                     # or read-only in this VSID
        beq     cr6, 1f                         # (patch 5)
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
        cmpwi   8, OWNER_TABLES
        beq     drop_table                      # the engine's own tables
        li      31, 0
        andis.  9, 28, FLAG_BLUE
        bne     fault                           # the blue task: its hooks
        b       open                            # another task: allowed

check:  lwz     9, CB_APPLZONE(31)              # who is current: the owner
        cmpwi   9, 0                            # of ApplZone's page
        beq     2f
        lhz     8, 0(9)                         # ApplZone is 2-byte aligned
        lhz     9, 2(9)
        rlwimi  9, 8, 16, 0, 15
        mr      8, 9
        bl      code_of                         # its first page can be
        cmplwi  8, OWNER_APP                    # OWNER_CODE's
        bge     1f
        li      8, 0                            # not an application's
1:      stw     8, CB_CURRENT(31)
        lwz     9, CB_SPACES(31)                # patch 5: the blue task's
        cmpwi   9, 0                            # VSIDs follow ApplZone; a
        beq     2f                              # switch the hook did not
        andis.  9, 28, FLAG_BLUE                # see is made here, and the
        beq     2f                              # store runs again
        lwz     9, vsid_mask - tw_base(26)
        rlwinm  16, 27, 4, 28, 31               # the store's segment
        srw     9, 9, 16
        andi.   9, 9, 1
        beq     2f                              # no application VSIDs there
        mfsrin  9, 27
        bl      vsid_owner
        cmpw    8, 9
        beq     2f
        bl      space_switch
        li      8, CB_LAZY
        bl      count
        stw     10, CB_LAZY_PC(31)              # for the engine's hook
        lwz     9, CB_APPLZONE(31)              # a new slice (patch 6)
        lhz     8, 0(9)
        lhz     9, 2(9)
        rlwimi  9, 8, 16, 0, 15
        mr      8, 9
        bl      slice_new
        b       retry
2:      mr      8, 27                           # the page's owner
        bl      owner_of
        mr.     18, 8
        beq     fault                           # not the engine's page
        cmpwi   18, OWNER_TABLES
        beq     drop_table                      # the engine's own tables
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
4:      bl      code_of
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

drop_table:                                     # a store into the control
        li      8, CB_TABLES                    # block or the owner table, by
        bl      count                           # any code in any mode:
        b       skip                            # dropped

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
        lwz     17, CB_SEQ(31)                  # for the hook that takes the
        andi.   8, 16, EVF_68K                  # fault: the page and the
        li      8, CB_DENY_NATIVE               # event's number
        beq     5f
        li      8, CB_DENY_68K
5:      add     8, 8, 31
        stw     9, 0(8)
        stw     17, 4(8)
        li      8, CB_DENIED
        bl      count
        lwz     9, 0(30)                        # patch 5: a page read-only
        rlwinm  8, 9, 0, 29, 30                 # by owner only gets a
        cmpwi   8, 4                            # read-only PLE (PLE & 6 = 4),
        beq     fault                           # so that the fault reads as
        rlwinm  9, 9, 0, 31, 28                 # a write-protect fault to the
        ori     9, 9, 4                         # VM Manager, which otherwise
        stw     9, 0(30)                        # retries the store; the
        b       fault                           # engine opens it again
6:      andi.   9, 16, EVF_SKIPPED
        beq     7f
        li      8, CB_SKIPPED
        bl      count
skip:   mfsrr0  9                               # past the emulated store
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
# control block at r31 (0 past the table's end). A byte with OWNER_CODE
# marks a page where an application's code lies but which is not its
# memory (the partly used first page of a partition, code outside it):
# owner_of gives 0 for it, and code_of gives the application, for the
# owner of a store's code and of ApplZone. Use r9 and cr0.
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
        rlwinm  9, 8, 25, 31, 31                # OWNER_CODE: 1
        addi    9, 9, -1                        # then 0, else all ones
        and     8, 8, 9
        blr
1:      li      8, 0
        blr

code_of:
        srwi    8, 8, 12
        lwz     9, CB_ENTRIES(31)
        cmplw   8, 9
        bge     1f
        rlwinm  9, 8, 22, 10, 29
        addi    9, 9, CB_PAGES
        lwzx    9, 31, 9
        rlwinm  8, 8, 0, 20, 31
        lbzx    8, 9, 8
        clrlwi  8, 8, 25                        # without OWNER_CODE
        blr
1:      li      8, 0
        blr

# Patch 5: a VSID for each application.
#
# Every application runs in the blue task, in the system's address space.
# With OP_SPACES on, that space's segment registers for the segments in
# vsid_mask (those holding the Process Manager's heap, where partitions and
# temporary memory lie) carry the current application's own VSIDs: segment
# << 20 | VSID_APP + its owner byte. OP_SWITCH, from the hook in the
# Process Manager's switch, writes them into the AddressSpace and loads
# them; the scheduler loads them from there after another space ran, and
# MP tasks, which share the space, see the same. A store that finds the
# VSIDs behind ApplZone switches them itself (CB_LAZY, in task_write).
#
# A page keeps one PTE: its PLE records one HTAB slot, and the kernel's VM
# and area code relies on that. When the PTE was built for another VSID,
# PagingFunc1 now drops it (InvalPTE folds its R and C bits into the PLE,
# DeletePTE points the PLE at the page again) and builds one for this VSID
# (CB_REBUILT). A PTE for a page another application owns is read-only for
# every key (PP 11; the kernel retries a user store in supervisor mode
# when only PP 01 stops it); otherwise PP comes from the PLE as before. So
# a switch re-protects nothing: each page touched after a switch is built
# again once, in the kernel, and an application's own pages are writable
# in its VSID.
#
# A store into a page that is read-only by owner (its PLE says read/write)
# comes to task_write like one into a write-protected page (ro_by_owner),
# and is decided the same way. When it is denied, the PLE is made
# read-only too (record): with virtual memory on, the 68k emulator asks the
# VM Manager about a write fault first, and it retries a store into a page
# its PLE says is writable, forever. The engine makes the PLE writable
# again when it takes the event. A page whose owner byte changes loses its
# PTE (OP_OWNERS), so its next access builds it for the new owner.

pf1_paged:
        bl      vsid_fix
        lwz     31, KDP_PageAttributeInit(1)    # as at the site
        b       PF1_paged
pf1_list:
        bl      vsid_fix
        lwz     31, KDP_PageAttributeInit(1)
        b       PF1_list
pf1_paged_pp:
        bne     1f                              # as at the site: build only
        bl      pp_owner                        # for "present, no PTE"
        b       PF1_build
1:      bge     cr7, 2f                         # not "no page": as before
        bl      sparse_fill                     # patch 7
        bne     2f
        li      8, 0x801                        # as @_12c, for an area not
        li      9, 0x01                         # at 0; then the test again,
        lwz     28, 0(26)                       # with the page in the PLE
        b       pf1_paged
2:      b       PF1_paged_rest
pf1_list_pp:
        bne     1f
        bl      pp_owner
        b       PF1_build
1:      b       PF1_list_rest

# Patch 7: sparse memory. The stock kernel has areas whose pages have no
# physical page until one is mapped (MPCreateArea, MP call 72, then
# MPMapPage), and PagingFunc1 reports a touch of such a page as a page
# fault, which Mac OS cannot serve with virtual memory off. Between
# sparse_lo and sparse_hi (OP_SPARSE), the patch serves it: it takes a
# physical page from its pool, clears it, puts it in the page's PLE as
# MPMapPage does, and lets PagingFunc1 build the PTE. The pool is a list
# through the first word of each free page (physical addresses), filled by
# OP_POOL_PUT with pages the caller owns and will not use, and protected by
# the HTAB lock, which PagingFunc1's caller holds. With the pool empty the
# fault goes on as in the stock kernel, so the caller must keep the pool
# ahead of what can be touched. OP_SPARSE_FREE takes pages back.
#
# sparse_fill: r27 the address, r26 &PLE, r28 the PLE (no page, no PTE).
# Returns cr0.eq when the PLE has a page now. Uses r14-r20, cr0, cr5.
sparse_fill:
        mflr    14
        bl      1f
1:      mflr    16
        mtlr    14
        lwz     17, sparse_lo - 1b(16)
        lwz     18, sparse_hi - 1b(16)
        cmplw   cr5, 27, 17
        blt     cr5, 9f
        cmplw   cr5, 27, 18
        bge     cr5, 9f
        lwz     19, pool_head - 1b(16)
        cmpwi   19, 0
        beq     8f
        lwz     17, 0(19)                       # the next free page
        stw     17, pool_head - 1b(16)
        lwz     17, pool_count - 1b(16)
        addi    17, 17, -1
        stw     17, pool_count - 1b(16)
        li      17, 0                           # clear it
        li      20, 0
2:      stwx    20, 19, 17
        addi    17, 17, 4
        cmplwi  17, 4096
        blt     2b
        andi.   28, 28, 0x7E6                   # as MPMapPage: the flags,
        or      28, 28, 19                      # the page, "has a page"
        ori     28, 28, PLE_HasPage
        stw     28, 0(26)
        mfsprg  17, 0
        lwz     17, EWA_SpecialAreaPtr(17)      # the area, from @_88
        lwz     18, Area_BytesMapped(17)
        addi    18, 18, 4096
        stw     18, Area_BytesMapped(17)
        lwz     17, sparse_fills - 1b(16)
        addi    17, 17, 1
        stw     17, sparse_fills - 1b(16)
        cmpw    17, 17                          # eq
        blr
8:      lwz     17, sparse_empty - 1b(16)       # no page to give
        addi    17, 17, 1
        stw     17, sparse_empty - 1b(16)
9:      cmpwi   1, 0                            # ne: r1 is the KDP
        blr

# vsid_fix: at PagingFunc1's PLE (r26 &PLE, r28 the PLE, r27 the address),
# when the PLE's PTE has another VSID than this segment's, drop the PTE
# and reload r28. Keeps r8, r9, r26, r27; uses r14-r22, r30, cr0, cr5-7.
vsid_fix:
        andi.   14, 28, PLE_InHTAB
        beqlr                                   # no PTE
        lwz     15, KDP_HTABORG(1)
        rlwinm  16, 28, 23, 9, 28               # its offset, as GetPTEFromPLE
        lwzx    16, 15, 16                      # its upper word
        mfsrin  17, 27
        rlwinm  16, 16, 25, 8, 31               # its VSID
        clrlwi  17, 17, 8                       # this segment's
        cmpw    16, 17
        beqlr
        mflr    22
        mr      20, 8
        mr      21, 9
        mr      30, 26
        bl      GetPTEFromPLE                   # r16/r17, r18; uses r19
        rlwinm  8, 27, 0, 0, 19
        bl      InvalPTE                        # its R and C into the PLE
        bl      DeletePTE                       # the PLE points at the page
        lwz     28, 0(26)
        bl      1f
1:      mflr    15
        lwz     14, enf_cb - 1b(15)
        cmpwi   14, 0
        beq     2f
        lwz     15, CB_REBUILT(14)
        addi    15, 15, 1
        stw     15, CB_REBUILT(14)
2:      mr      8, 20
        mr      9, 21
        mtlr    22
        blr

# pp_owner: before PagingFunc1 builds the PTE for address r27 with lower
# word r31, set PP 11 when application VSIDs are on in its segment and an
# application owns the page that is not this VSID's. Keeps r26-r28; uses
# r8, r9, r14-r16, r22, cr0, cr1.
pp_owner:
        mflr    22
        bl      1f
1:      mflr    14
        lwz     15, vsid_mask - 1b(14)
        lwz     14, enf_cb - 1b(14)
        cmpwi   14, 0
        beq     9f                              # no block
        lwz     16, CB_SPACES(14)
        cmpwi   16, 0
        beq     9f                              # no application VSIDs
        rlwinm  16, 27, 4, 28, 31               # the segment
        srw     16, 15, 16
        andi.   16, 16, 1
        beq     9f                              # none in this segment
        mr      16, 31
        mr      31, 14
        mr      8, 27
        bl      owner_of                        # uses r9
        mr      31, 16
        cmplwi  8, OWNER_APP
        blt     9f                              # not an application's page
        mfsrin  9, 27
        bl      vsid_owner
        cmpw    8, 9
        beq     9f                              # this VSID's application's
        ori     31, 31, 3                       # PP 11: read-only
9:      mtlr    22
        blr

# vsid_owner: r9 a segment register's value -> r9 the owner byte whose
# application VSID it holds, or 0. Uses cr1.
vsid_owner:
        clrlwi  9, 9, 12                        # the VSID's low 20 bits
        addis   9, 9, -0x10                     # - VSID_APP
        addi    9, 9, 0x100
        cmpwi   cr1, 9, 0
        bgelr   cr1
        li      9, 0                            # a kernel space's
        blr

# ro_by_owner: at task_write's site (r27 the address), cr6.eq when the page
# is read-only in this VSID because another application owns it. Keeps
# cr0, r16, r27, r31; uses r8, r9, r17, r18, r26, r28, r29, cr1, LR.
ro_by_owner:
        mfcr    29
        mflr    28
        li      18, 0                           # not read-only by owner
        bl      1f
1:      mflr    17
        lwz     9, vsid_mask - 1b(17)
        lwz     17, enf_cb - 1b(17)
        cmpwi   cr1, 17, 0
        beq     cr1, 9f
        lwz     8, CB_SPACES(17)
        cmpwi   cr1, 8, 0
        beq     cr1, 9f
        rlwinm  8, 27, 4, 28, 31
        srw     9, 9, 8
        andi.   9, 9, 1
        beq     9f
        mr      26, 31
        mr      31, 17
        mr      8, 27
        bl      owner_of
        mr      31, 26
        cmplwi  cr1, 8, OWNER_APP
        blt     cr1, 9f
        mfsrin  9, 27
        bl      vsid_owner
        cmpw    cr1, 8, 9
        beq     cr1, 9f
        li      18, 1
9:      mtcrf   0x80, 29                        # cr0 as it was
        cmpwi   cr6, 18, 1
        mtlr    28
        blr

# space_switch: give the system space, the blue task's, the VSIDs of
# application r8 (0: the system's own) in the segments of vsid_mask, and
# load them when it is the current space. Keeps r8; uses r9, r16-r19,
# cr0, cr1.
space_switch:
        mflr    19
        bl      1f
1:      mflr    17
        mtlr    19
        lwz     16, sys_space - 1b(17)
        cmpwi   16, 0
        beqlr                                   # never turned on
        lwz     9, vsid_mask - 1b(17)
        stw     8, vsid_cur - 1b(17)
        mfsprg  18, 0
        lwz     18, EWA_PA_CurAddressSpace(18)
        cmpw    cr1, 18, 16                     # is it loaded?
        li      17, 0
        cmpwi   8, 0
        beq     2f
        lis     17, VSID_APP >> 16
        ori     17, 17, VSID_APP & 0xFFFF
        add     17, 17, 8                       # VSID_APP + owner
2:      oris    17, 17, 0x2000                  # Kp 1, Ks 0, as the kernel's
        addi    16, 16, Space_SRs
        li      19, 0                           # the segment
3:      andi.   18, 9, 1
        beq     4f
        rlwinm  18, 19, 20, 8, 11               # segment << 20
        or      18, 18, 17
        stw     18, 0(16)
4:      srwi    9, 9, 1
        addi    16, 16, 4
        addi    19, 19, 1
        cmpwi   19, 16
        blt     3b
        bnelr   cr1                             # loaded at its next switch
        addi    16, 16, -64
        isync
        lwz     17, 0(16)
        lwz     18, 4(16)
        mtsr    0, 17
        mtsr    1, 18
        lwz     17, 8(16)
        lwz     18, 12(16)
        mtsr    2, 17
        mtsr    3, 18
        lwz     17, 16(16)
        lwz     18, 20(16)
        mtsr    4, 17
        mtsr    5, 18
        lwz     17, 24(16)
        lwz     18, 28(16)
        mtsr    6, 17
        mtsr    7, 18
        lwz     17, 32(16)
        lwz     18, 36(16)
        mtsr    8, 17
        mtsr    9, 18
        lwz     17, 40(16)
        lwz     18, 44(16)
        mtsr    10, 17
        mtsr    11, 18
        lwz     17, 48(16)
        lwz     18, 52(16)
        mtsr    12, 17
        mtsr    13, 18
        lwz     17, 56(16)
        lwz     18, 60(16)
        mtsr    14, 17
        mtsr    15, 18
        isync
        blr

# flush_page: drop the PTE of the page at r8, so that its next access
# builds it again. The caller holds the HTAB lock. Keeps r22-r29, r31;
# uses r8, r9, r14-r21, r30, cr0, cr1, cr5-7.
flush_page:
        mflr    20
        mr      21, 31
        mr      15, 8                           # the page
        bl      1f
1:      mflr    9
        lwz     8, sys_space - 1b(9)
        cmpwi   8, 0
        bne     2f
        mfsprg  8, 0                            # before OP_SPACES: the
        lwz     8, EWA_PA_CurAddressSpace(8)    # caller's
2:      mr      9, 15
        bl      FindAreaAbove                   # uses r16, r17
        lwz     16, Area_LogicalBase(8)
        lwz     17, Area_LogicalEnd(8)
        cmplw   15, 16
        blt     8f                              # in no area
        cmplw   15, 17
        bgt     8f
        lwz     17, Area_Flags(8)
        andi.   17, 17, 0x8000
        bne     8f                              # contiguous: no PLEs
        mr      31, 8
        mr      8, 15
        bl      SpaceGetPagePLE                 # uses r16-r18
        beq     8f
        lwz     16, 0(30)
        andi.   16, 16, PLE_InHTAB
        beq     8f                              # no PTE
        bl      GetPTEFromPLE
        mr      8, 15
        bl      InvalPTE
        bl      DeletePTE
8:      mr      31, 21
        mtlr    20
        blr

# flush_owned: drop the PTE of every application page (owner 3 and up,
# without OWNER_CODE) in the segments of vsid_mask. r31 the block; the caller holds the HTAB
# lock. Uses r8, r9, r14-r22, r25, r27, r29, r30.
flush_owned:
        mflr    29
        bl      1f
1:      mflr    9
        lwz     25, vsid_mask - 1b(9)
        lwz     27, CB_ENTRIES(31)
        li      22, 0                           # the page
2:      cmplw   22, 27
        bge     7f
        srwi    9, 22, 16                       # its segment
        srw     9, 25, 9
        andi.   9, 9, 1
        bne     3f
        addis   22, 22, 1                       # on to the next segment
        rlwinm  22, 22, 0, 0, 15
        b       2b
3:      rlwinm  9, 22, 22, 10, 29               # (page >> 12) * 4
        addi    9, 9, CB_PAGES
        lwzx    9, 31, 9                        # its table page
        rlwinm  8, 22, 0, 20, 31
        lbzx    8, 9, 8
        cmplwi  8, OWNER_APP
        blt     4f
        cmplwi  8, OWNER_CODE
        bge     4f                              # not its memory
        slwi    8, 22, 12
        bl      flush_page
4:      addi    22, 22, 1
        b       2b
7:      mtlr    29
        blr

# MP call 109 (patch 4): r3 the control block's physical address, page
# aligned, with CB_MAGIC and CB_VERSION set; or 0 to unregister; or an
# operation (OP_*, below). Returns 0, or -50 (paramErr) for a block it
# does not take. The kernel uses the block (16 KB, physically contiguous)
# and the table pages it names with translation off, so they must stay
# where they are while registered. Unregistering turns application VSIDs
# off first (patch 5).
#
# At entry: r3-r5 the arguments, r6 the ContextBlock (the caller's r6 and
# r7 at CTX_R6 and CTX_R7), r1 the KDP; r14-r31 and LR are free.
enf_register:
        cmpwi   3, 0
        beq     0f
        andi.   16, 3, 0xFFF
        bne     enf_op                          # not page aligned
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
0:      bl      3f
3:      mflr    16
        lwz     31, enf_cb - 3b(16)
        cmpwi   31, 0
        beq     2f
        lwz     9, CB_SPACES(31)
        cmpwi   9, 0
        beq     2f
        bl      spaces_off
        li      3, 0
2:      bl      3f
3:      mflr    16
        stw     3, enf_cb - 3b(16)
        li      3, 0
        b       MPReturn

# The operations on the registered block, whose pages Mac OS cannot write
# (the engine protects them, with owner OWNER_TABLES):
#   OP_OWNERS: r4 the first page, r5 the number of pages, r6 the owner
#     byte to set, r7 the byte it replaces (-1: any but OWNER_TABLES, so
#     that marking a range of the system heap leaves the tables'
#     protection alone). Pages past the table's end are left out. With
#     application VSIDs on, a page whose byte changes loses its PTE.
#   OP_MODE: r4 MODE_*.
#   OP_CURRENT: r4 the current application's owner (used without
#     CB_APPLZONE).
#   OP_SPACES (patch 5): r4 1 to give applications VSIDs of their own in
#     the segments of mask r5 (bit n: segment n; 0-5 only), 0 to stop.
#   OP_SWITCH: r4 a zone (ApplZone): load the VSIDs of the application
#     that owns it, or the system's.
#   OP_FLUSH: r4 the first page, r5 the number of pages: drop their PTEs.
#   OP_PATCH: r4 a word's logical address in the blue task's space, r5 the
#     word it must hold, r6 the word to store. The kernel stores it through
#     the physical address and makes it the code the CPU runs; the page's
#     PTE does not record the change, so to the Virtual Memory Manager a
#     file-mapped code page stays clean. The page must have a PTE (read it
#     first) and should be held, or it can be read in again without the
#     change. paramErr when the word differs or there is no PTE.
#   OP_PREEMPT (patch 6): r4 the time slice in timebase ticks (0: off), r5
#     the yield stub's entry. The slice starts again now.
#   OP_YIELD: from the yield stub. Returns the PC the redirect replaced
#     (0 when none is pending, and none is pending after it), and allows
#     the next redirect r4 ticks from now (0: one slice).
#   OP_FRESH: r4 an owner byte, given to a new application: its
#     CB_YIELDED byte back to 0.
#   OP_RESUME: r4 a register block (RB_*, 64-byte aligned) in the blue
#     task's space. Returns to its PC with its LR, CTR, CR, XER, r0, r1, r3
#     and r4, and every other register as at the call. It needs no block
#     registered: a stub that called WaitNextEvent can come back after the
#     engine stopped. paramErr when the block's page has no PTE (store into
#     it and call again).
enf_op:
        cmpwi   3, OP_RESUME
        beq     eo_resume
        bl      1f
1:      mflr    16
        lwz     31, enf_cb - 1b(16)
        cmpwi   31, 0
        beq     eo_bad                          # no block
        cmpwi   3, OP_MODE
        beq     eo_mode
        cmpwi   3, OP_CURRENT
        beq     eo_current
        cmpwi   3, OP_OWNERS
        beq     eo_owners
        cmpwi   3, OP_SPACES
        beq     eo_spaces
        cmpwi   3, OP_SWITCH
        beq     eo_switch
        cmpwi   3, OP_FLUSH
        beq     eo_flush
        cmpwi   3, OP_PATCH
        beq     eo_patch
        cmpwi   3, OP_PREEMPT
        beq     eo_preempt
        cmpwi   3, OP_YIELD
        beq     eo_yield
        cmpwi   3, OP_FRESH
        beq     eo_fresh
        cmpwi   3, OP_SPARSE
        beq     eo_sparse
        cmpwi   3, OP_POOL_PUT
        beq     eo_pool_put
        cmpwi   3, OP_POOL_GET
        beq     eo_pool_get
        cmpwi   3, OP_SPARSE_FREE
        beq     eo_sparse_free
        cmpwi   3, OP_SPARSE_INFO
        beq     eo_sparse_info
eo_bad: li      3, -50
        b       MPReturn
eo_ok:  li      3, 0
        b       MPReturn

eo_mode:
        cmplwi  4, MODE_ENFORCE
        bgt     eo_bad
        stw     4, CB_MODE(31)
        b       eo_ok

eo_current:
        stw     4, CB_CURRENT(31)
        b       eo_ok

eo_owners:
        lwz     24, CTX_R6(6)                   # the owner byte
        lwz     25, CTX_R7(6)                   # the byte it replaces, or -1
        mr      22, 4                           # the page
        mr      23, 5                           # pages left
        lwz     17, CB_ENTRIES(31)
        cmplw   22, 17
        bge     eo_ok                           # past the table
        subf    17, 22, 17                      # pages left in the table
        cmplw   23, 17
        ble     1f
        mr      23, 17
1:      cmpwi   23, 0
        beq     eo_ok
        lwz     26, CB_SPACES(31)
        cmpwi   26, 0
        beq     2f
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
2:      rlwinm  17, 22, 22, 10, 29              # (page >> 12) * 4
        addi    17, 17, CB_PAGES
        lwzx    27, 31, 17                      # that table page, physical
        rlwinm  28, 22, 0, 20, 31               # page & 0xFFF
        lbzx    29, 27, 28
        cmpw    29, 25
        beq     3f                              # the byte to replace
        cmpwi   25, -1
        bne     4f                              # not the byte to replace
        cmpwi   29, OWNER_TABLES
        beq     4f                              # any byte but the tables'
3:      cmpw    29, 24
        beq     4f                              # no change
        stbx    24, 27, 28
        cmpwi   26, 0
        beq     4f
        slwi    8, 22, 12
        bl      flush_page                      # built again for its owner
4:      addi    22, 22, 1
        addic.  23, 23, -1
        bne     2b
        cmpwi   26, 0
        beq     eo_ok
        li      9, 0
        stw     9, KDP_HTABLock(1)
        b       eo_ok

eo_spaces:
        cmpwi   4, 0
        bne     1f
        lwz     9, CB_SPACES(31)
        cmpwi   9, 0
        beq     eo_ok
        bl      spaces_off
        b       eo_ok
1:      addi    8, 1, KDP_HTABLock
        bl      AcquireLock
        li      8, 0
        bl      space_switch                    # an old mask's segments back
        bl      2f                              # to the system's VSIDs
2:      mflr    16
        rlwinm  5, 5, 0, 26, 31                 # segments 0-5 only
        stw     5, vsid_mask - 2b(16)
        mfsprg  17, 0
        lwz     17, EWA_PA_CurAddressSpace(17)  # the caller's, the blue task's
        stw     17, sys_space - 2b(16)
        li      9, 1
        stw     9, CB_SPACES(31)
        bl      flush_owned                     # built again by owner
        li      9, 0
        stw     9, KDP_HTABLock(1)
        b       eo_ok

eo_switch:
        mr      8, 4
        bl      slice_new                       # patch 6
        lwz     9, CB_SPACES(31)
        cmpwi   9, 0
        beq     eo_ok
        mr      8, 4
        bl      code_of
        cmplwi  8, OWNER_APP
        bge     1f
        li      8, 0                            # not an application's zone
1:      bl      2f
2:      mflr    16
        lwz     9, vsid_cur - 2b(16)
        cmpw    8, 9
        beq     eo_ok                           # loaded already
        bl      space_switch
        li      8, CB_VSWITCH
        bl      count
        b       eo_ok

eo_flush:
        mr      22, 4
        mr      23, 5
        cmpwi   23, 0
        beq     eo_ok
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
1:      slwi    8, 22, 12
        bl      flush_page
        addi    22, 22, 1
        addic.  23, 23, -1
        bne     1b
        li      9, 0
        stw     9, KDP_HTABLock(1)
        b       eo_ok

eo_patch:
        andi.   9, 4, 3
        bne     eo_bad
        lwz     24, CTX_R6(6)                   # the word to store
        mr      22, 4                           # the address
        mr      23, 5                           # the word it must hold
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
        mr      8, 22
        bl      phys_of
        li      9, 0
        stw     9, KDP_HTABLock(1)
        cmpwi   8, 0
        beq     eo_bad                          # no PTE
        lwz     9, 0(8)
        cmpw    9, 23
        bne     eo_bad                          # not the word expected
        stw     24, 0(8)
        dcbst   0, 8
        sync
        icbi    0, 8
        sync
        isync
        b       eo_ok

eo_preempt:
        cmpwi   4, 0
        beq     1f
        andi.   9, 5, 3
        bne     eo_bad                          # not an instruction
        cmpwi   5, 0
        beq     eo_bad
        stw     5, CB_STUB(31)
1:      stw     4, CB_SLICE(31)
        mftb    9
        add     9, 9, 4
        stw     9, CB_NEXT(31)
        b       eo_ok

eo_yield:
        lwz     3, CB_YIELD_PC(31)
        li      9, 0
        stw     9, CB_YIELD_PC(31)
        cmpwi   4, 0
        bne     1f
        lwz     4, CB_SLICE(31)
1:      mftb    9
        add     9, 9, 4
        stw     9, CB_NEXT(31)
        b       MPReturn

eo_fresh:
        cmplwi  4, 128
        bge     eo_bad
        addi    9, 4, CB_YIELDED
        li      17, 0
        stbx    17, 31, 9
        b       eo_ok

# Patch 7's operations.
eo_sparse:                                      # r4, r5: the range (0, 0: none)
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
        bl      1f
1:      mflr    16
        stw     4, sparse_lo - 1b(16)
        stw     5, sparse_hi - 1b(16)
        li      9, 0
        stw     9, KDP_HTABLock(1)
        b       eo_ok

eo_pool_put:                                    # r4: a physical page
        andi.   9, 4, 0xFFF
        bne     eo_bad
        cmpwi   4, 0
        beq     eo_bad
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
        bl      1f
1:      mflr    16
        lwz     17, pool_head - 1b(16)
        stw     17, 0(4)
        stw     4, pool_head - 1b(16)
        lwz     17, pool_count - 1b(16)
        addi    17, 17, 1
        stw     17, pool_count - 1b(16)
        li      9, 0
        stw     9, KDP_HTABLock(1)
        b       eo_ok

eo_pool_get:                                    # returns a page, or 0
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
        bl      1f
1:      mflr    16
        lwz     3, pool_head - 1b(16)
        cmpwi   3, 0
        beq     2f
        lwz     17, 0(3)
        stw     17, pool_head - 1b(16)
        lwz     17, pool_count - 1b(16)
        addi    17, 17, -1
        stw     17, pool_count - 1b(16)
2:      li      9, 0
        stw     9, KDP_HTABLock(1)
        b       MPReturn

eo_sparse_info:                                 # r4: 0 pages in the pool, 1
        bl      1f                              # pages given, 2 faults with
1:      mflr    16                              # the pool empty
        cmplwi  4, 2
        bgt     eo_bad
        slwi    9, 4, 2
        addi    9, 9, pool_count - 1b
        lwzx    3, 16, 9
        b       MPReturn

# r4 an address, r5 a number of pages: take the pages in the sparse range
# back into the pool (their PTEs dropped, their PLEs empty again). Returns
# how many it took.
eo_sparse_free:
        mr      22, 4
        mr      23, 5
        li      24, 0
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
1:      cmpwi   23, 0
        beq     7f
        bl      2f
2:      mflr    9
        lwz     16, sparse_lo - 2b(9)
        lwz     17, sparse_hi - 2b(9)
        cmplw   22, 16
        blt     6f
        cmplw   22, 17
        bge     6f
        mfsprg  8, 0
        lwz     8, EWA_PA_CurAddressSpace(8)
        mr      9, 22
        bl      FindAreaAbove                   # uses r16, r17
        lwz     16, Area_LogicalBase(8)
        lwz     17, Area_LogicalEnd(8)
        cmplw   22, 16
        blt     6f
        cmplw   22, 17
        bgt     6f
        lwz     17, Area_Flags(8)
        andi.   17, 17, 0x8000
        bne     6f                              # contiguous: no PLEs
        mr      25, 8                           # the area
        mr      31, 8
        mr      8, 22
        bl      SpaceGetPagePLE                 # uses r16-r18
        beq     6f
        lwz     16, 0(30)
        andi.   16, 16, PLE_InHTAB
        beq     3f
        bl      GetPTEFromPLE
        mr      8, 22
        bl      InvalPTE
        bl      DeletePTE                       # the page back in the PLE
3:      lwz     16, 0(30)
        andi.   17, 16, PLE_HasPage
        beq     6f
        rlwinm  19, 16, 0, 0, 19                # the physical page
        andi.   16, 16, 0x7E6
        stw     16, 0(30)
        bl      4f
4:      mflr    9
        lwz     17, pool_head - 4b(9)
        stw     17, 0(19)
        stw     19, pool_head - 4b(9)
        lwz     17, pool_count - 4b(9)
        addi    17, 17, 1
        stw     17, pool_count - 4b(9)
        lwz     17, Area_BytesMapped(25)
        addi    17, 17, -4096
        stw     17, Area_BytesMapped(25)
        addi    24, 24, 1
6:      addi    22, 22, 4096
        addi    23, 23, -1
        b       1b
7:      li      9, 0
        stw     9, KDP_HTABLock(1)
        mr      3, 24
        b       MPReturn

# r4 the block. Returns through MPReturn with r10 (the PC), r12 (LR) and
# r13 (CR) set, which SchExitInterrupt loads before its rfi, the caller's
# r0 in the ContextBlock and its r1 in the EWA, which it loads from there,
# and r3, r4, CTR and XER in place. r5 is the caller's: nothing here uses
# it.
eo_resume:
        andi.   9, 4, 63
        bne     eo_bad
        mr      22, 4
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
        mr      8, 22
        bl      phys_of
        li      9, 0
        stw     9, KDP_HTABLock(1)
        cmpwi   8, 0
        beq     eo_bad                          # no PTE
        lwz     10, RB_PC(8)
        lwz     12, RB_LR(8)
        lwz     9, RB_CTR(8)
        mtctr   9
        lwz     13, RB_CR(8)
        lwz     9, RB_XER(8)
        mtxer   9
        lwz     9, RB_R0(8)
        stw     9, CTX_R0(6)
        lwz     9, RB_R1(8)
        mfsprg  17, 0
        stw     9, EWA_R1(17)
        lwz     3, RB_R3(8)
        lwz     4, RB_R4(8)
        b       MPReturn

# phys_of: r8 a logical address in the blue task's space -> r8 its
# physical address, or 0 when its page has no PTE. The caller holds the
# HTAB lock. Keeps r22-r29, r31; uses r9, r14-r21, r30, cr0, cr1, cr5-7.
phys_of:
        mflr    20
        mr      21, 31
        mr      15, 8                           # the address
        bl      1f
1:      mflr    9
        lwz     8, sys_space - 1b(9)
        cmpwi   8, 0
        bne     2f
        mfsprg  8, 0                            # before OP_SPACES: the
        lwz     8, EWA_PA_CurAddressSpace(8)    # caller's
2:      mr      9, 15
        bl      FindAreaAbove                   # uses r16, r17
        lwz     16, Area_LogicalBase(8)
        lwz     17, Area_LogicalEnd(8)
        cmplw   15, 16
        blt     8f                              # in no area
        cmplw   15, 17
        bgt     8f
        lwz     17, Area_Flags(8)
        andi.   17, 17, 0x8000
        bne     8f                              # contiguous: no PLEs
        mr      31, 8
        mr      8, 15
        bl      SpaceGetPagePLE                 # uses r16-r18
        beq     8f
        lwz     16, 0(30)
        andi.   16, 16, PLE_InHTAB
        beq     8f                              # no PTE
        bl      GetPTEFromPLE                   # r17: its lower word
        rlwinm  8, 17, 0, 0, 19                 # the physical page
        rlwimi  8, 15, 0, 20, 31                # and the offset in it
        b       9f
8:      li      8, 0
9:      mr      31, 21
        mtlr    20
        blr

# spaces_off: the system's VSIDs again, CB_SPACES 0, and every application
# page's PTE dropped, to be built again from its PLE. r31 the block; uses
# r8, r9, r14-r30.
spaces_off:
        mflr    26
        addi    8, 1, KDP_HTABLock
        bl      AcquireLock
        li      8, 0
        bl      space_switch
        li      9, 0
        stw     9, CB_SPACES(31)
        bl      flush_owned
        li      9, 0
        stw     9, KDP_HTABLock(1)
        mtlr    26
        blr

# Patch 6: preemption at safe points.
#
# Mac OS switches applications only when the current one calls
# WaitNextEvent (or GetNextEvent, EventAvail). An application that
# computes for a long time without calling it keeps every other one
# stopped. With OP_PREEMPT, an application's own native code that runs
# past its slice is sent to the engine's yield stub, which calls
# WaitNextEvent for it when that is safe, then lets it go on.
#
# The redirect: at 0x146D0 the scheduler is about to move the blue task
# from native code (its alternate context, r6, physical, which holds the
# code's registers) to the 68k emulator, to run a 68k interrupt. When the
# slice is over (CB_NEXT), the code runs in user mode with the 68k
# interrupt mask at 0, and the PC is in the current application's own
# code, the patch puts the stub's address in the context's PC and keeps
# the PC in CB_YIELD_PC: after the interrupt the code goes on in the stub.
# One redirect is pending at a time, until the stub takes the PC with
# OP_YIELD; the stub returns to it with OP_RESUME. The stub decides what
# the kernel cannot see: task level, and no Toolbox call in progress.
#
# A slice starts when ApplZone changes (slice_new, at OP_SWITCH and at a
# lazy switch) and at OP_YIELD. The application ApplZone leaves gets its
# CB_YIELDED byte: it has called WaitNextEvent (or the like) itself, so it
# has set up the Toolbox, and the stub can call it for it.
#
# At the site: r1 the KDP, r6 the alternate context, r7 the flags, r8 the
# alternate context's flags (live), r19 the EWA, r27 the task's flags, r29
# PSA.MCR, r30 the task (live). r9, r14-r18, r20-r26, r28, r31, CR and LR
# are free: the code after the site sets them before it uses them.
preempt:
        mflr    24
        mr      28, 8
        bl      1f
1:      mflr    20
        lwz     31, enf_cb - 1b(20)
        cmpwi   31, 0
        beq     9f                              # no block
        lwz     9, CB_SLICE(31)
        cmpwi   9, 0
        beq     9f                              # preemption off
        lwz     9, CB_YIELD_PC(31)
        cmpwi   9, 0
        bne     9f                              # one is pending
        lwz     9, CTX_MSR(6)
        andi.   9, 9, MSR_PR
        beq     9f                              # supervisor mode
        lwz     9, KDP_PA_ECB(1)                # the 68k interrupt mask, as
        lwz     9, ECB_R25(9)                   # the emulator left it
        andi.   9, 9, 7
        bne     9f                              # interrupt time
        mftb    9
        lwz     21, CB_NEXT(31)
        subf.   9, 21, 9
        blt     9f                              # inside the slice
        lwz     9, CB_APPLZONE(31)              # the current application
        cmpwi   9, 0
        beq     9f
        lhz     8, 0(9)
        lhz     9, 2(9)
        rlwimi  9, 8, 16, 0, 15
        mr      8, 9
        bl      code_of
        cmplwi  8, OWNER_APP
        blt     9f                              # none
        mr      22, 8
        lwz     23, CTX_CODEPTR(6)
        mr      8, 23
        bl      code_of
        cmpw    8, 22
        bne     9f                              # not its own code
        stw     23, CB_YIELD_PC(31)
        lwz     9, CB_STUB(31)
        stw     9, CTX_CODEPTR(6)
        li      8, CB_PREEMPTS
        bl      count
9:      mr      8, 28
        lwz     6, KDP_PA_ECB(1)                # the replaced instruction
        mtlr    24
        blr

# slice_new: r8 a zone (ApplZone). When it is not the zone of the last
# slice, a slice starts now, and the application of that last zone has
# given up the processor. r31 the block; uses r8, r9, r16, r17.
slice_new:
        lwz     9, CB_ZONE(31)
        cmpw    8, 9
        beqlr
        stw     8, CB_ZONE(31)
        lwz     17, CB_SLICE(31)
        mftb    16
        add     16, 16, 17
        stw     16, CB_NEXT(31)
        mflr    16
        mr      8, 9
        bl      code_of
        mtlr    16
        cmplwi  8, OWNER_APP
        bltlr
        addi    8, 8, CB_YIELDED
        li      9, 1
        stbx    9, 31, 8
        blr

        .globl  task_log                        # for build.sh's nm
task_log:
        .long   0, 0, 0, 0                      # opens, 0, 0, 0
        .rept   16
        .long   0, 0, 0, 0                      # pc, address, task, PTE
        .endr
        .globl  enf_cb
enf_cb: .long   0                               # the control block, physical
vsid_mask:
        .long   0                               # patch 5: segments with
                                                # application VSIDs
sys_space:
        .long   0                               # the blue task's address
                                                # space, physical
vsid_cur:
        .long   0                               # the owner whose VSIDs it
                                                # holds (0: the system's)
sparse_lo:
        .long   0                               # patch 7: the sparse range,
sparse_hi:
        .long   0
pool_head:
        .long   0                               # its free pages (physical),
pool_count:
        .long   0                               # how many (OP_SPARSE_INFO 0),
sparse_fills:
        .long   0                               # pages given (1),
sparse_empty:
        .long   0                               # faults with none to give (2)
