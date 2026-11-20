// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The dispatch's address translation (nvn_dispatch.c includes it once): a guest address below
// guest_size is the host pointer arena + address, and a returned host pointer in the arena goes back
// to address. Native Metal on hosts whose pages are larger than the guest's (16 KiB over 4 KiB) only
// finds a host page in the arena while its guest pages are contiguous in the backing memory
// (HostMemory::ArenaReadWritePages). An address on another host page becomes the backing pointer of
// its guest page instead, and a reverse map (backing page -> the guest page last translated to it)
// takes such pointers back. A translation the dispatch cannot make safely latches a failure: from
// then on no call runs (XL_FAILED) and the hook stops the session (nvn_direct_failure).

#pragma once

#include <stdint.h>

#include "nvn_direct_dispatch.h"

static uintptr_t g_arena;
static uint64_t g_guest_size;
static nvn_direct_memory g_memory;

#ifdef LICHII_NVN_METAL_BRIDGE
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define GUEST_PAGE_SHIFT 12
#define GUEST_PAGE_MASK ((uint64_t)0xfff)

/// Per 4 KiB page of the backing memory: 1 + the guest page last translated to it, 0 for none.
static _Atomic uint32_t* g_guest_page_of;
static atomic_int g_failed;
static atomic_int g_failure_ready;
static char g_failure[512];

#define XL_FAILED() atomic_load_explicit(&g_failed, memory_order_relaxed)

__attribute__((format(printf, 1, 2), noinline, cold)) static void xl_fail(const char* format, ...) {
    if (atomic_exchange(&g_failed, 1)) return;
    va_list args;
    va_start(args, format);
    vsnprintf(g_failure, sizeof g_failure, format, args);
    va_end(args);
    fprintf(stderr, "[lichii] NVN Metal: %s\n", g_failure);
    atomic_store_explicit(&g_failure_ready, 1, memory_order_release);
}

/// Whether every host page holding guest [a, a + size) is mapped read-write in the arena.
static inline int arena_mapped(uint64_t a, uint64_t size) {
    const unsigned shift = g_memory.arena_page_shift;
    uint64_t last = a + (size ? size - 1 : 0);
    if (last < a || last >= g_guest_size) last = g_guest_size - 1;
    for (uint64_t page = g_memory.arena_page_first + (a >> shift),
                  end = g_memory.arena_page_first + (last >> shift); page <= end; page++) {
        if (!(__atomic_load_n(&g_memory.arena_pages[page >> 6], __ATOMIC_RELAXED) >> (page & 63) & 1)) return 0;
    }
    return 1;
}

/// Guest [a, a + size) (size 0: unknown, its first byte) through the backing memory of its guest
/// pages, which must follow each other there. A guest page the game has not mapped keeps its arena
/// address, as on 4 KiB hosts (an access faults there as it would on the console).
__attribute__((noinline)) static uint64_t backing_addr(uint64_t a, uint64_t size) {
    const uint64_t first = a & ~GUEST_PAGE_MASK;
    uint8_t* const p = (uint8_t*)g_memory.backing(g_memory.context, first);
    if (!p) return (uint64_t)g_arena + a;
    const uint64_t pages = ((a & GUEST_PAGE_MASK) + (size ? size : 1) + GUEST_PAGE_MASK) >> GUEST_PAGE_SHIFT;
    const uint64_t offset = (uint64_t)(p - g_memory.backing_base);
    if (offset >= g_memory.backing_size || pages > (g_memory.backing_size - offset) >> GUEST_PAGE_SHIFT) {
        xl_fail("guest %#llx+%#llx: backing %p is outside the backing memory", (unsigned long long)a,
                (unsigned long long)size, (void*)p);
        return 0;
    }
    for (uint64_t i = 1; i < pages; i++) {
        if ((uint8_t*)g_memory.backing(g_memory.context, first + (i << GUEST_PAGE_SHIFT)) != p + (i << GUEST_PAGE_SHIFT)) {
            xl_fail("guest %#llx+%#llx is outside the fastmem arena (16 KiB host pages) and its page %#llx "
                    "does not follow the one before it in backing memory",
                    (unsigned long long)a, (unsigned long long)size,
                    (unsigned long long)(first + (i << GUEST_PAGE_SHIFT)));
            return 0;
        }
    }
    for (uint64_t i = 0; i < pages; i++) {
        atomic_store_explicit(&g_guest_page_of[(offset >> GUEST_PAGE_SHIFT) + i],
                              (uint32_t)((first >> GUEST_PAGE_SHIFT) + i + 1), memory_order_relaxed);
    }
    return (uint64_t)(uintptr_t)p + (a & GUEST_PAGE_MASK);
}

__attribute__((noinline)) static uint64_t guest_of_backing(uint64_t h) {
    const uint64_t offset = h - (uint64_t)(uintptr_t)g_memory.backing_base;
    const uint32_t page = atomic_load_explicit(&g_guest_page_of[offset >> GUEST_PAGE_SHIFT], memory_order_relaxed);
    if (!page) {
        xl_fail("host pointer %#llx (backing offset %#llx) was never translated from a guest address",
                (unsigned long long)h, (unsigned long long)offset);
        return 0;
    }
    return ((uint64_t)(page - 1) << GUEST_PAGE_SHIFT) | (offset & GUEST_PAGE_MASK);
}
#else
#define XL_FAILED() 0
#endif

/// The host pointer of guest [a, a + size) (size 0: unknown, its first byte). Values at or past the
/// guest address space are host addresses the front end made itself and stay as they are.
static inline uint64_t host_addr_n(uint64_t a, uint64_t size) {
    if (a == 0 || a >= g_guest_size) return a;
#ifdef LICHII_NVN_METAL_BRIDGE
    if (g_memory.arena_pages && !arena_mapped(a, size)) return backing_addr(a, size);
#else
    (void)size;
#endif
    return (uint64_t)g_arena + a;
}
static inline uint64_t host_addr(uint64_t a) { return host_addr_n(a, 0); }
static inline void* host_ptr(uint64_t a) { return (void*)(uintptr_t)host_addr(a); }
static inline void* host_ptr_n(uint64_t a, uint64_t size) { return (void*)(uintptr_t)host_addr_n(a, size); }
static inline uint64_t guest_addr(uint64_t h) {
    if (h >= g_arena && h - g_arena < g_guest_size) return h - g_arena;
#ifdef LICHII_NVN_METAL_BRIDGE
    if (g_guest_page_of && h - (uint64_t)(uintptr_t)g_memory.backing_base < g_memory.backing_size) {
        return guest_of_backing(h);
    }
#endif
    return h;
}
