// cqma leak tracker — allocator hooks for AddressSanitizer.
//
// LeakSanitizer is not built into the Android/Termux sanitizer runtime, so
// cqma supplies its own leak accounting:
//
//   * __sanitizer_malloc_hook / __sanitizer_free_hook are called by ASan for
//     every user allocation (malloc, calloc, realloc, new, strdup, ...).
//   * every live block is recorded in a fixed-size, mmap-backed hash table
//     together with its allocation stack (no malloc recursion).
//   * at exit the process is scanned conservatively, LSan-style: writable
//     PT_LOAD segments of every module plus the main thread stack. A block
//     that no scanned word points at (interior pointers count) is reported
//     as unreachable — a real leak, not a still-reachable cache.
//
// This file is compiled with -fno-sanitize=all and linked into the analyzed
// ASan binary. It deliberately avoids malloc, stdio and C++ in the reporting
// path.
//
// Environment:
//   CQMA_LEAK_REPORT  path to write the report (default: stderr)
//   CQMA_LEAK_ROOT    project root; leaks whose stack has no frame under it
//                     are ignored (keeps libc/ASan startup allocations out)
//   CQMA_LEAK_SLOTS   hash-table slots (default 262144)
//   CQMA_LEAK_DISABLE set to 1 to disable tracking

#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#else
#include <link.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define CQMA_FRAMES 14
#define CQMA_MAX_GROUPS 4096

typedef struct {
    void *ptr;
    size_t size;
    uint32_t nframes;
    uint8_t state;      // 0 = empty, 1 = live, 2 = tombstone
    uint8_t reachable;
    uint16_t pad;
    void *frames[CQMA_FRAMES];
} slot_t;

typedef struct {
    uintptr_t start;
    uintptr_t end;
    uint32_t slot;
    uint32_t pad;
} range_t;

typedef struct {
    char top[320];
    char detail[4][256];
    size_t bytes;
    size_t count;
    uint32_t frames;
} group_t;

static slot_t *g_slots;
static size_t g_cap;
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
// Bionic requires the executable's TLS segment to be 64-byte aligned on
// aarch64; a plain int would leave it at 4 and the loader would abort.
static __thread int g_in_hook __attribute__((aligned(64)));
static volatile int g_tracking = 1;
static int g_disabled;
static int g_debug_scan;
static uint64_t g_total_allocs;
static uint64_t g_live_count;
static uint64_t g_live_bytes;
static uintptr_t g_min_addr = UINTPTR_MAX;
static uintptr_t g_max_addr;

extern void __sanitizer_symbolize_pc(void *, const char *, char *, size_t)
    __attribute__((weak));

