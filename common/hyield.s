# hyield: the yield stub of the kernel's preemption (kernel/nanokernel.s,
# patch 6).
#
# The kernel sends the current application's native code here when it has
# run past its slice: the code's PC is replaced with the stub's entry, at a
# 68k interrupt, and every register is as the code left it. The stub:
#   1. moves r1 below the code's red zone (224 bytes, which a leaf
#      function can use without a frame), aligns it to 64 bytes and saves
#      every volatile register: r0, r2-r31, LR, CTR, CR, XER, f0-f13 and
#      FPSCR, and with AltiVec VRSAVE, v0-v19 and VSCR (when VRSAVE is not
#      0);
#   2. takes the code's PC from the kernel (OP_YIELD);
#   3. calls the engine's check (the TVector at D_CHECK), which returns
#      whether the code can call WaitNextEvent now: task level, and only
#      the application's own code on its stack;
#   4. if so, calls WaitNextEvent(0, &event, 0, nil): no event is taken,
#      and the Process Manager can run other applications;
#   5. restores everything it can and returns to the PC with OP_RESUME,
#      which loads PC, LR, CTR, CR, XER, r0, r1, r3 and r4 from the
#      register block at F_RB.
# Steps 4 and 5 use only this block and system code, so a stub that comes
# back from WaitNextEvent after the engine stopped still returns. The
# engine never frees the block.
#
# The data is in front of the code, in this file's first 128 bytes
# (common/hyield.h, YieldData). The engine fills D_CHECK and D_WNE, and
# makes the branch at D_VECBR a nop when the CPU has AltiVec.

        .set    MP_ENFORCER, 109
        .set    OP_YIELD, 9
        .set    OP_RESUME, 10

        .set    D_MAGIC, 0x00
        .set    D_ENTRY, 0x04
        .set    D_VECBR, 0x08
        .set    D_CHECK, 0x0C
        .set    D_WNE, 0x10
        .set    D_ENTERED, 0x14
        .set    D_YIELDED, 0x18
        .set    D_LOST, 0x1C
        .set    D_SIZE, 0x80

        .set    SMALL, 240              # the red zone and 16 bytes
        .set    S_R12, 8                # in the small frame
        .set    BIG, 768                # the frame, 64-byte aligned:
        .set    F_RB, 64                # OP_RESUME's register block
        .set    RB_PC, F_RB + 0x00
        .set    RB_LR, F_RB + 0x04
        .set    RB_CTR, F_RB + 0x08
        .set    RB_CR, F_RB + 0x0C
        .set    RB_XER, F_RB + 0x10
        .set    RB_R0, F_RB + 0x14
        .set    RB_R1, F_RB + 0x18
        .set    RB_R3, F_RB + 0x1C
        .set    RB_R4, F_RB + 0x20
        .set    F_GPR, 128              # r2-r31
        .set    F_FPR, 248              # f0-f13
        .set    F_FPSCR, 360
        .set    F_VRSAVE, 368
        .set    F_VEC, 372              # 1: v0-v19 and VSCR saved
        .set    F_EVENT, 376            # WaitNextEvent's EventRecord
        .set    F_VR, 400               # v0-v19, then VSCR

        .text
        .globl  hyield_start
hyield_start:
        .long   0x594C4431              # 'YLD1'
        .long   entry - hyield_start
        .long   vecbr - hyield_start
        .space  D_SIZE - 12

entry:
        stwu    1,-SMALL(1)             # below the red zone
        stw     12,S_R12(1)
        clrlwi  12,1,26                 # r1 & 63
        subfic  12,12,-BIG
        stwux   1,1,12                  # aligned; back chain: the small frame
        stw     0,RB_R0(1)
        stw     3,RB_R3(1)
        stw     4,RB_R4(1)
        mflr    0
        stw     0,RB_LR(1)
        mfctr   0
        stw     0,RB_CTR(1)
        mfcr    0
        stw     0,RB_CR(1)
        mfxer   0
        stw     0,RB_XER(1)
        lwz     12,0(1)                 # the small frame
        addi    0,12,SMALL              # the code's r1
        stw     0,RB_R1(1)
        lwz     12,S_R12(12)            # the code's r12
        stmw    2,F_GPR(1)
        stfd    0,F_FPR+0(1)
        stfd    1,F_FPR+8(1)
        stfd    2,F_FPR+16(1)
        stfd    3,F_FPR+24(1)
        stfd    4,F_FPR+32(1)
        stfd    5,F_FPR+40(1)
        stfd    6,F_FPR+48(1)
        stfd    7,F_FPR+56(1)
        stfd    8,F_FPR+64(1)
        stfd    9,F_FPR+72(1)
        stfd    10,F_FPR+80(1)
        stfd    11,F_FPR+88(1)
        stfd    12,F_FPR+96(1)
        stfd    13,F_FPR+104(1)
        mffs    0
        stfd    0,F_FPSCR(1)
        li      0,0
        stw     0,F_VEC(1)
