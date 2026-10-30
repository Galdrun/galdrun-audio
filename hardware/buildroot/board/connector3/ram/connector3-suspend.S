/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2015 Freescale Semiconductor, Inc.
 * CPU-off / DDR self-refresh branch of Freescale's 2015 suspend-imx7.S.
 * The Mega/Fast domain stays powered for Ethernet wake. No DDR PHY reset,
 * rail switching or LPSR. All code and translation tables used in WFI are
 * in reserved OCRAM. Offsets match struct connector3_pm_info. */
#include <linux/linkage.h>
#include <asm/assembler.h>

    .arm
    .align 3
ENTRY(connector3_suspend)
    push {r4-r12, lr}
    /* SRC GPR1 is the physical OCRAM resume PC; GPR2 is its argument. */
    ldr r1, [r0, #0]
    adr r2, connector3_resume
    adr r3, connector3_suspend
    sub r2, r2, r3
    add r2, r2, #0x100
    add r2, r2, r1
    ldr r11, [r0, #20]
    str r2, [r11, #0x74]
    str r1, [r11, #0x78]

    /* Flush while the kernel mappings and DDR are still available. */
    push {r0-r3, lr}
    ldr r7, =v7_flush_dcache_all
    blx r7
    pop {r0-r3, lr}
    mrc p15, 0, r6, c1, c0, 0
    str r6, [r0, #32]
    bic r6, r6, #4
    bic r6, r6, #0x800
    mcr p15, 0, r6, c1, c0, 0
    dsb
    isb
    push {r0-r3, lr}
    ldr r7, =v7_flush_dcache_all
    blx r7
    pop {r0-r3, lr}

    mrc p15, 0, r6, c2, c0, 1
    str r6, [r0, #24]
    mrc p15, 0, r6, c2, c0, 2
    str r6, [r0, #28]
    /* TTBR1 N=1: only the high 8 KiB of this 16 KiB table is used. */
    orr r6, r6, #0x11
    ldr r7, [r0, #0]
    dsb
    mcr p15, 0, r7, c2, c0, 1
    mcr p15, 0, r6, c2, c0, 2
    isb
    mov r6, #0
    mcr p15, 0, r6, c8, c3, 0
    dsb
    isb

    ldr r11, [r0, #12]
    mov r7, #0
    str r7, [r11, #0x30]
    ldr r6, =0x10001
1:  ldr r7, [r11, #0x3fc]
    tst r7, r6
    bne 1b
    mov r7, #0x20
    str r7, [r11, #0x30]
2:  ldr r7, [r11, #4]
    and r7, r7, #3
    cmp r7, #3
    bne 2b
3:  ldr r7, [r11, #4]
    tst r7, #0x20
    beq 3b
    mov r7, #0x28
    str r7, [r11, #0x30]
    dsb
    wfi
    nop
    nop
    nop
    nop

    /* A pending interrupt can abort CPU power-down. Restore DDR and return
     * using the original page tables and cache state in that case. */
    ldr r11, [r0, #12]
    bl connector3_ddr_exit
    ldr r6, [r0, #24]
    mcr p15, 0, r6, c2, c0, 1
    ldr r6, [r0, #28]
    mcr p15, 0, r6, c2, c0, 2
    isb
    mov r6, #0
    mcr p15, 0, r6, c8, c3, 0
    dsb
    isb
    ldr r6, [r0, #32]
    mcr p15, 0, r6, c1, c0, 0
    isb
    pop {r4-r12, pc}

connector3_resume:
    /* Boot ROM enters with MMU off and r0 = physical pm_info. */
    mov r6, #1
    str r6, [r0, #36]
    mov r6, #0
    mcr p15, 0, r6, c7, c5, 0
    mcr p15, 0, r6, c7, c5, 6
    mov r6, #0x1800
    mcr p15, 0, r6, c1, c0, 0
    isb
    ldr r11, [r0, #16]
    mov r7, #0
    str r7, [r11, #0x74]
    str r7, [r11, #0x78]
    ldr r11, [r0, #8]
    bl connector3_ddr_exit
    ldr r7, [r0, #4]
    bx r7

connector3_ddr_exit:
    mov r7, #0
    str r7, [r11, #0x30]
4:  ldr r7, [r11, #4]
    and r7, r7, #3
    cmp r7, #3
    beq 4b
    mov r7, #1
    str r7, [r11, #0x30]
    dsb
    bx lr
    .ltorg
ENDPROC(connector3_suspend)
    .global connector3_suspend_end
connector3_suspend_end:
