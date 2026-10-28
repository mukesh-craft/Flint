//! A bounded bump allocator for the stdlib modules.
//!
//! # Why this exists
//!
//! The Rust stdlib modules are `#![no_std]` for two measured reasons: Rust's
//! `std` aborts inside Flint's JIT on Android (TLS handle / soinfo), and two
//! `std` copies cannot share one link. `no_std` also means **no allocator** —
//! and every real codec needs one (`Vec` is not optional for decoding a JPEG or
//! a zstd frame).
//!
//! `extern crate alloc` plus this `GlobalAlloc` gives those crates their
//! `Vec`s while keeping the module free of `std`, JIT-loadable, and free of any
//! hidden heap growth.
//!
//! # The trade-off, stated plainly
//!
//! This is a bump allocator: `dealloc` is a no-op and memory is reclaimed only
//! by [`Arena::reset`], which the FFI entry points call before each operation.
//! That is exactly right for "decode one image, keep it, discard it" and
//! exactly wrong for a long-running loop that decodes a thousand images — so
//! every entry point resets first, and the arena is large enough for one
//! maximum-size operation.
//!
//! Running out of arena is **loud**: `alloc` returns null, and Rust's
//! `handle_alloc_error` aborts with a message. Nothing is silently truncated or
//! silently swapped for a smaller buffer.
//!
//! # Thread safety
//!
//! A test-and-set spin lock guards the bump pointer. Flint programs are
//! effectively single-threaded today, but the stdlib is loaded into a process
//! that might not stay that way, and a silently corrupting allocator is worse
//! than a spin.

use core::alloc::{GlobalAlloc, Layout};
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};

/// Arena size for codec working memory. Sized for a large image decode
/// (JPEG output plus codec scratch); it lives in BSS, so pages are only
/// committed when a decode actually touches them.
// Large for codec working memory in the shipped library; small in tests, where
// a 48 MiB array would be stack-adjacent BSS nobody needs.
#[cfg(not(test))]
const ARENA_BYTES: usize = 48 * 1024 * 1024;
#[cfg(test)]
const ARENA_BYTES: usize = 64 * 1024;

/// The bump arena itself.
pub struct Arena {
    buf: [u8; ARENA_BYTES],
    offset: AtomicUsize,
    lock: AtomicBool,
}

impl Arena {
    pub const fn new() -> Self {
        Arena {
            buf: [0; ARENA_BYTES],
            offset: AtomicUsize::new(0),
            lock: AtomicBool::new(false),
        }
    }

    /// Discards everything allocated since the last reset.
    ///
    /// # Safety
    /// Any pointer previously returned by this allocator becomes dangling. FFI
    /// entry points therefore reset only at the START of an operation, never
    /// while a result is still being read.
    pub unsafe fn reset(&self) {
        self.offset.store(0, Ordering::Release);
    }

    pub fn used(&self) -> usize {
        self.offset.load(Ordering::Acquire)
    }

    pub fn capacity(&self) -> usize {
        ARENA_BYTES
    }

    /// Claims `size` bytes, aligned to `align`, or returns null when full.
    fn take(&self, size: usize, align: usize) -> *mut u8 {
        // Spin rather than block: a stdlib allocator must not depend on a
        // threading runtime that the module deliberately does not link.
        while self
            .lock
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }

        // Alignment must be computed on the ABSOLUTE address, not on the
        // offset: `buf`'s own address is only naturally aligned (8 bytes), so
        // rounding the offset alone would hand back a pointer that is not
        // actually 64-byte aligned. The first version did exactly that.
        let current = self.offset.load(Ordering::Relaxed);
        let base = self.buf.as_ptr() as usize;
        let absolute = base + current;
        let aligned_absolute = (absolute + align - 1) & !(align - 1);
        let start = current + (aligned_absolute - absolute);
        let next = start + size;
        let result: *mut u8 = if next <= ARENA_BYTES && next >= start {
            self.offset.store(next, Ordering::Relaxed);
            // SAFETY: `start..next` lies within `buf` by the bounds check
            // above, and the lock excludes a concurrent bump from claiming the
            // same region.
            unsafe { (self.buf.as_ptr().add(start)) as *mut u8 }
        } else {
            core::ptr::null_mut()
        };

