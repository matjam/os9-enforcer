# h68k: catch writes to protected pages made by 68k code.
#
# The NanoKernel hands exceptions in the 68k emulator's context to the
# handler address in the emulator context block (ECB, 0x68FFF100) at +0x4C,
# normally the emulator's own entry 0x6806D440, which turns a write fault
# into a 68k bus error. This code replaces that entry. For a write
# violation (exception code 21) inside the range set in the data block, it
# makes the page writable with MPSetAreaAccess, records the fault, and
# returns to the kernel with r3 = 0, so the store runs again. Anything else
# goes on to the original entry with every register as it arrived.
#
# Entry state, from the kernel (NKInterrupts.s LetBlueHandleOwnException):
#   r3  = ECB (logical)          free: the original overwrites it first
#   r4  = ECB+0x54               reloaded from there before chaining
#   LR  = ReturnFromException trap; `blr` with r3 = 0 resumes the fault
#   all other registers, CR and CTR = the interrupted emulator's, and must
#   be the same at `blr`, because the trap resumes with live registers
#
# Fault details the kernel saved in the ECB: +0x40 flags (code in the top
# byte), +0x74 fault address, +0x84 PC of the faulting instruction,
# +0x8C LR, +0x94/+0x9C r3/r4.
#
# The emulator is a threaded interpreter: r24 is its 68k instruction
# pointer, and each opcode handler fetches the next opcode (and one more
# word) with `lhau rX,2(r24)` before its own store. At a faulting store,
# r24 is the address of the next 68k instruction + 2. r1 is the 68k A7, and
# r4 usually holds the value being stored.
#
# The code is copied to 0x68FFFD00 and the data block lives at 0x68FFFB80
# (unused space in the emulator data page, within branch range of
# 0x6806D440). Data offsets are below; keep common/h68k.h in step.

        .set    BASE_FROM_ECB, 0xA80    # data block = ECB + 0xA80
        .set    E, -BASE_FROM_ECB       # ECB field X is at X+E(base)

        .set    D_COUNT_ALL, 0x04
        .set    D_COUNT_HANDLED, 0x08
        .set    D_LO, 0x0C
        .set    D_HI, 0x10              # 0 = handle nothing
        .set    D_AREA, 0x14
        .set    D_KSTATUS, 0x1C         # last MPSetAreaAccess status
        .set    D_RING_IDX, 0x20
        .set    D_CR, 0x24
        .set    D_R0, 0x28
        .set    D_R5, 0x2C
        .set    D_R6, 0x30
        .set    D_R7, 0x34
        .set    D_ECB40, 0x38           # six saved ECB words, 0x38-0x4C
        .set    D_COUNT_PASSED, 0x50    # write faults outside the range
        .set    D_PASSED, 0x54          # the last one: pc, addr, r24, zone, a7
        .set    D_BITMAP, 0x68          # page bitmap, or 0 for the whole range
        .set    D_RING, 0x80            # RING_N entries of 32 bytes
        .set    RING_N, 8

        # Ring entry and passed-fault record fields.
        .set    R_SEQ, 0x00             # count_handled after this event
        .set    R_PC, 0x04              # emulator instruction that faulted
        .set    R_ADDR, 0x08            # fault address
        .set    R_R24, 0x0C             # next 68k instruction + 2
        .set    R_LR, 0x10
        .set    R_ZONE, 0x14            # ApplZone (lowmem 0x2AA)
        .set    R_R4, 0x18              # usually the value stored
        .set    R_A7, 0x1C              # 68k stack pointer
        .set    P_PC, 0x00
        .set    P_ADDR, 0x04
        .set    P_R24, 0x08
        .set    P_ZONE, 0x0C
        .set    P_A7, 0x10

        .text
        .globl  h68k_start
h68k_start:
        addi    3,3,BASE_FROM_ECB       # r3 = data block
        mfcr    4
        stw     4,D_CR(3)
        lwz     4,D_COUNT_ALL(3)
        addi    4,4,1
        stw     4,D_COUNT_ALL(3)

        lwz     4,0x40+E(3)             # exception code
        srwi    4,4,24
        cmpwi   4,21                    # ecDataWriteViolation
        bne     chain

        stw     5,D_R5(3)
        lwz     4,0x74+E(3)             # fault address
        lwz     5,D_LO(3)
        cmplw   4,5
        blt     chain_write
        lwz     5,D_HI(3)
        cmplw   4,5
        bge     chain_write

        stw     0,D_R0(3)
        stw     6,D_R6(3)
        stw     7,D_R7(3)

        # With a bitmap, handle the page only if its bit is set: bit
        # (addr - lo) >> 12, most significant bit of each byte first.
        lwz     5,D_BITMAP(3)
        cmpwi   5,0
        beq     handle
        lwz     6,D_LO(3)
        subf    6,6,4
        srwi    6,6,12                  # page index
        srwi    7,6,3
        lbzx    7,5,7                   # its byte
        rlwinm  6,6,0,29,31             # its bit, 0 = most significant
        slw     7,7,6
        andi.   7,7,0x80
        bne     handle
        lwz     0,D_R0(3)
        lwz     6,D_R6(3)
        lwz     7,D_R7(3)
        b       chain_write