static void *map_zero(size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static uint64_t hash_ptr(const void *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static int ensure_table(void) {
    if (g_slots) {
        return 1;
    }
    long slots = 262144;
    const char *env = getenv("CQMA_LEAK_SLOTS");
    if (env && *env) {
        long value = strtol(env, NULL, 10);
        if (value >= 1024) {
            slots = value;
        }
    }
    size_t bytes = (size_t)slots * sizeof(slot_t);
    void *memory = map_zero(bytes);
    if (!memory) {
        g_disabled = 1;
        return 0;
    }
    g_slots = (slot_t *)memory;
    g_cap = (size_t)slots;
    return 1;
}

static void lock_table(void) {
    while (atomic_flag_test_and_set_explicit(&g_lock, memory_order_acquire)) {
        // spin; the critical section is a few instructions
    }
}

static void unlock_table(void) {
    atomic_flag_clear_explicit(&g_lock, memory_order_release);
}

static void track_alloc(void *ptr, size_t size) {
    if (!ptr || !g_tracking || g_disabled || g_in_hook) {
        return;
    }
    g_in_hook = 1;
    if (!ensure_table()) {
        g_in_hook = 0;
        return;
    }
    void *frames[CQMA_FRAMES];
    int nframes = backtrace(frames, CQMA_FRAMES);

    lock_table();
    if (g_live_count + 1 > g_cap - (g_cap / 4)) {
        g_disabled = 1;
        unlock_table();
        g_in_hook = 0;
        return;
    }
    size_t mask = g_cap - 1;
    size_t index = (size_t)(hash_ptr(ptr) & mask);
    size_t tombstone = SIZE_MAX;
    slot_t *slot = NULL;
    for (size_t probes = 0; probes <= mask; probes++) {
        slot_t *candidate = &g_slots[index];
        if (candidate->state == 0) {
            slot = (tombstone != SIZE_MAX) ? &g_slots[tombstone] : candidate;
            break;
        }
        if (candidate->state == 2) {
            if (tombstone == SIZE_MAX) {
                tombstone = index;
            }
        } else if (candidate->ptr == ptr) {
            slot = candidate;
            break;
        }
        index = (index + 1) & mask;
    }
    if (!slot) {
        slot = (tombstone != SIZE_MAX) ? &g_slots[tombstone] : NULL;
    }
    if (!slot) {
        g_disabled = 1;
        unlock_table();
        g_in_hook = 0;
        return;
    }
    if (slot->state != 1) {
        g_live_count++;
        g_live_bytes += size;
    } else {
        g_live_bytes = g_live_bytes - slot->size + size;
    }
    slot->ptr = ptr;
    slot->size = size;
    slot->nframes = (uint32_t)(nframes < 0 ? 0 : nframes);
    slot->state = 1;
    slot->reachable = 0;
    for (int i = 0; i < nframes; i++) {
        slot->frames[i] = frames[i];
    }
    g_total_allocs++;
    uintptr_t address = (uintptr_t)ptr;
    if (address < g_min_addr) {
        g_min_addr = address;
    }
    if (address + size > g_max_addr) {
        g_max_addr = address + size;
    }
    unlock_table();
    g_in_hook = 0;
}

static void track_free(void *ptr) {
    if (!ptr || !g_tracking || g_disabled || g_in_hook || !g_slots) {
        return;
    }
    g_in_hook = 1;
    lock_table();
    size_t mask = g_cap - 1;
    size_t index = (size_t)(hash_ptr(ptr) & mask);
    for (size_t probes = 0; probes <= mask; probes++) {
        slot_t *slot = &g_slots[index];
        if (slot->state == 0) {
            break;
        }
        if (slot->state == 1 && slot->ptr == ptr) {
            g_live_count--;
            g_live_bytes -= slot->size;
            slot->state = 2;
            slot->ptr = NULL;
            slot->size = 0;
            slot->nframes = 0;
            break;
        }
        index = (index + 1) & mask;
    }
    unlock_table();
    g_in_hook = 0;
}

void __sanitizer_malloc_hook(void *ptr, size_t size) {
    track_alloc(ptr, size);
}

void __sanitizer_free_hook(void *ptr) {
    track_free(ptr);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

static int is_internal(const char *frame) {
    static const char *needles[] = {
        "<null>", "__asan", "__sanitizer", "__interceptor", "__ubsan",
        "libllvm/src/compiler-rt", "/apex/", "asan_malloc", "asan_free",
        "operator new", "operator delete", "track_alloc", "track_free",
        "cqma_leak", "backtrace",
    };
    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
        if (strstr(frame, needles[i])) {
            return 1;
        }
    }
    const char *space = strrchr(frame, ' ');
    const char *function = space ? space + 1 : frame;
    static const char *exact[] = {
        "malloc", "calloc", "realloc", "free", "reallocarray", "memalign",
        "aligned_alloc", "posix_memalign", "strdup", "strndup", "asprintf",
        "vasprintf",
    };
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        if (strcmp(function, exact[i]) == 0) {
            return 1;
        }
    }
    return strncmp(function, "pthread_", 8) == 0;
}

static void symbolize(void *pc, char *out, size_t out_size) {
    out[0] = '\0';
    if (__sanitizer_symbolize_pc) {
        char location[256] = {0};
        char function[192] = {0};
        __sanitizer_symbolize_pc(pc, "%S", location, sizeof(location));
        __sanitizer_symbolize_pc(pc, "%f", function, sizeof(function));
        if (location[0] && function[0]) {
            snprintf(out, out_size, "%s %s", location, function);
        } else if (location[0]) {
            snprintf(out, out_size, "%s", location);
        } else if (function[0]) {
            snprintf(out, out_size, "? %s", function);
        } else {
            snprintf(out, out_size, "? %p", pc);
        }
        return;
    }
    Dl_info info;
    if (dladdr(pc, &info) && info.dli_fname) {
        uintptr_t offset = (uintptr_t)pc - (uintptr_t)info.dli_fbase;
        snprintf(out, out_size, "%s+0x%lx", info.dli_fname,
                 (unsigned long)offset);
    } else {
        snprintf(out, out_size, "? %p", pc);
    }
}

static int under_root(const char *location, const char *root) {
    if (!root || !*root) {
        return 1;
    }
    return strncmp(location, root, strlen(root)) == 0;
}

// Transitive marking. A block that is reachable is not necessarily a *root*:
// it can hold the only pointers to other blocks (a hash table, a linked list,
// Flint's quarantine ring). LeakSanitizer follows those chains, and so must we
// — otherwise every container's contents look "lost" and the report fills with
// false positives.
static uint32_t *g_work;
static size_t g_work_len;
static size_t g_work_cap;

static void mark_block(uint32_t index) {
    slot_t *slot = &g_slots[index];
    if (slot->reachable) {
        return;
    }
    slot->reachable = 1;
    if (g_work_len < g_work_cap) {
        g_work[g_work_len++] = index;
    }
}

static void scan_words(uintptr_t low, uintptr_t high, const range_t *ranges,
                       size_t count) {
    uintptr_t step = sizeof(uintptr_t);
    uintptr_t cursor = (low + step - 1) & ~(step - 1);
    for (; cursor + step <= high; cursor += step) {
        uintptr_t word = *(volatile uintptr_t *)cursor;
        if (word < g_min_addr || word >= g_max_addr) {
            continue;
        }
        size_t lo = 0;
        size_t hi = count;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (ranges[mid].start <= word) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == 0) {
            continue;
        }
        const range_t *candidate = &ranges[lo - 1];
        if (word < candidate->end) {
            slot_t *slot = &g_slots[candidate->slot];
            if (g_debug_scan && !slot->reachable) {
                char line[256];
                int n = snprintf(line, sizeof(line),
                                 "REACH scan=%p word=%p block=%p size=%zu\n",
                                 (void *)cursor, (void *)word, slot->ptr,
                                 slot->size);
                if (n > 0) {
                    (void)!write(2, line, (size_t)n);
                }
            }
            mark_block(candidate->slot);
        }
    }
}

