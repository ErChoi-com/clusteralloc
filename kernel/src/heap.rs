//! The Rust global allocator, on the same C core as the Linux library.
//!
//! One heap. A heap has a single owner and the only other context is an
//! interrupt handler, so every call runs with interrupts off. (The current
//! handlers never allocate, but the heap does not rely on that.)

use crate::cpu::without_interrupts;
use crate::ffi::{CaHeap, ca_alloc_aligned, ca_free, ca_heap_acquire, ca_realloc};
use core::alloc::{GlobalAlloc, Layout};
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

const MIN_ALIGN: usize = 16; // CA_MIN_ALIGN: what plain ca_realloc guarantees

static HEAP: AtomicPtr<CaHeap> = AtomicPtr::new(ptr::null_mut());

pub fn init() {
    let heap = ca_heap_acquire();
    assert!(!heap.is_null(), "no memory for the kernel heap");
    HEAP.store(heap, Ordering::Relaxed);
}

fn heap() -> *mut CaHeap {
    HEAP.load(Ordering::Relaxed)
}

struct Core;

unsafe impl GlobalAlloc for Core {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        without_interrupts(|| unsafe { ca_alloc_aligned(heap(), layout.size(), layout.align()) }.cast())
    }

    unsafe fn dealloc(&self, p: *mut u8, _layout: Layout) {
        without_interrupts(|| unsafe { ca_free(heap(), p.cast()) })
    }

    unsafe fn realloc(&self, p: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        if layout.align() <= MIN_ALIGN {
            return without_interrupts(|| unsafe { ca_realloc(heap(), p.cast(), new_size) }.cast());
        }
        let new_layout = unsafe { Layout::from_size_align_unchecked(new_size, layout.align()) };
        let q = unsafe { self.alloc(new_layout) };
        if !q.is_null() {
            unsafe {
                ptr::copy_nonoverlapping(p, q, layout.size().min(new_size));
                self.dealloc(p, layout);
            }
        }
        q
    }
}

#[global_allocator]
static ALLOCATOR: Core = Core;
