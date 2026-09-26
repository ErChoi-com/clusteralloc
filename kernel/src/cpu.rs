//! The handful of privileged instructions the lab needs.

use core::arch::asm;

pub unsafe fn outb(port: u16, value: u8) {
    unsafe { asm!("out dx, al", in("dx") port, in("al") value, options(nomem, nostack, preserves_flags)) }
}

pub unsafe fn inb(port: u16) -> u8 {
    let value: u8;
    unsafe { asm!("in al, dx", out("al") value, in("dx") port, options(nomem, nostack, preserves_flags)) }
    value
}

pub unsafe fn outl(port: u16, value: u32) {
    unsafe { asm!("out dx, eax", in("dx") port, in("eax") value, options(nomem, nostack, preserves_flags)) }
}

pub fn enable_interrupts() {
    unsafe { asm!("sti", options(nomem, nostack)) }
}

pub fn disable_interrupts() {
    unsafe { asm!("cli", options(nomem, nostack)) }
}

/// Sleep until the next interrupt.
pub fn halt() {
    unsafe { asm!("hlt", options(nomem, nostack, preserves_flags)) }
}

fn interrupts_enabled() -> bool {
    let flags: u64;
    unsafe { asm!("pushfq", "pop {}", out(reg) flags, options(nomem, preserves_flags)) }
    flags & (1 << 9) != 0
}

pub fn without_interrupts<R>(f: impl FnOnce() -> R) -> R {
    let was_on = interrupts_enabled();
    disable_interrupts();
    let result = f();
    if was_on {
        enable_interrupts();
    }
    result
}

const DEBUG_EXIT_PORT: u16 = 0xf4;

/// Leave QEMU through the isa-debug-exit device. QEMU exits with
/// (value << 1) | 1, so success is 33 and failure 35.
pub fn exit(success: bool) -> ! {
    unsafe { outl(DEBUG_EXIT_PORT, if success { 0x10 } else { 0x11 }) };
    // No such device (not QEMU): just stop.
    loop {
        disable_interrupts();
        halt();
    }
}
