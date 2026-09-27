//! The allocator core (alloc/alloc.h), as Rust sees it.

use core::ffi::c_void;

/// Opaque `ca_heap`.
#[repr(C)]
pub struct CaHeap {
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

unsafe extern "C" {
    pub safe fn ca_heap_acquire() -> *mut CaHeap;
    pub fn ca_alloc_aligned(h: *mut CaHeap, size: usize, align: usize) -> *mut c_void;
    pub fn ca_realloc(h: *mut CaHeap, p: *mut c_void, size: usize) -> *mut c_void;
    pub fn ca_free(h: *mut CaHeap, p: *mut c_void);
    pub safe fn ca_stats_get() -> CaStats;
}