/// Follow pointers stored inside reachable blocks until the graph closes.
static void scan_transitively(const range_t *ranges, size_t count) {
    while (g_work_len > 0) {
        uint32_t index = g_work[--g_work_len];
        slot_t *slot = &g_slots[index];
        if (!slot->ptr || slot->size == 0) {
            continue;
        }
        uintptr_t low = (uintptr_t)slot->ptr;
        scan_words(low, low + slot->size, ranges, count);
    }
}

static void scan_stack(const range_t *ranges, size_t count) {
    uintptr_t stack_low = (uintptr_t)__builtin_frame_address(0);
    uintptr_t stack_base = 0;
    size_t stack_size = 0;
#if defined(__APPLE__)
    stack_base = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
    stack_size = (size_t)pthread_get_stacksize_np(pthread_self());
    stack_base -= stack_size;
#else
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void *address = NULL;
        size_t size = 0;
        if (pthread_attr_getstack(&attr, &address, &size) == 0) {
            stack_base = (uintptr_t)address;
            stack_size = size;
        }
        pthread_attr_destroy(&attr);
    }
#endif
    if (stack_base && stack_size && stack_base + stack_size > stack_low) {
        scan_words(stack_low, stack_base + stack_size, ranges, count);
    }
}
#if !defined(__APPLE__)
struct scan_context {
    const range_t *ranges;
    size_t count;
};

