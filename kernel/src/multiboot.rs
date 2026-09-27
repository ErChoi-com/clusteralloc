//! Multiboot info: only the memory map is used.

use core::ptr::read_unaligned;

pub const BOOT_MAGIC: u32 = 0x2BAD_B002;
const MAX_REGIONS: usize = 32;

#[derive(Clone, Copy, Default)]
pub struct Region {
    pub start: u64,
    pub end: u64,
}

pub struct MemoryMap {
    regions: [Region; MAX_REGIONS],
    len: usize,
}

impl MemoryMap {
    pub fn usable(&self) -> &[Region] {
        &self.regions[..self.len]
    }
}

/// Copy the usable RAM regions out of the Multiboot info.
///
/// Call before anything allocates: QEMU puts the info block just after the
/// image, in memory the frame allocator will later hand out.
pub unsafe fn memory_map(info: u32) -> MemoryMap {
    let info = info as usize as *const u8;
    let mut map = MemoryMap { regions: [Region::default(); MAX_REGIONS], len: 0 };
    unsafe {
        let flags = read_unaligned(info as *const u32);
        assert!(flags & (1 << 6) != 0, "boot loader gave no memory map");
        let length = read_unaligned(info.add(44) as *const u32) as usize;
        let base = read_unaligned(info.add(48) as *const u32) as usize as *const u8;

        // Entries: size (not counting itself), base, length, type; 1 = RAM.
        let mut offset = 0;
        while offset < length && map.len < MAX_REGIONS {
            let entry = base.add(offset);
            let size = read_unaligned(entry as *const u32) as usize;
            let start = read_unaligned(entry.add(4) as *const u64);
            let bytes = read_unaligned(entry.add(12) as *const u64);
            let kind = read_unaligned(entry.add(20) as *const u32);
            if kind == 1 && bytes > 0 {
                map.regions[map.len] = Region { start, end: start + bytes };
                map.len += 1;
            }
            offset += size + 4;
        }
    }
    map
}
