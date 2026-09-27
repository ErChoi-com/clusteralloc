//! Bare-metal x86-64 kernel for running the pipeline benchmark with no OS
//! underneath. Boots, sets up interrupts and checks the timer works.

#![no_std]
#![no_main]

mod cpu;
mod interrupts;
mod serial;

use core::panic::PanicInfo;
use core::sync::atomic::Ordering;

core::arch::global_asm!(include_str!("boot.s"), options(att_syntax));
core::arch::global_asm!(include_str!("interrupts.s"), options(att_syntax));

#[unsafe(no_mangle)]
extern "C" fn kmain(magic: u32, _info: u32) -> ! {
    serial::init();
    println!("\nlatency lab: long mode, 4 GiB identity mapped (magic {magic:#x})");

    interrupts::init();
    interrupts::set_timer(1000);
    cpu::enable_interrupts();
    while interrupts::TICKS.load(Ordering::Relaxed) < 100 {
        cpu::halt();
    }
    cpu::disable_interrupts();
    println!("timer: 100 ticks");
    cpu::exit(true)
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    cpu::disable_interrupts();
    println!("\npanic: {info}");
    cpu::exit(false)
}
