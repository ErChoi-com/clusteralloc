//! COM1, polled. QEMU's `-serial stdio` puts it on the terminal.

use crate::cpu::{self, inb, outb};
use core::fmt::{self, Write};

const COM1: u16 = 0x3f8;

pub fn init() {
    unsafe {
        outb(COM1 + 1, 0x00); // no UART interrupts, we poll
        outb(COM1 + 3, 0x80); // divisor latch on
        outb(COM1, 0x01); //     115200 baud
        outb(COM1 + 1, 0x00);
        outb(COM1 + 3, 0x03); // 8N1, latch off
        outb(COM1 + 2, 0xC7); // FIFOs on and cleared
        outb(COM1 + 4, 0x03); // DTR + RTS
    }
}

fn put(byte: u8) {
    unsafe {
        while inb(COM1 + 5) & 0x20 == 0 {
            core::hint::spin_loop();
        }
        outb(COM1, byte);
    }
}

struct Port;

impl Write for Port {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        for b in s.bytes() {
            if b == b'\n' {
                put(b'\r');
            }
            put(b);
        }
        Ok(())
    }
}

pub fn print(args: fmt::Arguments) {
    // A line is never torn by an exception handler printing in the middle.
    cpu::without_interrupts(|| {
        let _ = Port.write_fmt(args);
    });
}

#[macro_export]
macro_rules! print {
    ($($arg:tt)*) => { $crate::serial::print(format_args!($($arg)*)) };
}

#[macro_export]
macro_rules! println {
    () => { $crate::print!("\n") };
    ($($arg:tt)*) => { $crate::serial::print(format_args!("{}\n", format_args!($($arg)*))) };
}