static int scan_module(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct scan_context *context = (struct scan_context *)data;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *header = &info->dlpi_phdr[i];
        if (header->p_type != PT_LOAD || !(header->p_flags & PF_W)) {
            continue;
        }
        uintptr_t low = (uintptr_t)info->dlpi_addr + header->p_vaddr;
        uintptr_t high = low + header->p_memsz;
        scan_words(low, high, context->ranges, context->count);
    }
    return 0;
}
#else
static void scan_globals(const range_t *ranges, size_t count) {
    uint32_t images = _dyld_image_count();
    for (uint32_t i = 0; i < images; i++) {
        const struct mach_header *header = _dyld_get_image_header(i);
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
        if (!header || header->magic != MH_MAGIC_64) {
            continue;
        }
        const struct mach_header_64 *mach = (const struct mach_header_64 *)header;
        const struct segment_command_64 *segment =
            (const struct segment_command_64 *)(mach + 1);
        for (uint32_t j = 0; j < mach->ncmds; j++) {
            if (segment->cmd == LC_SEGMENT_64 &&
                (segment->initprot & VM_PROT_WRITE)) {
                uintptr_t low = (uintptr_t)(segment->vmaddr + slide);
                scan_words(low, low + segment->vmsize, ranges, count);
            }
            segment = (const struct segment_command_64 *)((const char *)segment +
                                                          segment->cmdsize);
        }
    }
}
#endif

static int range_compare(const void *a, const void *b) {
    const range_t *left = (const range_t *)a;
    const range_t *right = (const range_t *)b;
    if (left->start < right->start) return -1;
    if (left->start > right->start) return 1;
    return 0;
}

static int group_compare(const void *a, const void *b) {
    const group_t *left = (const group_t *)a;
    const group_t *right = (const group_t *)b;
    if (left->bytes < right->bytes) return 1;
    if (left->bytes > right->bytes) return -1;
    return 0;
}

static size_t append(char *buffer, size_t used, size_t capacity,
                     const char *text) {
    size_t length = strlen(text);
    if (used + length + 1 > capacity) {
        length = capacity - used - 1;
    }
    memcpy(buffer + used, text, length);
    used += length;
    buffer[used] = '\0';
    return used;
}

