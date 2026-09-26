//! Bare-metal x86-64 kernel for running the pipeline benchmark with no OS
//! underneath. So far it only boots and talks over the serial port.

#![no_std]
#![no_main]

mod cpu;
mod serial;

use core::panic::PanicInfo;

core::arch::global_asm!(include_str!("boot.s"), options(att_syntax));

#[unsafe(no_mangle)]
extern "C" fn kmain(magic: u32, _info: u32) -> ! {
    serial::init();
    println!("\nlatency lab: long mode, 4 GiB identity mapped (magic {magic:#x})");
    cpu::exit(true)
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    cpu::disable_interrupts();
    println!("\npanic: {info}");
    cpu::exit(false)
}
