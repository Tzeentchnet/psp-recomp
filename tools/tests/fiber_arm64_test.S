#if defined(__APPLE__) && defined(__aarch64__)

.text
.p2align 2
.globl _sr_test_arm64_preserved_registers
_sr_test_arm64_preserved_registers:
    sub sp, sp, #176
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #160]

    mov x19, #0x1919
    mov x20, #0x2020
    mov x21, #0x2121
    mov x22, #0x2222
    mov x23, #0x2323
    mov x24, #0x2424
    mov x25, #0x2525
    mov x26, #0x2626
    mov x27, #0x2727
    mov x28, #0x2828
    mov x29, #0x2929

    mov x9, #0xd808
    fmov d8, x9
    mov x9, #0xd909
    fmov d9, x9
    mov x9, #0xda10
    fmov d10, x9
    mov x9, #0xdb11
    fmov d11, x9
    mov x9, #0xdc12
    fmov d12, x9
    mov x9, #0xdd13
    fmov d13, x9
    mov x9, #0xde14
    fmov d14, x9
    mov x9, #0xdf15
    fmov d15, x9

    ldr x9, [sp, #160]
    orr x9, x9, #0x400000
    msr fpcr, x9
    mov x9, #1
    msr fpsr, x9

    bl _sr_fiber_switch

    mov x9, #0x1919
    cmp x19, x9
    b.ne 1f
    mov x9, #0x2020
    cmp x20, x9
    b.ne 1f
    mov x9, #0x2121
    cmp x21, x9
    b.ne 1f
    mov x9, #0x2222
    cmp x22, x9
    b.ne 1f
    mov x9, #0x2323
    cmp x23, x9
    b.ne 1f
    mov x9, #0x2424
    cmp x24, x9
    b.ne 1f
    mov x9, #0x2525
    cmp x25, x9
    b.ne 1f
    mov x9, #0x2626
    cmp x26, x9
    b.ne 1f
    mov x9, #0x2727
    cmp x27, x9
    b.ne 1f
    mov x9, #0x2828
    cmp x28, x9
    b.ne 1f
    mov x9, #0x2929
    cmp x29, x9
    b.ne 1f

    fmov x9, d8
    mov x10, #0xd808
    cmp x9, x10
    b.ne 1f
    fmov x9, d9
    mov x10, #0xd909
    cmp x9, x10
    b.ne 1f
    fmov x9, d10
    mov x10, #0xda10
    cmp x9, x10
    b.ne 1f
    fmov x9, d11
    mov x10, #0xdb11
    cmp x9, x10
    b.ne 1f
    fmov x9, d12
    mov x10, #0xdc12
    cmp x9, x10
    b.ne 1f
    fmov x9, d13
    mov x10, #0xdd13
    cmp x9, x10
    b.ne 1f
    fmov x9, d14
    mov x10, #0xde14
    cmp x9, x10
    b.ne 1f
    fmov x9, d15
    mov x10, #0xdf15
    cmp x9, x10
    b.ne 1f

    mrs x9, fpcr
    ldr x10, [sp, #160]
    orr x10, x10, #0x400000
    cmp x9, x10
    b.ne 1f
    mrs x9, fpsr
    cmp x9, #1
    b.ne 1f

    mov w0, #1
    b 2f
1:
    mov w0, #0
2:
    ldp x9, x10, [sp, #160]
    msr fpcr, x9
    msr fpsr, x10
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #176
    ret

.subsections_via_symbols

#endif
