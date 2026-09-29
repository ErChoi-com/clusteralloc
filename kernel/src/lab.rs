//! Runs the bench/pipeline.c workload on bare metal. Producer and consumer
//! stages each own a heap, so every free still goes through the inbox path,
//! just on one CPU. Two runs: interrupts off, and with a 1 kHz timer.

use crate::ffi::{
    CaHeap, PipeReport, PipeRow, PipeSide, Pipeline, ca_alloc, ca_free, ca_heap_acquire, ca_stats_get,
    pipe_init, pipe_reset_stats, pipe_run_interleaved, pipe_size, pipe_summarize, pipe_ticks,
};
use crate::interrupts::{self, TICKS};
use crate::{cpu, println};
use alloc::alloc::{Layout, alloc, dealloc};
use alloc::vec::Vec;
use core::ffi::c_void;
use core::sync::atomic::Ordering::Relaxed;

const MESSAGES: u64 = 2_000_000;
const TIMER_HZ: u32 = 1000;

struct Run {
    name: &'static str,
    timer_interrupts: u64,
    report: PipeReport,
}

unsafe extern "C" fn stage_alloc(heap: *mut c_void, size: usize) -> *mut c_void {
    unsafe { ca_alloc(heap.cast(), size) }
}

unsafe extern "C" fn stage_free(heap: *mut c_void, p: *mut c_void) {
    unsafe { ca_free(heap.cast(), p) }
}

fn wait_ticks(n: u64) {
    let until = TICKS.load(Relaxed) + n;
    while TICKS.load(Relaxed) < until {
        cpu::halt();
    }
}

/// TSC frequency in Hz, timed against the PIT.
fn calibrate() -> u64 {
    const TICKS_TIMED: u64 = 100;
    let period_ns = interrupts::set_timer(TIMER_HZ);
    cpu::enable_interrupts();
    wait_ticks(1); // start on a tick edge
    let t0 = pipe_ticks();
    wait_ticks(TICKS_TIMED);
    let t1 = pipe_ticks();
    cpu::disable_interrupts();
    ((t1 - t0) as u128 * 1_000_000_000 / (TICKS_TIMED * period_ns) as u128) as u64
}

fn measure(pipe: *mut Pipeline, name: &'static str, timer: bool) -> Run {
    if timer {
        cpu::enable_interrupts();
    }
    unsafe {
        pipe_run_interleaved(pipe, MESSAGES / 10); // warm the spans and the pool
        pipe_reset_stats(pipe);
    }
    let before = TICKS.load(Relaxed);
    unsafe { pipe_run_interleaved(pipe, MESSAGES) };
    let timer_interrupts = TICKS.load(Relaxed) - before;
    cpu::disable_interrupts();

    let mut report = PipeReport::default();
    unsafe { pipe_summarize(pipe, &mut report) };
    Run { name, timer_interrupts, report }
}

fn print_row(name: &str, row: &PipeRow, tsc_hz: u64) {
    let ns = |ticks: u64| (ticks as u128 * 1_000_000_000 / tsc_hz as u128) as u64;
    println!(
        "  {name:<12} {:>8} {:>8} {:>8} {:>8} {:>10}",
        ns(row.p50),
        ns(row.p99),
        ns(row.p999),
        ns(row.p9999),
        ns(row.max)
    );
}

pub fn run() -> bool {
    let tsc_hz = calibrate();
    println!("lab: TSC at {} MHz, {MESSAGES} messages per run, both stages on cpu 0", tsc_hz / 1_000_000);

    let producer: *mut CaHeap = ca_heap_acquire();
    let consumer: *mut CaHeap = ca_heap_acquire();
    assert!(!producer.is_null() && !consumer.is_null(), "no memory for the stage heaps");

    // The pipeline struct comes from the kernel heap.
    let layout = Layout::from_size_align(pipe_size(), 64).unwrap();
    let pipe = unsafe { alloc(layout) }.cast::<Pipeline>();
    assert!(!pipe.is_null(), "no memory for the pipeline");
    unsafe {
        pipe_init(
            pipe,
            PipeSide { alloc: stage_alloc, free: stage_free, ctx: producer.cast() },
            PipeSide { alloc: stage_alloc, free: stage_free, ctx: consumer.cast() },
            42,
        );
    }

    let runs: Vec<Run> = [("interrupts off", false), ("1 kHz timer", true)]
        .into_iter()
        .map(|(name, timer)| measure(pipe, name, timer))
        .collect();
    unsafe { dealloc(pipe.cast(), layout) };

    let mut ok = true;
    for run in &runs {
        let r = &run.report;
        println!("{}: {} timer interrupts during the run", run.name, run.timer_interrupts);
        println!("  {:<12} {:>8} {:>8} {:>8} {:>8} {:>10}", "(ns)", "p50", "p99", "p99.9", "p99.99", "max");
        print_row("alloc", &r.alloc, tsc_hz);
        print_row("remote free", &r.free, tsc_hz);
        if r.corrupt != 0 || r.failed != 0 || r.messages != MESSAGES {
            println!("  {} corrupt, failed = {}, {} messages", r.corrupt, r.failed, r.messages);
            ok = false;
        }
    }

    let s = ca_stats_get();
    println!(
        "core: {} heaps, {} allocs, {} remote frees, {} KiB mapped, {} KiB pooled, {} invalid frees",
        s.heaps,
        s.allocs,
        s.remote_frees,
        s.mapped_bytes >> 10,
        s.pool_bytes >> 10,
        s.invalid_frees
    );
    // Every message, warm-up included, is freed by the other stage.
    ok && s.invalid_frees == 0 && s.remote_frees >= runs.len() as u64 * MESSAGES
}
