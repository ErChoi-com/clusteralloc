//! Bare-metal x86-64 kernel for running the pipeline benchmark with no OS
//! underneath. One CPU, identity-mapped, no tasks.
//!
//! Boot: boot.s, serial, memory map, frames, IDT/PIC, heap.

#![no_std]
#![no_main]

extern crate alloc;

mod cpu;
mod ffi;
mod heap;
mod interrupts;
mod memory;
mod multiboot;
mod serial;

use alloc::boxed::Box;
use alloc::vec::Vec;
use core::panic::PanicInfo;
use core::sync::atomic::Ordering;

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
    interrupts::set_timer(1000);
    cpu::enable_interrupts();
    while interrupts::TICKS.load(Ordering::Relaxed) < 100 {
        cpu::halt();
    }
    cpu::disable_interrupts();
    println!("timer: 100 ticks");

    heap::init();

    // Smoke test: lots of small blocks plus one large one (the Vec).
    let v: Vec<Box<u64>> = (0..10_000).map(Box::new).collect();
    assert_eq!(v.iter().map(|b| **b).sum::<u64>(), 49_995_000);
    let s = ffi::ca_stats_get();
    println!("heap: {} allocs, {} large, {} KiB mapped", s.allocs, s.large_allocs, s.mapped_bytes >> 10);
    drop(v);
    cpu::exit(true)
}

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    cpu::disable_interrupts();
    println!("\npanic: {info}");
    cpu::exit(false)
}
