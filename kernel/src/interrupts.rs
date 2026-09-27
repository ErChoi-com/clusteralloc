//! IDT, legacy PIC and PIT: just enough for a periodic timer interrupt that
//! the lab can switch on and off.

use crate::cpu::{self, IrqCell, outb};
use core::arch::asm;
use core::sync::atomic::{AtomicU64, Ordering};

const VECTORS: usize = 48;
const TIMER_VECTOR: u64 = 32; // PIC lines are remapped to 32..47
const PIT_HZ: u32 = 1_193_182;

const PIC1_CMD: u16 = 0x20;
const PIC1_DATA: u16 = 0x21;
const PIC2_CMD: u16 = 0xA0;
const PIC2_DATA: u16 = 0xA1;
const PIC_EOI: u8 = 0x20;
const PIT_CHANNEL0: u16 = 0x40;
const PIT_COMMAND: u16 = 0x43;

/// Timer interrupts taken so far.
pub static TICKS: AtomicU64 = AtomicU64::new(0);

/// Stack layout left by interrupts.s, lowest address first. Most fields are
/// never read but the layout must match the stub.
#[allow(dead_code)]
#[rustfmt::skip]
#[repr(C)]
pub struct Frame {
    r15: u64, r14: u64, r13: u64, r12: u64, r11: u64, r10: u64, r9: u64, r8: u64,
    rbp: u64, rdi: u64, rsi: u64, rdx: u64, rcx: u64, rbx: u64, rax: u64,
    vector: u64,
    error: u64,
    rip: u64,
    cs: u64,
    rflags: u64,
    rsp: u64,
    ss: u64,
}

#[derive(Clone, Copy)]
#[repr(C)]
struct Gate {
    offset_low: u16,
    selector: u16,
    ist: u8,
    attributes: u8,
    offset_mid: u16,
    offset_high: u32,
    reserved: u32,
}

impl Gate {
    const EMPTY: Gate = Gate {
        offset_low: 0,
        selector: 0,
        ist: 0,
        attributes: 0,
        offset_mid: 0,
        offset_high: 0,
        reserved: 0,
    };

    fn interrupt(handler: u64) -> Gate {
        Gate {
            offset_low: handler as u16,
            selector: 0x08, // the only code segment, from boot.s
            ist: 0,
            attributes: 0x8E, // present, ring 0, interrupt gate (IF cleared on entry)
            offset_mid: (handler >> 16) as u16,
            offset_high: (handler >> 32) as u32,
            reserved: 0,
        }
    }
}

#[repr(C, packed)]
struct IdtPointer {
    limit: u16,
    base: u64,
}

static IDT: IrqCell<[Gate; VECTORS]> = IrqCell::new([Gate::EMPTY; VECTORS]);

unsafe extern "C" {
    static isr_table: [u64; VECTORS];
}

pub fn init() {
    IDT.with(|idt| {
        for (gate, &handler) in idt.iter_mut().zip(unsafe { &isr_table }) {
            *gate = Gate::interrupt(handler);
        }
        let pointer = IdtPointer { limit: (size_of_val(idt) - 1) as u16, base: idt.as_ptr() as u64 };
        unsafe { asm!("lidt [{}]", in(reg) &pointer, options(readonly, nostack, preserves_flags)) };
    });
    remap_pics();
}

/// Move the PICs off the CPU exception vectors and mask all but the timer.
fn remap_pics() {
    unsafe {
        outb(PIC1_CMD, 0x11); // ICW1: init, expect ICW4
        outb(PIC2_CMD, 0x11);
        outb(PIC1_DATA, 32); //  ICW2: vector offsets
        outb(PIC2_DATA, 40);
        outb(PIC1_DATA, 0x04); // ICW3: slave on line 2
        outb(PIC2_DATA, 0x02);
        outb(PIC1_DATA, 0x01); // ICW4: 8086 mode
        outb(PIC2_DATA, 0x01);
        outb(PIC1_DATA, 0xFE); // masks: only IRQ0
        outb(PIC2_DATA, 0xFF);
    }
}

/// Program PIT channel 0 for a periodic interrupt near `hz`. Returns the
/// actual period in ns (the divisor is rounded).
pub fn set_timer(hz: u32) -> u64 {
    let divisor = (PIT_HZ / hz).clamp(1, 0xFFFF) as u16;
    unsafe {
        outb(PIT_COMMAND, 0x34); // channel 0, lobyte/hibyte, rate generator
        outb(PIT_CHANNEL0, divisor as u8);
        outb(PIT_CHANNEL0, (divisor >> 8) as u8);
    }
    divisor as u64 * 1_000_000_000 / PIT_HZ as u64
}

const EXCEPTIONS: [&str; 32] = [
    "divide error",
    "debug",
    "NMI",
    "breakpoint",
    "overflow",
    "bound range",
    "invalid opcode",
    "device not available",
    "double fault",
    "coprocessor overrun",
    "invalid TSS",
    "segment not present",
    "stack fault",
    "general protection",
    "page fault",
    "reserved",
    "x87 error",
    "alignment check",
    "machine check",
    "SIMD error",
    "virtualization",
    "control protection",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "reserved",
    "hypervisor injection",
    "VMM communication",
    "security",
    "reserved",
];

#[unsafe(no_mangle)]
extern "C" fn interrupt_dispatch(frame: &mut Frame) {
    match frame.vector {
        TIMER_VECTOR => {
            TICKS.fetch_add(1, Ordering::Relaxed);
            unsafe { outb(PIC1_CMD, PIC_EOI) };
        }
        v @ 0..32 => {
            let cr2: u64;
            unsafe { asm!("mov {}, cr2", out(reg) cr2, options(nomem, nostack, preserves_flags)) };
            crate::println!(
                "\ncpu exception {v}: {} at rip {:#x} (error {:#x}, cr2 {:#x}, rsp {:#x})",
                EXCEPTIONS[v as usize],
                frame.rip,
                frame.error,
                cr2,
                frame.rsp
            );
            cpu::exit(false);
        }
        // Spurious IRQ 7/15 can arrive despite the mask; they need no EOI.
        _ => {}
    }
}