static void report(void) {
    if (!g_tracking || !g_slots || g_live_count == 0) {
        return;
    }
    g_tracking = 0;
    g_debug_scan = getenv("CQMA_LEAK_DEBUG_SCAN") != NULL;

    size_t live = (size_t)g_live_count;
    range_t *ranges = (range_t *)map_zero(live * sizeof(range_t));
    if (!ranges) {
        return;
    }
    size_t count = 0;
    for (size_t i = 0; i < g_cap; i++) {
        slot_t *slot = &g_slots[i];
        if (slot->state == 1) {
            ranges[count].start = (uintptr_t)slot->ptr;
            ranges[count].end = (uintptr_t)slot->ptr + slot->size;
            ranges[count].slot = (uint32_t)i;
            count++;
        }
    }
    qsort(ranges, count, sizeof(range_t), range_compare);

    g_work = (uint32_t *)map_zero(count * sizeof(uint32_t));
    g_work_cap = g_work ? count : 0;
    g_work_len = 0;

    if (getenv("CQMA_LEAK_DEBUG_ALL")) {
        for (size_t i = 0; i < count; i++) {
            slot_t *slot = &g_slots[ranges[i].slot];
            char text[256] = {0};
            for (uint32_t f = 0; f < slot->nframes; f++) {
                symbolize(slot->frames[f], text, sizeof(text));
                if (text[0] && text[0] != '?' && !is_internal(text)) {
                    break;
                }
                text[0] = '\0';
            }
            char line[400];
            int n = snprintf(line, sizeof(line), "ALL size=%zu frame=[%s]\n",
                             slot->size, text);
            if (n > 0) {
                (void)!write(2, line, (size_t)n);
            }
        }
    }

    // Stack scanning is deliberately opt-in: at exit, dead frames still hold
    // stale pointers to leaked blocks, which would hide exactly the leaks we
    // want to report. Globals are the reliable "still reachable" roots.
    if (getenv("CQMA_LEAK_SCAN_STACK")) {
        scan_stack(ranges, count);
    }
#if !defined(__APPLE__)
    if (!getenv("CQMA_LEAK_NO_GLOBALS")) {
        struct scan_context context = {ranges, count};
        dl_iterate_phdr(scan_module, &context);
    }
#else
    if (!getenv("CQMA_LEAK_NO_GLOBALS")) {
        scan_globals(ranges, count);
    }
#endif

    // Anything only reachable through an already-reachable block is still
    // live data, not a leak. Without this pass, every container's payload
    // would be falsely reported.
    scan_transitively(ranges, count);

    size_t unreachable = 0;
    size_t unreachable_bytes = 0;
    size_t reachable = 0;
    size_t reachable_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        slot_t *slot = &g_slots[ranges[i].slot];
        if (slot->reachable) {
            reachable++;
            reachable_bytes += slot->size;
        } else {
            unreachable++;
            unreachable_bytes += slot->size;
        }
    }

    const char *root = getenv("CQMA_LEAK_ROOT");
    int debug = getenv("CQMA_LEAK_DEBUG") != NULL;
    group_t *groups = (group_t *)map_zero(CQMA_MAX_GROUPS * sizeof(group_t));
    size_t group_count = 0;
    if (groups && unreachable > 0) {
        for (size_t i = 0; i < count; i++) {
            slot_t *slot = &g_slots[ranges[i].slot];
            if (slot->reachable) {
                continue;
            }
            char frames[CQMA_FRAMES][256];
            char top[320] = {0};
            uint32_t kept = 0;
            for (uint32_t f = 0; f < slot->nframes && kept < CQMA_FRAMES; f++) {
                symbolize(slot->frames[f], frames[kept], sizeof(frames[kept]));
                if (debug) {
                    char line[400];
                    int n = snprintf(line, sizeof(line), "DBG %p -> [%s]\n",
                                     slot->frames[f], frames[kept]);
                    if (n > 0) {
                        (void)!write(2, line, (size_t)n);
                    }
                }
                if (frames[kept][0] == '?' || is_internal(frames[kept])) {
                    continue;
                }
                if (!top[0] && under_root(frames[kept], root)) {
                    snprintf(top, sizeof(top), "%s", frames[kept]);
                }
                kept++;
            }
            if (!top[0]) {
                continue;  // allocation made entirely outside the project
            }
            group_t *group = NULL;
            for (size_t g = 0; g < group_count; g++) {
                if (strcmp(groups[g].top, top) == 0) {
                    group = &groups[g];
                    break;
                }
            }
            if (!group) {
                if (group_count >= CQMA_MAX_GROUPS) {
                    continue;
                }
                group = &groups[group_count++];
                snprintf(group->top, sizeof(group->top), "%s", top);
                group->frames = kept < 4 ? kept : 4;
                for (uint32_t f = 0; f < group->frames; f++) {
                    snprintf(group->detail[f], sizeof(group->detail[f]), "%s",
                             frames[f]);
                }
            }
            group->bytes += slot->size;
            group->count++;
        }
        qsort(groups, group_count, sizeof(group_t), group_compare);
    }

    char buffer[65536];
    size_t used = 0;
    used = append(buffer, used, sizeof(buffer), "CQMA-LEAK-TRACKER 1\n");
    char line[256];
    snprintf(line, sizeof(line),
             "summary live=%zu live_bytes=%zu reachable=%zu reachable_bytes=%zu "
             "unreachable=%zu unreachable_bytes=%zu tracked=%llu\n",
             live, (size_t)g_live_bytes, reachable, reachable_bytes, unreachable,
             unreachable_bytes, (unsigned long long)g_total_allocs);
    used = append(buffer, used, sizeof(buffer), line);
    for (size_t g = 0; g < group_count; g++) {
        snprintf(line, sizeof(line), "LEAK|%zu|%zu|", groups[g].bytes,
                 groups[g].count);
        used = append(buffer, used, sizeof(buffer), line);
        used = append(buffer, used, sizeof(buffer), groups[g].top);
        for (uint32_t f = 1; f < groups[g].frames; f++) {
            used = append(buffer, used, sizeof(buffer), ";");
            used = append(buffer, used, sizeof(buffer), groups[g].detail[f]);
        }
        used = append(buffer, used, sizeof(buffer), "\n");
        if (used > sizeof(buffer) - 1024) {
            break;
        }
    }

    const char *path = getenv("CQMA_LEAK_REPORT");
    int fd = path && *path ? open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600) : 2;
    if (fd >= 0) {
        size_t offset = 0;
        while (offset < used) {
            ssize_t written = write(fd, buffer + offset, used - offset);
            if (written <= 0) {
                break;
            }
            offset += (size_t)written;
        }
        if (fd != 2) {
            close(fd);
        }
    }
}

__attribute__((destructor)) static void cqma_leak_report(void) {
    report();
}
