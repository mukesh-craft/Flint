// flint_aegis.h — AEGIS LEASES: generational memory-safety runtime for Flint.
//
// A lease is an int64 handle: (index into lease table). Each table slot carries
// a generation counter. Free/move retires the generation; any later use of the
// old lease panics instead of causing use-after-free/double-free (temporal
// safety). Indexed read/write also bounds-checks (spatial safety).
//
// This is the runtime half of the hybrid model. The compile-time half is the
// AegisChecker AST pass in src/main.cpp, which rejects provable
// use-after-free / double-free / free-while-borrowed before code runs.
// The runtime table catches everything the static pass cannot see:
// FFI-escaped leases, leases laundered through integers, dynamic frees.
#ifndef FLINT_AEGIS_H
#define FLINT_AEGIS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Allocate nbytes, register a lease, return the lease id (>0). Panics on OOM
// or absurd size. Memory is zero-initialized (no info leaks).
int64_t flint_aegis_alloc(int64_t nbytes);
// Retire a lease and free its memory. Returns 0. Panics on double-free,
// unknown lease, or free-while-borrowed (borrow count > 0).
int64_t flint_aegis_free(int64_t lease);
// i64-slot access with liveness + bounds checks. Panics with a precise
// message instead of UB. Slots are 8 bytes: idx in [0, len).
int64_t flint_aegis_read_i64(int64_t lease, int64_t idx);
void    flint_aegis_write_i64(int64_t lease, int64_t idx, int64_t val);
// Unchecked variants for hot loops (Vale-style skip-check dereference).
// Caller must have proven liveness+bounds (e.g. hoisted check). UB if wrong.
int64_t flint_aegis_read_i64_unchecked(int64_t lease, int64_t idx);
void    flint_aegis_write_i64_unchecked(int64_t lease, int64_t idx, int64_t val);
// Number of i64 slots. Panics on dead lease.
int64_t flint_aegis_len(int64_t lease);
// Non-panicking liveness query (weak-reference pattern): 1 live, 0 dead.
int64_t flint_aegis_check(int64_t lease);
// Move semantics at runtime: retires `lease`, returns a fresh lease id for
// the SAME allocation. Any use of the old id panics (use-after-move).
int64_t flint_aegis_move(int64_t lease);
// Borrow tracking: free() while borrowed panics. Borrow/unborrow nest.
int64_t flint_aegis_borrow(int64_t lease);
int64_t flint_aegis_unborrow(int64_t lease);
// Byte-level access (for buffers/strings-as-bytes). Bounds-checked.
int64_t flint_aegis_read_u8(int64_t lease, int64_t idx);
void    flint_aegis_write_u8(int64_t lease, int64_t idx, int64_t val);

// ---- UAF quarantine (S5) ----
//
// Leases cover only explicit flint_aegis_* allocations, so str/map/chan were
// invisible to the runtime backstop: a use-after-free through any of them read
// whatever the allocator handed out next. Rather than put a lease (and a
// mutex) behind every string operation -- which would tax the hot path -- the
// quarantine keeps freed blocks alive and poisoned for a while. A stale read
// then hits poison and traps, instead of silently returning another
// allocation's bytes.
//
// Off by default so the hot path costs one branch; enable with
// FLINT_AEGIS_QUARANTINE=1 (or a --asan-style build) for debugging and tests.

// Bytes of freed memory retained before the block is really released.
#define AEGIS_QUARANTINE_BYTES ((size_t)8 << 20)

// Record `ptr` as freed. Subsequent reads of it trap.
void flint_aegis_quarantine_free(void* ptr, size_t size);

// Non-zero when the quarantine is enabled.
int64_t flint_aegis_quarantine_enabled(void);

// Panic if `ptr` is currently quarantined (i.e. already freed). Call this on
// any pointer before dereferencing it.
void flint_aegis_check_live(void* ptr, const char* what);

#ifdef __cplusplus
}
#endif

#endif // FLINT_AEGIS_H
