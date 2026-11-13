# The NanoKernel the test machine boots: the stock NanoKernel 2.28
# (kernel/stock/NanoKernel-v02.28.bin, from kernel/extract.sh) with the
# patches below. kernel/build.sh assembles it.
#
# `stock N` copies the stock kernel's bytes from the current offset up to
# offset N; each patch then assembles its instructions at its site. Labels
# are offsets in the kernel, so branches between stock and added code
# resolve here. Added code goes after the stock kernel (STOCK_END). The
# kernel must end by KERNEL_LIMIT: the 68k emulator follows it in the ROM
# image. Names in comments are from os9-protect's
# refs/powermac-rom/NanoKernel.

        .set    STOCK_END, 0x19B40
        .set    KERNEL_LIMIT, 0x50000

        .text
kernel:

        .macro  stock to
        .incbin "NanoKernel-v02.28.bin", . - kernel, \to - (. - kernel)
        .endm

        .set    FDP_011c, kernel + 0x591C       # end of an emulated access

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
# r20 is free here (only 8-byte stores use it) and survives a page fault
# taken inside the emulation (MemRetryDSI).

        stock   0x5EC8
        b       stwcx_probe                     # was: isync

        stock   STOCK_END

stwcx_probe:
        beq+    1f                              # the store happened
        lwarx   20,23,19
        stwcx.  20,23,19                        # faults if write-protected
        crxor   2,2,2                           # report failure (cr0.eq = 0)
1:      isync
        mfcr    23                              # as FDP_06c0: cr0 into the
        rlwimi  13,23,0,0,3                     # caller's saved CR
        b       FDP_011c

        .if     . - kernel > KERNEL_LIMIT
        .error  "the kernel runs into the 68k emulator"
        .endif
