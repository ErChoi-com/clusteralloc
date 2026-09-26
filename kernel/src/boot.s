/*
 * Multiboot entry: 32-bit protected mode, paging off, eax = magic, ebx = info.
 * Identity-maps the first 4 GiB, enters long mode and calls kmain(magic, info).
 *
 * The header uses the address fields (flags bit 16) so the loader can take a
 * flat binary and does not have to parse a 64-bit ELF.
 */

.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00010003            /* page-align, memory map, address fields */

.section .multiboot, "a"
.align 4
mb_header:
    .long MB_MAGIC
    .long MB_FLAGS
    .long -(MB_MAGIC + MB_FLAGS)
    .long mb_header                  /* header_addr */
    .long __image_start              /* load_addr */
    .long 0                          /* load_end_addr: the whole file */
    .long __image_end                /* bss_end_addr: the loader zeroes up to here */
    .long start32                    /* entry_addr */

.section .boot, "ax"
.code32
.global start32
start32:
    mov $boot_stack_top, %esp
    mov %eax, %ebp                   /* save magic and info */
    mov %ebx, %esi

    /* PML4[0] -> PDPT, PDPT[0..3] -> four page directories = 4 GiB. */
    mov $pdpt + 3, %eax
    mov %eax, pml4
    mov $pd + 3, %eax
    xor %ecx, %ecx
1:  mov %eax, pdpt(,%ecx,8)
    add $4096, %eax
    inc %ecx
    cmp $4, %ecx
    jne 1b

    /* 2048 large pages of 2 MiB: present, writable, PS. */
    xor %ecx, %ecx
2:  mov %ecx, %eax
    shl $21, %eax
    or $0x83, %eax
    mov %eax, pd(,%ecx,8)
    inc %ecx
    cmp $2048, %ecx
    jne 2b

    /* Except the first 2 MiB, which gets 4 KiB pages so that page 0 can
     * stay unmapped: a null pointer dereference faults instead of reading
     * the real-mode interrupt table. */
    mov $pt0 + 3, %eax
    mov %eax, pd
    mov $1, %ecx
3:  mov %ecx, %eax
    shl $12, %eax
    or $0x3, %eax
    mov %eax, pt0(,%ecx,8)
    inc %ecx
    cmp $512, %ecx
    jne 3b

    mov $pml4, %eax
    mov %eax, %cr3
    mov %cr4, %eax
    or $0x20, %eax                   /* PAE */
    mov %eax, %cr4
    mov $0xC0000080, %ecx            /* EFER */
    rdmsr
    or $0x100, %eax                  /* long mode enable */
    wrmsr
    mov %cr0, %eax
    or $0x80000001, %eax             /* paging + protected mode */
    mov %eax, %cr0

    lgdt gdt_ptr
    ljmp $0x08, $start64

.code64
start64:
    xor %eax, %eax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    mov $boot_stack_top, %rsp

    mov %ebp, %edi                   /* kmain(magic, info); 32-bit moves zero-extend */
    mov %esi, %esi
    call kmain
4:  cli
    hlt
    jmp 4b

.align 8
gdt:
    .quad 0
    .quad 0x00209A0000000000         /* 0x08: 64-bit ring 0 code */
gdt_end:
gdt_ptr:
    .word gdt_end - gdt - 1
    .long gdt

.section .bss, "aw", @nobits
.align 4096
pml4:   .skip 4096
pdpt:   .skip 4096
pd:     .skip 4 * 4096
pt0:    .skip 4096

.align 16
boot_stack:
    .skip 64 * 1024
boot_stack_top:
