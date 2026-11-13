# h68k: catch writes to protected pages made by 68k code.
#
# The NanoKernel hands exceptions in the 68k emulator's context to the
# handler address in the emulator context block (ECB, 0x68FFF100) at +0x4C,
# normally the emulator's own entry 0x6806D440, which turns a write fault
# into a 68k bus error (or hangs, when it cannot take one). This code
# replaces that entry. For a write violation (exception code 21) on a page
# it manages (inside [lo, hi) and, with a bitmap, with the page's bit set),
# it makes the page writable with MPSetAreaAccess and returns to the kernel
# with r3 = 0, so the store runs again. A write inside [cur_lo, cur_hi), the
# current application's own memory, is only counted; any other is recorded
# in a ring for the program that installed the hook. Anything else goes on
# to the original entry with every register as it arrived. The hook never
# touches the stack.
#
# Deny rule: when the 68k code doing a recorded store (the next 68k
# instruction, r24 - 2) is inside [cur_lo, cur_hi), the current
# application's own code is writing outside its memory. The ring entry's pc
# gets bit 0 set. When the fault address is at or above D_DENY_FROM, the
# hook then leaves the page protected, stores ApplZone at D_STOP (when set)
# to name the application to stop, and passes the fault on, and the
# emulator gives the application a bus error. A store by the emulator's own
# code that fills a 68k-to-native call frame (D_FRAME, FRAME_LEN bytes) is
# not the application's, although r24 then still points at its trap. A
# recorded store to the page in the word at D_KDENY (the page the kernel's
# owner check denied, common/kcb.h) is denied in the same way, whatever its
# address, and the word cleared.
#
# Entry state, from the kernel (NKInterrupts.s LetBlueHandleOwnException):
#   r3  = ECB (logical)          free: the original overwrites it first
#   r4  = ECB+0x54               reloaded from there before chaining
#   LR  = ReturnFromException trap; `blr` with r3 = 0 resumes the fault
#   all other registers, CR and CTR = the interrupted emulator's, and must
#   be the same at `blr`, because the trap resumes with live registers
#
# CR is the exception: the hook changes cr0 and cr1 and puts back only
# those two fields. It runs with external interrupts enabled (the kernel
# clears MSR[EE] for handlers in the native context only), and the kernel's
# interrupt handler tells the emulator about a pending interrupt by setting
# bits in the interrupted code's CR (NanoKernel 0x15524, `or r13,r13,r31`,
# mask at KDP+0x674: 0x00E00000, in cr2, on the test machine). An
# interrupt taken inside the hook, most often right after its `sc`, sets
# those bits in the hook's CR. Putting back all of CR from D_CR cleared
# them: the emulator never ran the interrupt, so the kernel
# never sent the OpenPIC its end of interrupt, and every interrupt at that
# priority or below stopped (USB keyboard and mouse first, 2026-11-12).
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
# The data block is at 0x68FFFB80 and the code is copied to 0x68FFFC40, in
# unused space in the emulator data page, within branch range of 0x6806D440.
# The ring is in the system heap. Data offsets are below; keep common/h68k.h
# in step.

        .set    BASE_FROM_ECB, 0xA80    # data block = ECB + 0xA80
        .set    E, -BASE_FROM_ECB       # ECB field X is at X+E(base)

        .set    D_COUNT_ALL, 0x04
        .set    D_COUNT_HANDLED, 0x08   # recorded in the ring (= last seq)
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
        .set    D_COUNT_PASSED, 0x50    # write faults not handled
        .set    D_PASSED, 0x54          # the last one: pc, addr, r24, zone, a7
        .set    D_BITMAP, 0x68          # page bitmap, or 0 for the whole range
        .set    D_CUR_LO, 0x70          # the current application's pages:
        .set    D_CUR_HI, 0x74          # handled but not recorded
        .set    D_RING, 0x78            # RING_N entries of 32 bytes, or 0
        .set    D_COUNT_SELF, 0x7C      # handled inside [cur_lo, cur_hi)
        .set    D_DENY_FROM, 0x88       # deny the app's code at or above this
        .set    D_COUNT_DENIED, 0x8C
        .set    D_DENIED, 0x90          # the last one: pc, addr, r24, zone, a7
        .set    D_STOP, 0xA4            # gets the zone on a denial, or 0
        .set    D_FRAME, 0xA8           # the emulator's frame code, or 0
        .set    D_KDENY, 0xAC           # the kernel's denied page, or 0
        .set    FRAME_LEN, 0x70
        .set    RING_N, 256
        .set    APPCODE, 6              # CR bit (cr1.eq): the current app's code

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
        stw     0,D_R0(3)
        stw     6,D_R6(3)
        stw     7,D_R7(3)
        lwz     4,0x74+E(3)             # fault address
        lwz     5,D_LO(3)
        cmplw   4,5
        blt     pass
        lwz     6,D_HI(3)
        cmplw   4,6
        bge     pass

        # With a bitmap, handle the page only if its bit is set: bit
        # (addr - lo) >> 12, most significant bit of each byte first.
        lwz     6,D_BITMAP(3)
        cmpwi   6,0
        beq     handle
        subf    5,5,4                   # addr - lo
        srwi    5,5,12                  # page index
        srwi    7,5,3
        lbzx    7,6,7                   # its byte
        rlwinm  5,5,0,29,31             # its bit, 0 = most significant
        slw     7,7,5
        andi.   7,7,0x80
        beq     pass

