# hnat: catch writes to protected pages made by native PowerPC code, before
# the Exception Manager sees them.
#
# The NanoKernel hands exceptions in the blue task's native context to the
# handler address in the native context block (NCB, 0x68FFF400) at +0x4C,
# normally the ROM Exception Manager entry 0xFFCF0F28. That entry saves every
# register in a frame on the interrupted stack, so a fault on a protected
# stack page faults again inside it, which is fatal. This hook goes in front
# of it and never touches the stack. For a write violation (exception code
# 21) on a page it manages (inside [lo, hi) and, with a bitmap, with the
# page's bit set), it makes the page writable with MPSetAreaAccess and
# returns to the kernel with r3 = 0, so the store runs again. A write inside
# [cur_lo, cur_hi), the current application's own memory, is only counted;
# any other is recorded in a ring for the program that installed the hook.
# Anything else goes on to the original entry with every register as it
# arrived.
#
# Deny rule: when the faulting PC of a recorded store is inside [cur_lo,
# cur_hi), the current application's own code is writing outside its
# memory. The ring entry's pc gets bit 0 set. With D_ENFORCE set, the hook
# then leaves the page protected, stores ApplZone at D_STOP (when set) to
# name the application to stop, and passes the fault on to the Exception
# Manager.
#
# Entry state: as for h68k.s, with the NCB in r3 and NCB+0x54 (the
# Exception Manager's TVector) in r4. Fault details in the NCB: +0x40 flags
# (code in the top byte), +0x74 fault address, +0x84 PC, +0x8C LR,
# +0x94/+0x9C r3/r4.
#
# The code, its data block and its ring are in one system-heap block. The
# data block's address is patched into each `lis r4,0` / `addi r4,r4,0`
# pair, and the original handler into `ba 0` (an absolute branch, which
# reaches 0xFFCF0F28 from anywhere). The data block has the same layout as
# h68k's (common/hook.h).

        .set    D_COUNT_ALL, 0x04
        .set    D_COUNT_HANDLED, 0x08
        .set    D_LO, 0x0C
        .set    D_HI, 0x10
        .set    D_AREA, 0x14
        .set    D_KSTATUS, 0x1C
        .set    D_RING_IDX, 0x20
        .set    D_CR, 0x24
        .set    D_R0, 0x28
        .set    D_R5, 0x2C
        .set    D_R6, 0x30
        .set    D_R7, 0x34
        .set    D_CTX40, 0x38           # six saved NCB words, 0x38-0x4C
        .set    D_COUNT_PASSED, 0x50
        .set    D_PASSED, 0x54          # the last one: pc, addr, r3, zone, sp
        .set    D_BITMAP, 0x68
        .set    D_CUR_LO, 0x70
        .set    D_CUR_HI, 0x74
        .set    D_RING, 0x78
        .set    D_COUNT_SELF, 0x7C
        .set    D_CTX, 0x80             # the NCB, kept across the kernel call
        .set    D_ENFORCE, 0x88         # nonzero: deny the current app's code
        .set    D_COUNT_DENIED, 0x8C
        .set    D_DENIED, 0x90          # the last one: pc, addr, r3, zone, sp
        .set    D_STOP, 0xA4            # gets the zone on a denial, or 0
        .set    RING_N, 64
        .set    APPCODE, 6              # CR bit (cr1.eq): the current app's code

        .set    R_SEQ, 0x00
        .set    R_PC, 0x04
        .set    R_ADDR, 0x08
        .set    R_R3, 0x0C              # the interrupted r3
        .set    R_LR, 0x10
        .set    R_ZONE, 0x14
        .set    R_R4, 0x18              # the interrupted r4
        .set    R_SP, 0x1C              # the interrupted stack pointer
        .set    P_PC, 0x00
        .set    P_ADDR, 0x04
        .set    P_R3, 0x08
        .set    P_ZONE, 0x0C
        .set    P_SP, 0x10

        .text
        .globl  hnat_start
hnat_start:
        lis     4,0                     # patched: data block, high half
        addi    4,4,0                   # patched: data block, low half
        stw     5,D_R5(4)
        mfcr    5
        stw     5,D_CR(4)
        lwz     5,D_COUNT_ALL(4)
        addi    5,5,1
        stw     5,D_COUNT_ALL(4)

        lwz     5,0x40(3)               # exception code
        srwi    5,5,24
        cmpwi   5,21                    # ecDataWriteViolation
        bne     chain

        stw     0,D_R0(4)
        stw     6,D_R6(4)
        stw     7,D_R7(4)
        lwz     7,0x74(3)               # r7 = fault address
        lwz     5,D_LO(4)
        cmplw   7,5
        blt     not_ours
        lwz     6,D_HI(4)
        cmplw   7,6
        bge     not_ours

        lwz     6,D_BITMAP(4)
        cmpwi   6,0
        beq     handle
        subf    5,5,7                   # addr - lo
        srwi    5,5,12                  # page index
        srwi    0,5,3
        lbzx    0,6,0                   # its byte
        rlwinm  5,5,0,29,31             # its bit, 0 = most significant
        slw     0,0,5
        andi.   0,0,0x80
        beq     not_ours

handle:
        lwz     5,D_CUR_LO(4)
        cmplw   7,5
        blt     record
        lwz     5,D_CUR_HI(4)
        cmplw   7,5
        bge     record
        lwz     5,D_COUNT_SELF(4)
        addi    5,5,1
        stw     5,D_COUNT_SELF(4)
        b       open

