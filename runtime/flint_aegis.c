// flint_aegis.c — AEGIS LEASES runtime (generational lease table).
//
// Model: lease id = (slot_index * 2^32) | generation-at-issue. The table slot
// holds the CURRENT generation. A lease is live iff slot.gen == lease.gen &&
// slot.live. Free/move bumps the generation, so stale leases can never
// validate — even if the slot is recycled for a later allocation (ABA-proof).
// Random-ish start generations per process make cross-run lease forgery
// impractical; within a run, 32-bit generations make wrap-around infeasible.
//
// Hardening: all sizes overflow-checked, 1 GiB per-allocation cap, 1M live
// lease cap, zero-init allocs, mutex-guarded table, saturating borrow counts.

#include "flint_aegis.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
// PORT(v0.20): pthread mutex guards the lease table. POSIX (Linux/macOS/
// Android) fine; Windows needs SRWLOCK/CRITICAL_SECTION or MinGW pthreads.
#include <pthread.h>

extern void flint_panic(const char* msg);
extern void flint_set_err(int64_t err);

#define AEGIS_MAX_BYTES   ((int64_t)1 << 30)  // 1 GiB per allocation
#define AEGIS_MAX_LEASES  ((int64_t)1 << 20)  // 1M live leases
#define AEGIS_INIT_CAP    256

typedef struct {
    void*     ptr;      // allocation base (NULL when slot empty)
    int64_t   size;     // bytes
    uint32_t  gen;      // current generation (never 0 when occupied)
    int       live;     // 1 while allocated
    int64_t   borrow;   // active borrow count (saturates, never wraps)
} AegisSlot;

static AegisSlot*  g_slots   = NULL;
static int64_t     g_cap     = 0;
static int64_t     g_live    = 0;
static int64_t     g_freelist = -1;  // singly-linked free stack via index reuse
static int64_t*    g_nextfree = NULL;
static uint32_t    g_gen_src = 0x9e3779b9u; // xorshift state for start generations
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static uint32_t aegis_rand32(void) {
    // xorshift32 — unpredictable start generations per slot.
    uint32_t x = g_gen_src ? g_gen_src : 0x85ebca6bu;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_gen_src = x ? x : 1u;
    return g_gen_src | 1u; // never 0
}

// Pack/unpack: high 32 = slot index, low 32 = generation.
static int64_t aegis_pack(int64_t slot, uint32_t gen) {
    return (slot << 32) | (int64_t)gen;
}
static int64_t aegis_slot_of(int64_t lease) { return lease >> 32; }
static uint32_t aegis_gen_of(int64_t lease) { return (uint32_t)(lease & 0xffffffffLL); }

static void aegis_lock(void)   { pthread_mutex_lock(&g_mtx); }
static void aegis_unlock(void) { pthread_mutex_unlock(&g_mtx); }

static void aegis_ensure_cap_locked(void) {
    if (g_cap > 0 && (g_live < g_cap || g_freelist >= 0)) return;
    int64_t ncap = g_cap ? g_cap * 2 : AEGIS_INIT_CAP;
    if (ncap > AEGIS_MAX_LEASES + 64) ncap = AEGIS_MAX_LEASES + 64;
    AegisSlot* ns = (AegisSlot*)realloc(g_slots, (size_t)ncap * sizeof(AegisSlot));
    int64_t* nn = (int64_t*)realloc(g_nextfree, (size_t)ncap * sizeof(int64_t));
    if (!ns || !nn) {
        free(ns); free(nn);
        aegis_unlock();
        flint_panic("aegis: lease table OOM");
        aegis_lock(); // unreachable, keeps analyzers calm
        return;
    }
    for (int64_t i = g_cap; i < ncap; i++) {
        ns[i].ptr = NULL; ns[i].size = 0; ns[i].gen = 0; ns[i].live = 0; ns[i].borrow = 0;
        nn[i] = -1;
    }
    g_slots = ns; g_nextfree = nn; g_cap = ncap;
}

static int64_t aegis_claim_slot_locked(void) {
    aegis_ensure_cap_locked();
    int64_t slot;
    if (g_freelist >= 0) {
        slot = g_freelist;
        g_freelist = g_nextfree[slot];
    } else {
        slot = -1;
        for (int64_t i = 0; i < g_cap; i++) {
            if (!g_slots[i].live && g_slots[i].ptr == NULL && g_slots[i].gen == 0) { slot = i; break; }
        }
        if (slot < 0) { return -1; } // table saturated
    }
    return slot;
}

// Look up a lease. Returns slot or -1. If `need_live`, dead/retired also -1.
// Must hold the lock. Never panics (callers decide panic vs query).
static int64_t aegis_lookup_locked(int64_t lease, int need_live) {
    if (lease <= 0) return -1;
    int64_t slot = aegis_slot_of(lease);
    uint32_t gen = aegis_gen_of(lease);
    if (slot < 0 || slot >= g_cap) return -1;
    AegisSlot* s = &g_slots[slot];
    if (s->gen != gen) return -1;          // retired generation (free/move/ABA)
    if (need_live && !s->live) return -1;
    if (!s->live || !s->ptr) return -1;
    return slot;
}

int64_t flint_aegis_alloc(int64_t nbytes) {
    if (nbytes <= 0 || nbytes > AEGIS_MAX_BYTES) {
        flint_set_err(1);
        flint_panic("aegis_alloc: size out of range (1..1GiB)");
    }
    void* p = calloc(1, (size_t)nbytes); // zero-init: no heap info leaks
    if (!p) flint_panic("aegis_alloc: OOM");
    aegis_lock();
    if (g_live >= AEGIS_MAX_LEASES) {
        aegis_unlock(); free(p);
        flint_panic("aegis_alloc: too many live leases");
    }
    int64_t slot = aegis_claim_slot_locked();
    if (slot < 0) {
        aegis_unlock(); free(p);
        flint_panic("aegis_alloc: lease table saturated");
    }
    uint32_t gen = g_slots[slot].gen ? g_slots[slot].gen + 1 : aegis_rand32();
    if (gen == 0) gen = 1;
    g_slots[slot].ptr = p;
    g_slots[slot].size = nbytes;
    g_slots[slot].gen = gen;
    g_slots[slot].live = 1;
    g_slots[slot].borrow = 0;
    g_live++;
    int64_t lease = aegis_pack(slot, gen);
    aegis_unlock();
    return lease;
}

int64_t flint_aegis_free(int64_t lease) {
    aegis_lock();
    int64_t slot = aegis_lookup_locked(lease, 0);
    if (slot < 0) {
        aegis_unlock();
        flint_panic("aegis_free: double-free or unknown lease");
    }
    AegisSlot* s = &g_slots[slot];
    if (!s->live) {
        aegis_unlock();
        flint_panic("aegis_free: double-free");
    }
    if (s->borrow > 0) {
        aegis_unlock();
        flint_panic("aegis_free: cannot free while borrowed");
    }
    free(s->ptr);
    s->ptr = NULL;
    s->live = 0;
    s->gen++;                       // retire generation: stale leases die here
    if (s->gen == 0) s->gen = 1;
    g_nextfree[slot] = g_freelist;  // recycle slot (new gen on reuse => ABA-proof)
    g_freelist = slot;
    g_live--;
    aegis_unlock();
    return 0;
}

int64_t flint_aegis_check(int64_t lease) {
    aegis_lock();
    int64_t ok = aegis_lookup_locked(lease, 1) >= 0 ? 1 : 0;
    aegis_unlock();
    return ok;
}

int64_t flint_aegis_len(int64_t lease) {
    aegis_lock();
    int64_t slot = aegis_lookup_locked(lease, 1);
    if (slot < 0) {
        aegis_unlock();
        flint_panic("aegis: use-after-free / use-after-move (dead lease)");
    }
    int64_t n = g_slots[slot].size / 8;
    aegis_unlock();
    return n;
}

int64_t flint_aegis_read_i64(int64_t lease, int64_t idx) {
    aegis_lock();
    int64_t slot = aegis_lookup_locked(lease, 1);
    if (slot < 0) {
        aegis_unlock();
        flint_panic("aegis_read: use-after-free / use-after-move (dead lease)");
    }
    int64_t n = g_slots[slot].size / 8;
    if (idx < 0 || idx >= n) {
        aegis_unlock();
        flint_panic("aegis_read: index out of bounds");
    }
    int64_t v = ((int64_t*)g_slots[slot].ptr)[idx];
    aegis_unlock();
    return v;
}

void flint_aegis_write_i64(int64_t lease, int64_t idx, int64_t val) {
    aegis_lock();
    int64_t slot = aegis_lookup_locked(lease, 1);
    if (slot < 0) {
        aegis_unlock();
        flint_panic("aegis_write: use-after-free / use-after-move (dead lease)");
    }
    int64_t n = g_slots[slot].size / 8;
    if (idx < 0 || idx >= n) {
        aegis_unlock();
        flint_panic("aegis_write: index out of bounds");
    }
    ((int64_t*)g_slots[slot].ptr)[idx] = val;
    aegis_unlock();
}

int64_t flint_aegis_read_i64_unchecked(int64_t lease, int64_t idx) {
    int64_t slot = aegis_slot_of(lease);
    // No validation: caller guarantees liveness+bounds (hot-loop opt-out).
    return ((int64_t*)g_slots[slot].ptr)[idx];
}

void flint_aegis_write_i64_unchecked(int64_t lease, int64_t idx, int64_t val) {
    int64_t slot = aegis_slot_of(lease);
    ((int64_t*)g_slots[slot].ptr)[idx] = val;
}

int64_t flint_aegis_move(int64_t lease) {
    aegis_lock();
    int64_t slot = aegis_lookup_locked(lease, 1);
    if (slot < 0) {
        aegis_unlock();
        flint_panic("aegis_move: cannot move dead lease (double-move?)");
    }
    AegisSlot* s = &g_slots[slot];
    if (s->borrow > 0) {
        aegis_unlock();
        flint_panic("aegis_move: cannot move while borrowed");
    }
    // Bump generation in place: same allocation, new lease id. Old id dies.
    s->gen++;
    if (s->gen == 0) s->gen = 1;
    int64_t fresh = aegis_pack(slot, s->gen);
    aegis_unlock();
    return fresh;
}

int64_t flint_aegis_borrow(int64_t lease) {
    aegis_lock();
    int64_t slot = aegis_lookup_locked(lease, 1);
    if (slot < 0) {
        aegis_unlock();
        flint_panic("aegis_borrow: cannot borrow dead lease");
    }
    if (g_slots[slot].borrow < INT64_MAX) g_slots[slot].borrow++;
    aegis_unlock();
    return lease; // borrow handle == lease (count tracked in table)
}

int64_t flint_aegis_unborrow(int64_t lease) {
    aegis_lock();
    int64_t slot = aegis_lookup_locked(lease, 0);
    if (slot >= 0 && g_slots[slot].borrow > 0) g_slots[slot].borrow--;
    aegis_unlock();
    return 0;
}

int64_t flint_aegis_read_u8(int64_t lease, int64_t idx) {
    aegis_lock();
    int64_t slot = aegis_lookup_locked(lease, 1);
    if (slot < 0) {
        aegis_unlock();
        flint_panic("aegis_read_u8: use-after-free / use-after-move (dead lease)");
    }
    if (idx < 0 || idx >= g_slots[slot].size) {
        aegis_unlock();
        flint_panic("aegis_read_u8: index out of bounds");
    }
    int64_t v = (unsigned char)((char*)g_slots[slot].ptr)[idx];
    aegis_unlock();
    return v;
}

void flint_aegis_write_u8(int64_t lease, int64_t idx, int64_t val) {
    aegis_lock();
    int64_t slot = aegis_lookup_locked(lease, 1);
    if (slot < 0) {
        aegis_unlock();
        flint_panic("aegis_write_u8: use-after-free / use-after-move (dead lease)");
    }
    if (idx < 0 || idx >= g_slots[slot].size) {
        aegis_unlock();
        flint_panic("aegis_write_u8: index out of bounds");
    }
    ((char*)g_slots[slot].ptr)[idx] = (char)(val & 0xff);
    aegis_unlock();
}