handle:
        # A write to a page we protected.

        # The kernel call below goes through the same context block; keep
        # the exception words it might touch.
        lwz     5,0x40+E(3)
        stw     5,D_ECB40+0x00(3)
        lwz     5,0x44+E(3)
        stw     5,D_ECB40+0x04(3)
        lwz     5,0x84+E(3)
        stw     5,D_ECB40+0x08(3)
        lwz     5,0x8C+E(3)
        stw     5,D_ECB40+0x0C(3)
        lwz     5,0x94+E(3)
        stw     5,D_ECB40+0x10(3)
        lwz     5,0x9C+E(3)
        stw     5,D_ECB40+0x14(3)

        # Ring entry (see R_ fields).
        lwz     5,D_COUNT_HANDLED(3)
        addi    5,5,1
        stw     5,D_COUNT_HANDLED(3)
        lwz     6,D_RING_IDX(3)
        addi    7,6,1
        andi.   7,7,RING_N-1
        stw     7,D_RING_IDX(3)
        slwi    6,6,5
        add     6,6,3                   # r6 = data block + 32 * index
        stw     5,D_RING+R_SEQ(6)
        lwz     7,0x84+E(3)
        stw     7,D_RING+R_PC(6)
        stw     4,D_RING+R_ADDR(6)
        stw     24,D_RING+R_R24(6)
        lwz     7,0x8C+E(3)
        stw     7,D_RING+R_LR(6)
        lhz     7,0x2AA(0)              # ApplZone is 2-byte aligned
        lhz     5,0x2AC(0)
        rlwimi  5,7,16,0,15
        stw     5,D_RING+R_ZONE(6)
        lwz     7,0x9C+E(3)
        stw     7,D_RING+R_R4(6)
        stw     1,D_RING+R_A7(6)

        # MPSetAreaAccess(area, write, write, page, 4096)
        rlwinm  6,4,0,0,19
        li      7,4096
        li      4,2
        li      5,2
        li      0,76
        lwz     3,D_AREA(3)
        sc
        lis     4,0x6900
        addi    4,4,-0x480              # data block, 0x68FFFB80
        stw     3,D_KSTATUS(4)
        mr      3,4

        lwz     5,D_ECB40+0x00(3)
        stw     5,0x40+E(3)
        lwz     5,D_ECB40+0x04(3)
        stw     5,0x44+E(3)
        lwz     5,D_ECB40+0x08(3)
        stw     5,0x84+E(3)
        lwz     5,D_ECB40+0x0C(3)
        stw     5,0x8C+E(3)
        lwz     5,D_ECB40+0x10(3)
        stw     5,0x94+E(3)
        lwz     5,D_ECB40+0x14(3)
        stw     5,0x9C+E(3)

        lwz     0,D_R0(3)
        lwz     5,D_R5(3)
        lwz     6,D_R6(3)
        lwz     7,D_R7(3)
        lwz     4,D_CR(3)
        mtcrf   0xFF,4
        li      3,0
        blr                             # ReturnFromException: retry the store

chain_write:
        # Keep the details: this fault is about to kill the application,
        # or hang the machine if the emulator cannot take a bus error now.
        stw     4,D_PASSED+P_ADDR(3)
        lwz     4,0x84+E(3)
        stw     4,D_PASSED+P_PC(3)
        stw     24,D_PASSED+P_R24(3)
        stw     1,D_PASSED+P_A7(3)
        lhz     4,0x2AA(0)
        lhz     5,0x2AC(0)
        rlwimi  5,4,16,0,15
        stw     5,D_PASSED+P_ZONE(3)
        lwz     5,D_R5(3)
        lwz     4,D_COUNT_PASSED(3)
        addi    4,4,1
        stw     4,D_COUNT_PASSED(3)
chain:
        lwz     4,D_CR(3)
        mtcrf   0xFF,4
        lwz     4,0x54+E(3)
        addi    3,3,-BASE_FROM_ECB
        .globl  h68k_branch
h68k_branch:
        b       .                       # patched: original handler
        .globl  h68k_end
h68k_end:
