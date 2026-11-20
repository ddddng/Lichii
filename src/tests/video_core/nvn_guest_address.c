// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// nvn_guest_address.h as native Metal uses it on a host with 16 KiB pages over 4 KiB guest pages:
// a guest page table over a small backing memory, and the arena's read-write host pages as
// HostMemory::ArenaReadWritePages reports them.

#define LICHII_NVN_METAL_BRIDGE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "video_core/nvn_direct/nvn_guest_address.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%d: %s\n", __LINE__, #expr); exit(1); } } while (0)

enum { GUEST_PAGE = 0x1000, HOST_PAGE = 0x4000, GUEST_SIZE = 0x100000, BACKING_SIZE = 0x40000 };
static _Alignas(HOST_PAGE) uint8_t backing[BACKING_SIZE];
static int64_t page_table[GUEST_SIZE / GUEST_PAGE]; // backing offset + 1 of each guest page, 0 unmapped
static uint64_t read_write[GUEST_SIZE / HOST_PAGE / 64];

static void* backing_of(void* context, uint64_t address) {
    (void)context;
    const int64_t entry = page_table[address / GUEST_PAGE];
    return entry ? backing + entry - 1 + address % GUEST_PAGE : NULL;
}

static void map(uint64_t guest, uint64_t offset) { page_table[guest / GUEST_PAGE] = (int64_t)offset + 1; }

// As HostMemory: a host page is in the arena while its four guest pages follow each other from a
// 16 KiB aligned backing offset.
static void update_arena(void) {
    memset(read_write, 0, sizeof read_write);
    for (uint64_t host = 0; host < GUEST_SIZE; host += HOST_PAGE) {
        const int64_t first = page_table[host / GUEST_PAGE];
        int in = first && (first - 1) % HOST_PAGE == 0;
        for (uint64_t i = 1; in && i < HOST_PAGE / GUEST_PAGE; i++) in = page_table[host / GUEST_PAGE + i] == first + (int64_t)(i * GUEST_PAGE);
        if (in) read_write[host / HOST_PAGE / 64] |= 1ull << (host / HOST_PAGE % 64);
    }
}

static void clear_failure(void) {
    atomic_store(&g_failed, 0);
    atomic_store(&g_failure_ready, 0);
    g_failure[0] = 0;
}

int main(void) {
    void* const arena = mmap(NULL, GUEST_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(arena != MAP_FAILED);
    g_arena = (uintptr_t)arena;
    g_guest_size = GUEST_SIZE;
    g_memory = (nvn_direct_memory){.backing = backing_of, .backing_fd = -1, .backing_base = backing,
                                   .backing_size = BACKING_SIZE, .arena_pages = read_write,
                                   .arena_page_first = 0, .arena_page_shift = 14};
    g_guest_page_of = calloc(BACKING_SIZE / GUEST_PAGE, sizeof *g_guest_page_of);
    CHECK(g_guest_page_of);
    const uint64_t b = (uint64_t)(uintptr_t)backing;

    // Heap: 0x10000-0x13fff congruent (arena); 0x14000 follows 0x13000 in the backing, but its
    // host page is not congruent.
    for (uint64_t i = 0; i < 5; i++) map(0x10000 + i * GUEST_PAGE, 0x4000 + i * GUEST_PAGE);
    map(0x15000, 0x20000);
    // A stack alias of heap pages at an address the game chose: contiguous, not 16 KiB aligned.
    map(0x40000, 0x1000);
    map(0x41000, 0x2000);
    // Pages that do not follow each other in the backing.
    map(0x50000, 0x30000);
    map(0x51000, 0x9000);
    update_arena();

    CHECK(host_addr(0x10010) == g_arena + 0x10010);
    CHECK(guest_addr(g_arena + 0x10010) == 0x10010);
    CHECK(host_addr_n(0x10010, 0x3ff0) == g_arena + 0x10010);
    // An object over the host page boundary into a page outside the arena: all of it through backing.
    CHECK(host_addr_n(0x13ff0, 0x20) == b + 0x7ff0);
    CHECK(guest_addr(b + 0x7ff0) == 0x13ff0);
    CHECK(guest_addr(b + 0x8008) == 0x14008);
    CHECK(host_addr(0x14010) == b + 0x8010);

    // The stack object of the crash: nxnative writes it through backing.
    const uint64_t pool = host_addr_n(0x40d20, 0x100);
    CHECK(pool == b + 0x1d20);
    memset((void*)(uintptr_t)pool, 0xab, 0x100);
    CHECK(backing[0x1d20] == 0xab && backing[0x1e1f] == 0xab);
    CHECK(guest_addr(pool) == 0x40d20);
    CHECK(host_addr_n(0x40ff0, 0x20) == b + 0x1ff0);
    CHECK(guest_addr(b + 0x2004) == 0x41004);
    CHECK(!XL_FAILED());

    // Not reachable as one piece: the call must not run.
    CHECK(host_addr_n(0x50ff8, 0x10) == 0);
    CHECK(XL_FAILED());
    CHECK(atomic_load(&g_failure_ready) && strstr(g_failure, "0x51000"));
    clear_failure();

    // A returned pointer into backing no translation produced.
    CHECK(guest_addr(b + 0x3c000) == 0);
    CHECK(XL_FAILED());
    clear_failure();

    // Another alias of the same heap page: the last translation wins.
    map(0x60000, 0x1000);
    update_arena();
    CHECK(host_addr(0x60004) == b + 0x1004);
    CHECK(guest_addr(b + 0x1004) == 0x60004);

    // A page the guest has not mapped keeps its arena address (it faults there as on 4 KiB hosts).
    CHECK(host_addr(0x70000) == g_arena + 0x70000);
    // Host values the front end made, and null, pass both ways.
    CHECK(host_addr(0) == 0 && host_addr(GUEST_SIZE + 5) == GUEST_SIZE + 5);
    CHECK(guest_addr(0) == 0 && guest_addr(0x123) == 0x123);

    // The game unmaps a neighbour guest page: its host page leaves the arena, and objects on it
    // are reached through backing from then on. Mapping it back returns them to the arena.
    page_table[0x12000 / GUEST_PAGE] = 0;
    update_arena();
    CHECK(host_addr(0x10010) == b + 0x4010);
    CHECK(guest_addr(b + 0x4010) == 0x10010);
    map(0x12000, 0x6000);
    update_arena();
    CHECK(host_addr(0x10010) == g_arena + 0x10010);
    CHECK(!XL_FAILED());

    // 4 KiB hosts (no read-write bits): the arena only, as before.
    g_memory.arena_pages = NULL;
    free(g_guest_page_of);
    g_guest_page_of = NULL;
    CHECK(host_addr_n(0x40d20, 0x100) == g_arena + 0x40d20);
    CHECK(guest_addr(b + 0x1d20) == b + 0x1d20);
    puts("nvn_guest_address_tests: ok");
    return 0;
}