handle:
        # The current application's own pages: count only.
        lwz     5,D_CUR_LO(3)
        cmplw   4,5
        blt     record
        lwz     5,D_CUR_HI(3)
        cmplw   4,5
        bge     record
        lwz     5,D_COUNT_SELF(3)
        addi    5,5,1
        stw     5,D_COUNT_SELF(3)
        b       open

record:
        # Deny rule: is the 68k code doing the store inside the current
        # range? The answer is kept in CR bit APPCODE, which the exit paths
        # restore from D_CR with the rest of cr0 and cr1.
        crxor   APPCODE,APPCODE,APPCODE
        addi    6,24,-2                 # the next 68k instruction
        lwz     5,D_CUR_LO(3)
        cmplw   6,5
        blt     app_known
        lwz     5,D_CUR_HI(3)
        cmplw   6,5
        bge     app_known
        lwz     5,0x84+E(3)             # not if the emulator's frame code
        lwz     7,D_FRAME(3)
        subf    5,7,5
        cmplwi  5,FRAME_LEN
        blt     app_known
        creqv   APPCODE,APPCODE,APPCODE
app_known:
        lwz     5,D_COUNT_HANDLED(3)
        addi    5,5,1
        stw     5,D_COUNT_HANDLED(3)
        lwz     7,D_RING(3)
        cmpwi   7,0
        beq     decide
        lwz     6,D_RING_IDX(3)
        slwi    0,6,5
        add     7,7,0                   # r7 = ring entry
        addi    6,6,1
        andi.   6,6,RING_N-1
        stw     6,D_RING_IDX(3)
        stw     5,R_SEQ(7)
        lwz     6,0x84+E(3)
        bne     1,pc_known              # cr1.eq clear: not the app's code
        ori     6,6,1
pc_known:
        stw     6,R_PC(7)
        stw     4,R_ADDR(7)
        stw     24,R_R24(7)
        lwz     6,0x8C+E(3)
        stw     6,R_LR(7)
        lhz     6,0x2AA(0)              # ApplZone is 2-byte aligned
        lhz     5,0x2AC(0)
        rlwimi  5,6,16,0,15
        stw     5,R_ZONE(7)
        lwz     6,0x9C+E(3)
        stw     6,R_R4(7)
        stw     1,R_A7(7)

decide:
        lwz     5,D_KDENY(3)            # the kernel's owner check denied
        cmpwi   5,0                     # this page: deny
        beq     by_rule
        lwz     6,0(5)
        rlwinm  7,4,0,0,19
        cmpw    6,7
        bne     by_rule
        li      6,0
        stw     6,0(5)
        b       deny
by_rule:
        bne     1,open
        lwz     5,D_DENY_FROM(3)
        cmplw   4,5
        bge     deny

open:
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
        mtcrf   0xC0,4                  # cr0 and cr1 only (see the top)
        li      3,0
        blr                             # ReturnFromException: retry the store

deny:
        # The page stays protected; the emulator turns the fault into a bus
        # error for the application.
        lwz     5,D_STOP(3)             # name the application to stop
        cmpwi   5,0
        beq     1f
        lhz     6,0x2AA(0)
        lhz     7,0x2AC(0)
        rlwimi  7,6,16,0,15
        stw     7,0(5)
1:      addi    5,3,D_COUNT_DENIED
        b       note

pass:
        # A write fault not handled. Keep the details: this fault is about
        # to kill the application, or hang the machine if the emulator
        # cannot take a bus error now.
        addi    5,3,D_COUNT_PASSED
note:
        # r5: a count with its HookPassed after it (D_COUNT_PASSED and
        # D_PASSED, or D_COUNT_DENIED and D_DENIED); r4: the address.
        stw     4,4+P_ADDR(5)
        lwz     4,0x84+E(3)
        stw     4,4+P_PC(5)
        stw     24,4+P_R24(5)
        stw     1,4+P_A7(5)
        lhz     6,0x2AA(0)
        lhz     7,0x2AC(0)
        rlwimi  7,6,16,0,15
        stw     7,4+P_ZONE(5)
        lwz     4,0(5)
        addi    4,4,1
        stw     4,0(5)
        lwz     0,D_R0(3)
        lwz     5,D_R5(3)
        lwz     6,D_R6(3)
        lwz     7,D_R7(3)
chain:
        lwz     4,D_CR(3)
        mtcrf   0xC0,4                  # cr0 and cr1 only (see the top)
        lwz     4,0x54+E(3)
        addi    3,3,-BASE_FROM_ECB
        .globl  h68k_branch
h68k_branch:
        b       .                       # patched: original handler
        .globl  h68k_end
h68k_end:
