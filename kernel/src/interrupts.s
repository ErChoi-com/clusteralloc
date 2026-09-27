/*
 * Entry stubs for vectors 0..47 (exceptions, then the PICs). Each pushes a
 * dummy error code if the CPU did not, saves the registers and calls
 * interrupt_dispatch(&frame).
 *
 * The CPU aligns RSP to 16 before pushing its 5-word frame; error code,
 * vector and 15 registers make 22 words, so the call is aligned.
 */

.section .text

.macro isr_plain vec
isr_\vec:
    push $0
    push $\vec
    jmp isr_common
.endm

.macro isr_code vec                  /* the CPU already pushed an error code */
isr_\vec:
    push $\vec
    jmp isr_common
.endm

.irp vec, 0, 1, 2, 3, 4, 5, 6, 7, 9, 15, 16, 18, 19, 20, 22, 23, 24, 25, 26, 27, 28, 31
    isr_plain \vec
.endr
.irp vec, 8, 10, 11, 12, 13, 14, 17, 21, 29, 30
    isr_code \vec
.endr
.irp vec, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47
    isr_plain \vec
.endr

isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, %rdi
    cld
    call interrupt_dispatch
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp                    /* vector and error code */
    iretq

.section .rodata
.align 8
.global isr_table
isr_table:
.irp vec, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47
    .quad isr_\vec
.endr
