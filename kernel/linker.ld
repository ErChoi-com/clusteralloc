/* Flat image at 1 MiB. The Multiboot header must be in the first 8 KiB. */
ENTRY(start32)

SECTIONS
{
    . = 1M;
    __image_start = .;

    .boot : {
        KEEP(*(.multiboot))
        *(.boot)
    }

    .text : {
        *(.text .text.*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
        *(.data.rel.ro .data.rel.ro.*)
    }

    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    .bss : ALIGN(4K) {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN(4K);
    __image_end = .;

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note .note.*)
    }
}
