# The NanoKernel the test machine boots: the stock NanoKernel 2.28
# (kernel/stock/NanoKernel-v02.28.bin, from kernel/extract.sh) with the
# patches below. kernel/build.sh assembles it.
#
# `stock N` copies the stock kernel's bytes from the current offset up to
# offset N; each patch then assembles its instructions at its site, so the
# sites must come in offset order. Labels are offsets in the kernel, so
# branches between stock and added code resolve here. Added code goes
# after the stock kernel (STOCK_END). The kernel must end by KERNEL_LIMIT:
# the 68k emulator follows it in the ROM image. Names in comments are from
# os9-protect's refs/powermac-rom/NanoKernel and Internal/NKOpaque.a.

        .set    STOCK_END, 0x19B40
        .set    KERNEL_LIMIT, 0x50000

        .text
kernel:

        .macro  stock to
        .incbin "NanoKernel-v02.28.bin", . - kernel, \to - (. - kernel)
        .endm

# Stock code the added code branches to.
        .set    FDP_011c, kernel + 0x591C       # end of an emulated access
        .set    SAA_pageloop, kernel + 0x10B68  # MPSetAreaAccess: @pageloop
        .set    SAA_done, kernel + 0x10BFC      # after the loop: unlock, return

# Kernel structures.
        .set    EWA_CoherenceGroup, -0x338      # EWA.CPUBase + CPU.LLL.Freeform
        .set    CG_LA_CpuPlugin, 0x28
        .set    CG_CpuPluginSize, 0x30
        .set    CG_CpuPluginSpacePtr, 0x4C
        .set    Area_PageSize, 0x68
        .set    Area_AddressSpacePtr, 0x6C
        .set    kAccessWrite, 0x02

# --- Patch sites, in offset order ------------------------------------------

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

        .if     . - kernel > KERNEL_LIMIT
        .error  "the kernel runs into the 68k emulator"
        .endif