vecbr:  b       1f                      # a nop with AltiVec
        mfspr   0,256                   # VRSAVE
        stw     0,F_VRSAVE(1)
        cmpwi   0,0
        beq     1f                      # no vector register in use
        li      0,1
        stw     0,F_VEC(1)
        addi    3,1,F_VR
        li      4,0
        stvx    0,3,4
        li      4,16
        stvx    1,3,4
        li      4,32
        stvx    2,3,4
        li      4,48
        stvx    3,3,4
        li      4,64
        stvx    4,3,4
        li      4,80
        stvx    5,3,4
        li      4,96
        stvx    6,3,4
        li      4,112
        stvx    7,3,4
        li      4,128
        stvx    8,3,4
        li      4,144
        stvx    9,3,4
        li      4,160
        stvx    10,3,4
        li      4,176
        stvx    11,3,4
        li      4,192
        stvx    12,3,4
        li      4,208
        stvx    13,3,4
        li      4,224
        stvx    14,3,4
        li      4,240
        stvx    15,3,4
        li      4,256
        stvx    16,3,4
        li      4,272
        stvx    17,3,4
        li      4,288
        stvx    18,3,4
        li      4,304
        stvx    19,3,4
        mfvscr  0
        li      4,320
        stvx    0,3,4

1:      bcl     20,31,2f
2:      mflr    31                      # r31: the data, kept across calls
        addi    31,31,hyield_start - 2b
        lwz     3,D_ENTERED(31)
        addi    3,3,1
        stw     3,D_ENTERED(31)

        li      0,MP_ENFORCER           # the code's PC
        li      3,OP_YIELD
        li      4,0
        sc
        stw     3,RB_PC(1)
        andi.   0,3,3
        beq     3f
        lwz     3,D_LOST(31)            # no PC: the block was unregistered
        addi    3,3,1                   # between the redirect and here,
        stw     3,D_LOST(31)            # which cannot happen
        tw      31,0,0

3:      lwz     12,D_CHECK(31)          # check(sp, pc, lr, data)
        lwz     0,0(12)
        lwz     2,4(12)
        mtctr   0
        lwz     3,RB_R1(1)
        lwz     4,RB_PC(1)
        lwz     5,RB_LR(1)
        mr      6,31
        bctrl
        cmpwi   3,0
        beq     resume

        lwz     12,D_WNE(31)            # WaitNextEvent(0, &event, 0, nil)
        lwz     0,0(12)
        lwz     2,4(12)
        mtctr   0
        li      3,0
        addi    4,1,F_EVENT
        li      5,0
        li      6,0
        bctrl
        lwz     3,D_YIELDED(31)
        addi    3,3,1
        stw     3,D_YIELDED(31)

resume:
        lwz     0,F_VEC(1)
        cmpwi   0,0
        beq     4f
        addi    3,1,F_VR
        li      4,320
        lvx     0,3,4
        mtvscr  0
        li      4,0
        lvx     0,3,4
        li      4,16
        lvx     1,3,4
        li      4,32
        lvx     2,3,4
        li      4,48
        lvx     3,3,4
        li      4,64
        lvx     4,3,4
        li      4,80
        lvx     5,3,4
        li      4,96
        lvx     6,3,4
        li      4,112
        lvx     7,3,4
        li      4,128
        lvx     8,3,4
        li      4,144
        lvx     9,3,4
        li      4,160
        lvx     10,3,4
        li      4,176
        lvx     11,3,4
        li      4,192
        lvx     12,3,4
        li      4,208
        lvx     13,3,4
        li      4,224
        lvx     14,3,4
        li      4,240
        lvx     15,3,4
        li      4,256
        lvx     16,3,4
        li      4,272
        lvx     17,3,4
        li      4,288
        lvx     18,3,4
        li      4,304
        lvx     19,3,4
        lwz     0,F_VRSAVE(1)
        mtspr   256,0
4:      lfd     0,F_FPSCR(1)
        mtfsf   0xFF,0
        lfd     0,F_FPR+0(1)
        lfd     1,F_FPR+8(1)
        lfd     2,F_FPR+16(1)
        lfd     3,F_FPR+24(1)
        lfd     4,F_FPR+32(1)
        lfd     5,F_FPR+40(1)
        lfd     6,F_FPR+48(1)
        lfd     7,F_FPR+56(1)
        lfd     8,F_FPR+64(1)
        lfd     9,F_FPR+72(1)
        lfd     10,F_FPR+80(1)
        lfd     11,F_FPR+88(1)
        lfd     12,F_FPR+96(1)
        lfd     13,F_FPR+104(1)
        lwz     2,F_GPR(1)
        lmw     5,F_GPR+12(1)           # r5-r31
5:      li      3,OP_RESUME
        addi    4,1,F_RB
        lwz     0,0(4)                  # a store, so that the block's page
        stw     0,0(4)                  # has a PTE
        li      0,MP_ENFORCER
        sc                              # returns only when it failed
        b       5b
