//! The shared C: alloc/alloc.h and bench/pipeline.h, as Rust sees them.

use core::ffi::c_void;

/// Opaque `ca_heap`.
#[repr(C)]
pub struct CaHeap {
    _private: [u8; 0],
}

/// Opaque `pipeline`; sized at run time with `pipe_size()`.
#[repr(C)]
pub struct Pipeline {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CaStats {
    pub heaps: u64,
    pub allocs: u64,
    pub frees: u64,
    pub remote_frees: u64,
    pub small_bytes: u64,
    pub spans: u64,
    pub large_allocs: u64,
    pub large_bytes: u64,
    pub pool_bytes: u64,
    pub mapped_bytes: u64,
    pub invalid_frees: u64,
}

#[repr(C)]
pub struct PipeSide {
    pub alloc: unsafe extern "C" fn(ctx: *mut c_void, size: usize) -> *mut c_void,
    pub free: unsafe extern "C" fn(ctx: *mut c_void, p: *mut c_void),
    pub ctx: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PipeRow {
    pub p50: u64,
    pub p99: u64,
    pub p999: u64,
    pub p9999: u64,
    pub max: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PipeReport {
    pub alloc: PipeRow,
    pub free: PipeRow,
    pub messages: u64,
    pub corrupt: u64,
    pub failed: i32,
}

unsafe extern "C" {
    pub safe fn ca_heap_acquire() -> *mut CaHeap;
    pub fn ca_alloc(h: *mut CaHeap, size: usize) -> *mut c_void;
    pub fn ca_alloc_aligned(h: *mut CaHeap, size: usize, align: usize) -> *mut c_void;
    pub fn ca_realloc(h: *mut CaHeap, p: *mut c_void, size: usize) -> *mut c_void;
    pub fn ca_free(h: *mut CaHeap, p: *mut c_void);
    pub safe fn ca_stats_get() -> CaStats;

    pub safe fn pipe_size() -> usize;
    pub fn pipe_init(p: *mut Pipeline, producer: PipeSide, consumer: PipeSide, seed: u64);
    pub fn pipe_reset_stats(p: *mut Pipeline);
    pub fn pipe_run_interleaved(p: *mut Pipeline, messages: u64);
    pub fn pipe_summarize(p: *const Pipeline, r: *mut PipeReport);
    pub safe fn pipe_ticks() -> u64;
}