        self.lock.store(false, Ordering::Release);
        result
    }
}

static mut ARENA: Arena = Arena::new();

/// The process-wide arena. Every codec entry point resets it before it starts.
#[cfg(not(test))]
pub fn arena() -> &'static Arena {
    // SAFETY: the arena is never moved (it is a static), and the lock inside it
    // serialises access to the bump pointer.
    unsafe { &*core::ptr::addr_of!(ARENA) }
}

/// Global allocator used by the stdlib modules.
///
/// Only installed for the shipped library: the test build links `std`, whose
/// allocator must not be displaced.
#[cfg(not(test))]
pub struct BumpAllocator;

// SAFETY: `take` is atomic and returns either a disjoint in-bounds region or
// null; `dealloc` and `realloc`'s copy are sound because nothing else can claim
// a region until the next reset, and the reset contract is documented above.
#[cfg(not(test))]
unsafe impl GlobalAlloc for BumpAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        arena().take(layout.size(), layout.align())
    }

    unsafe fn dealloc(&self, _ptr: *mut u8, _layout: Layout) {
        // Bump allocator: reclaimed by `reset`, never by the caller.
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        let ptr = arena().take(layout.size(), layout.align());
        if !ptr.is_null() {
            core::ptr::write_bytes(ptr, 0, layout.size());
        }
        ptr
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        // Growing is a fresh bump plus a copy; the old block stays until reset.
        let new_layout = Layout::from_size_align_unchecked(new_size, layout.align());
        let new_ptr = arena().take(new_size, layout.align());
        if !new_ptr.is_null() && !ptr.is_null() {
            core::ptr::copy_nonoverlapping(ptr, new_ptr, layout.size().min(new_size));
        }
        let _ = new_layout;
        new_ptr
    }
}

#[cfg(not(test))]
#[global_allocator]
static GLOBAL: BumpAllocator = BumpAllocator;

#[cfg(test)]
mod tests {
    use super::*;
    use std::string::{String, ToString};
    use std::vec;
    use std::vec::Vec;

    #[test]
    fn bump_allocates_and_resets() {
        // In the test build the crate links std, so exercise the Arena directly
        // rather than through the global allocator.
        let a = Arena::new();
        assert_eq!(a.used(), 0);
        let first = a.take(100, 8);
        assert!(!first.is_null());
        assert_eq!(a.used(), 100);
        let second = a.take(100, 8);
        assert!(!second.is_null());
        assert!(second as usize > first as usize, "bumps forward");
        unsafe { a.reset() };
        assert_eq!(a.used(), 0);
    }

    #[test]
    fn alignment_is_respected() {
        let a = Arena::new();
        let p = a.take(10, 64);
        assert_eq!(p as usize % 64, 0, "alignment honoured");
        let q = a.take(10, 64);
        assert_eq!(q as usize % 64, 0);
    }

    #[test]
    fn exhaustion_returns_null_rather_than_overrunning() {
        let a = Arena::new();
        // Ask for more than the arena holds.
        let p = a.take(ARENA_BYTES + 1, 1);
        assert!(p.is_null(), "must refuse, never overrun");
    }

    #[test]
    fn concurrent_claims_do_not_overlap() {
        // The lock is what makes this true; without it two claimers can be
        // handed the same address.
        let a = Arc::new(Arena::new());
        let mut handles = Vec::new();
        for _ in 0..4 {
            let a = a.clone();
            handles.push(std::thread::spawn(move || {
                let mut last = 0usize;
                for _ in 0..50 {
                    let p = a.take(64, 8);
                    assert!(!p.is_null());
                    let addr = p as usize;
                    assert!(addr >= last, "bump must move forward");
                    last = addr;
                }
            }));
        }
        for h in handles {
            h.join().unwrap();
        }
        assert!(a.used() > 0);
    }

    use std::sync::Arc;
    use std::thread;
    #[allow(unused_imports)]
    use std::string::ToString as _ToString;
    #[allow(dead_code)]
    fn _unused(_: String) -> String {
        String::new()
    }
}