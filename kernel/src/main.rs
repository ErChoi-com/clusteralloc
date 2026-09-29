//! Bare-metal x86-64 kernel that runs the pipeline benchmark with no OS
//! underneath. One CPU, identity-mapped, no tasks. Exits QEMU with the
//! verdict, so `make run` doubles as a test.
//!
//! Boot: boot.s, serial, memory map, frames, IDT/PIC, heap, lab.

#![no_std]
#![no_main]

extern crate alloc;

mod cpu;
mod ffi;
mod heap;
mod interrupts;
mod lab;
mod memory;
mod multiboot;
mod serial;

use core::panic::PanicInfo;

core::arch::global_asm!(include_str!("boot.s"), options(att_syntax));
core::arch::global_asm!(include_str!("interrupts.s"), options(att_syntax));

#[unsafe(no_mangle)]
extern "C" fn kmain(magic: u32, info: u32) -> ! {
    serial::init();
    println!("\nlatency lab: long mode, 4 GiB identity mapped");
    assert_eq!(magic, multiboot::BOOT_MAGIC, "not started by a Multiboot loader");

    let map = unsafe { multiboot::memory_map(info) };
    let free = memory::init(&map);
    println!("memory: {} MiB in 64 KiB frames", free >> 20);

    interrupts::init();
    heap::init();

    let ok = lab::run();
    println!("{}", if ok { "ok" } else { "FAIL" });
    cpu::exit(ok)
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    cpu::disable_interrupts();
    println!("\npanic: {info}");
    cpu::exit(false)
}