record:
        # Deny rule: is the faulting PC inside the current range? The answer
        # is kept in CR bit APPCODE, which the exit paths restore from D_CR
        # with the rest of CR.
        crxor   APPCODE,APPCODE,APPCODE
        lwz     6,0x84(3)               # PC
        lwz     5,D_CUR_LO(4)
        cmplw   6,5
        blt     app_known
        lwz     5,D_CUR_HI(4)
        cmplw   6,5
        bge     app_known
        creqv   APPCODE,APPCODE,APPCODE
app_known:
        lwz     5,D_COUNT_HANDLED(4)
        addi    5,5,1
        stw     5,D_COUNT_HANDLED(4)
        lwz     6,D_RING(4)
        cmpwi   6,0
        beq     decide
        lwz     0,D_RING_IDX(4)
        slwi    0,0,5
        add     6,6,0                   # r6 = ring entry
        srwi    0,0,5
        nor     0,0,0                   # r0 + 1 as -(~r0): `addi 0,0,1`
        neg     0,0                     # would read r0 as 0
        andi.   0,0,RING_N-1
        stw     0,D_RING_IDX(4)
        stw     5,R_SEQ(6)
        lwz     5,0x84(3)
        bne     1,pc_known              # cr1.eq clear: not the app's code
        ori     5,5,1
pc_known:
        stw     5,R_PC(6)
        stw     7,R_ADDR(6)
        lwz     5,0x94(3)
        stw     5,R_R3(6)
        lwz     5,0x8C(3)
        stw     5,R_LR(6)
        lhz     5,0x2AA(0)              # ApplZone
        lhz     0,0x2AC(0)
        rlwimi  0,5,16,0,15
        stw     0,R_ZONE(6)
        lwz     5,0x9C(3)
        stw     5,R_R4(6)
        stw     1,R_SP(6)

decide:
        bne     1,open
        lwz     5,D_ENFORCE(4)
        cmpwi   5,0
        bne     deny

open:
        # The kernel call below goes through the same context block.
        lwz     5,0x40(3)
        stw     5,D_CTX40+0x00(4)
        lwz     5,0x44(3)
        stw     5,D_CTX40+0x04(4)
        lwz     5,0x84(3)
        stw     5,D_CTX40+0x08(4)
        lwz     5,0x8C(3)
        stw     5,D_CTX40+0x0C(4)
        lwz     5,0x94(3)
        stw     5,D_CTX40+0x10(4)
        lwz     5,0x9C(3)
        stw     5,D_CTX40+0x14(4)
        stw     3,D_CTX(4)

        # MPSetAreaAccess(area, write, write, page, 4096)
        rlwinm  6,7,0,0,19
        li      7,4096
        lwz     3,D_AREA(4)
        li      4,2
        li      5,2
        li      0,76
        sc
        lis     4,0                     # patched: data block, high half
        addi    4,4,0                   # patched: data block, low half
        stw     3,D_KSTATUS(4)
        lwz     3,D_CTX(4)

        lwz     5,D_CTX40+0x00(4)
        stw     5,0x40(3)
        lwz     5,D_CTX40+0x04(4)
        stw     5,0x44(3)
        lwz     5,D_CTX40+0x08(4)
        stw     5,0x84(3)
        lwz     5,D_CTX40+0x0C(4)
        stw     5,0x8C(3)
        lwz     5,D_CTX40+0x10(4)
        stw     5,0x94(3)
        lwz     5,D_CTX40+0x14(4)
        stw     5,0x9C(3)

        lwz     0,D_R0(4)
        lwz     6,D_R6(4)
        lwz     7,D_R7(4)
        lwz     5,D_CR(4)
        mtcrf   0xFF,5
        lwz     5,D_R5(4)
        li      3,0
        blr                             # ReturnFromException: retry the store

deny:
        # The page stays protected; the Exception Manager handles the fault.
        stw     7,D_DENIED+P_ADDR(4)
        lwz     5,0x84(3)
        stw     5,D_DENIED+P_PC(4)
        lwz     5,0x94(3)
        stw     5,D_DENIED+P_R3(4)
        stw     1,D_DENIED+P_SP(4)
        lhz     5,0x2AA(0)
        lhz     0,0x2AC(0)
        rlwimi  0,5,16,0,15
        stw     0,D_DENIED+P_ZONE(4)
        lwz     5,D_COUNT_DENIED(4)
        addi    5,5,1
        stw     5,D_COUNT_DENIED(4)
        lwz     5,D_STOP(4)             # name the application to stop
        cmpwi   5,0
        beq     deny_out
        stw     0,0(5)
deny_out:
        lwz     0,D_R0(4)
        lwz     6,D_R6(4)
        lwz     7,D_R7(4)
        b       chain

not_ours:
        stw     7,D_PASSED+P_ADDR(4)
        lwz     5,0x84(3)
        stw     5,D_PASSED+P_PC(4)
        lwz     5,0x94(3)
        stw     5,D_PASSED+P_R3(4)
        stw     1,D_PASSED+P_SP(4)
        lhz     5,0x2AA(0)
        lhz     0,0x2AC(0)
        rlwimi  0,5,16,0,15
        stw     0,D_PASSED+P_ZONE(4)
        lwz     5,D_COUNT_PASSED(4)
        addi    5,5,1
        stw     5,D_COUNT_PASSED(4)
        lwz     0,D_R0(4)
        lwz     6,D_R6(4)
        lwz     7,D_R7(4)
chain:
        lwz     5,D_CR(4)
        mtcrf   0xFF,5
        lwz     5,D_R5(4)
        lwz     4,0x54(3)
        .globl  hnat_branch
hnat_branch:
        ba      0                       # patched: original handler
        .globl  hnat_end
hnat_end:
