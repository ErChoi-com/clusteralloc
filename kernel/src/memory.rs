//! Physical memory, handed to the allocator core as its host pages.
//!
//! Frames are 64 KiB, the core's span size, so every frame is span-aligned
//! and a bitmap for the 4 GiB boot.s maps is only 8 KiB. Memory is identity
//! mapped, so allocating a frame is just flipping bits.

use crate::cpu::IrqCell;
use crate::multiboot::MemoryMap;
use core::ffi::c_void;

const FRAME_SHIFT: u32 = 16;
const FRAME: u64 = 1 << FRAME_SHIFT;
const FRAMES: usize = (4u64 << 30 >> FRAME_SHIFT) as usize; // what boot.s maps
const WORDS: usize = FRAMES / 64;

struct Frames {
    used: [u64; WORDS], // 1 = taken (or not RAM)
    free: usize,
    next: usize, // next-fit cursor
}

static FRAMES_MAP: IrqCell<Frames> = IrqCell::new(Frames { used: [u64::MAX; WORDS], free: 0, next: 0 });

unsafe extern "C" {
    static __image_end: u8;
}

impl Frames {
    fn is_used(&self, f: usize) -> bool {
        self.used[f / 64] & (1 << (f % 64)) != 0
    }

    fn mark(&mut self, first: usize, count: usize, used: bool) {
        for f in first..first + count {
            if used {
                self.used[f / 64] |= 1 << (f % 64);
            } else {
                self.used[f / 64] &= !(1 << (f % 64));
            }
        }
    }

    /// First run of `count` free frames at or after `from`.
    fn find(&self, from: usize, count: usize) -> Option<usize> {
        let mut f = from;
        while f + count <= FRAMES {
            if self.used[f / 64] == u64::MAX {
                f = (f / 64 + 1) * 64; // skip a full word at once
                continue;
            }
            match (f..f + count).find(|&g| self.is_used(g)) {
                Some(taken) => f = taken + 1,
                None => return Some(f),
            }
        }
        None
    }

    fn take(&mut self, count: usize) -> Option<usize> {
        let first = self.find(self.next, count).or_else(|| self.find(0, count))?;
        self.mark(first, count, true);
        self.free -= count;
        self.next = first + count;
        Some(first)
    }
}

/// Free every whole frame of usable RAM above the kernel image.
pub fn init(map: &MemoryMap) -> u64 {
    let image_end = &raw const __image_end as u64;
    FRAMES_MAP.with(|frames| {
        for region in map.usable() {
            let start = region.start.max(image_end).next_multiple_of(FRAME);
            let end = region.end.min((FRAMES as u64) << FRAME_SHIFT) & !(FRAME - 1);
            if start < end {
                let count = ((end - start) >> FRAME_SHIFT) as usize;
                frames.mark((start >> FRAME_SHIFT) as usize, count, false);
                frames.free += count;
            }
        }
        (frames.free as u64) << FRAME_SHIFT
    })
}

#[unsafe(no_mangle)]
extern "C" fn ca_pages_alloc(bytes: usize) -> *mut c_void {
    let count = (bytes as u64).div_ceil(FRAME) as usize;
    FRAMES_MAP
        .with(|frames| frames.take(count))
        .map_or(core::ptr::null_mut(), |first| ((first as u64) << FRAME_SHIFT) as *mut c_void)
}

#[unsafe(no_mangle)]
extern "C" fn ca_pages_free(p: *mut c_void, bytes: usize) {
    let first = (p as u64 >> FRAME_SHIFT) as usize;
    let count = (bytes as u64).div_ceil(FRAME) as usize;
    FRAMES_MAP.with(|frames| {
        debug_assert!((first..first + count).all(|f| frames.is_used(f)), "double free of a frame");
        frames.mark(first, count, false);
        frames.free += count;
    });
}
