// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The host functions Banana Native's NVN front end and Vulkan replay call (upstream's host in
// dm/host implements them for a game it runs natively), as Lichii has them: there is no native
// module table, cheat engine, frame generation or custom driver; the guest's memory is Lichii's.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvn_count.h"

uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
uint64_t g_t0_ns;
__attribute__((constructor)) static void start_time(void) { g_t0_ns = now_ns(); }
uint64_t guest_ns(void) { return now_ns() - g_t0_ns; }

// The front end's slot table names (upstream's names.txt, nvn_thunks.S): Lichii's hook calls the
// implementations itself, so the thunks and their entry stubs are never run.
const char* nvn_names[NVN_NFUNC] = {
#include "nvn_names.inc"
};
void* nvn_thunk[NVN_NFUNC];
void nvn_unimpl_entry(void) { abort(); }
void nvn_prof_entry(void) { abort(); }
void nvn_big_entry(void) { abort(); }

typedef struct module_t module_t;
module_t* g_mod;
int g_nmod;
module_t* module_of(const void* p) { (void)p; return NULL; }
const char* dm_module_dir(void) { return "."; }
void dm_backtrace(uint64_t fp) { (void)fp; }
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
void dm_cheats_present(void) {}
void crash_set_wait_dump(void (*fn)(void)) { (void)fn; }
void dm_host_shutdown(const char* why) {
    fprintf(stderr, "[nvn_direct] the replay stops the emulator: %s\n", why ? why : "");
    abort();
}
int front_screen_max_fps(void) { return 0; }
void dm_vi_layer_crop(int c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
void* vka_driver_open(void) { return NULL; }
const char* vka_driver_tag(void) { return NULL; }
void nxhost_framegen_set(int on, int multiplier, int target_rate, int flow_pct) {
    (void)on; (void)multiplier; (void)target_rate; (void)flow_pct;
}
struct vka_img;
int vka_fg_frame(uint64_t serial, const int* crop, double period, uint64_t fence, struct vka_img** gen,
                 double* hold, double* out_period) {
    (void)serial; (void)crop; (void)period; (void)fence; (void)gen; (void)hold;
    if (out_period) *out_period = 0;
    return 0;
}
int dm_sdk_ounce(void) { return 1; } // direct mode is for NVN2 (Switch 2) games


// Guest memory. The front end and the replay reach it through host pointers into Lichii's
// fastmem arena (the dispatch adds the arena's base to the game's addresses); every page they
// are given is mapped by the guest. Write tracking is Lichii's (dotfiles-25x.5): the replay's
// own fault and pre-write hooks are not installed.
void dm_fault_hook_register(int (*fn)(void* addr, int is_write, void* uc)) { (void)fn; }
void dm_guest_write_prep_register(void (*fn)(void* addr, size_t len)) { (void)fn; }
void dm_guest_write_prepare(void* addr, size_t len) { (void)addr; (void)len; }
uint64_t dm_guest_unmapped(uintptr_t a, size_t n) { (void)a; (void)n; return 0; }
uint64_t dm_guest_read_mapped(void* dst, uintptr_t a, size_t n) {
    memcpy(dst, (const void*)a, n);
    return 0;
}
// The placement swap's guest block (vka_mem.c): pools are not placed in Lichii.
void svc_guest_range(uint8_t** base, size_t* size) { *base = NULL; *size = 0; }

// Upstream's write-protect tracker (nvn_dirty.c, not built): nothing is tracked, no write is seen.
void nvn_dirty_add_range(const void* mem, size_t size) { (void)mem; (void)size; }
void nvn_dirty_remove_range(const void* mem) { (void)mem; }
void nvn_dirty_protect(const void* addr, size_t len) { (void)addr; (void)len; }
void nvn_dirty_hold(const void* addr, size_t len) { (void)addr; (void)len; }
void nvn_dirty_unhold(void) {}
uint64_t nvn_dirty_now(void) { return 0; }
int nvn_dirty_since(const void* addr, size_t len, uint64_t stamp) { (void)addr; (void)len; (void)stamp; return 0; }
const void* nvn_dirty_span(const void* addr, size_t len) { (void)addr; (void)len; return NULL; }
int nvn_dirty_since_span(const void* span, const void* addr, size_t len, uint64_t stamp) {
    (void)span; (void)addr; (void)len; (void)stamp;
    return 0;
}
int nvn_dirty_bounds_since(const void* span, const void* addr, size_t len, uint64_t stamp, size_t* lo, size_t* hi) {
    (void)span; (void)addr; (void)len; (void)stamp; (void)lo; (void)hi;
    return 0;
}
void nvn_dirty_stats(uint64_t* faults, uint64_t* preps) { *faults = *preps = 0; }
void nvn_dirty_mp_stats(uint64_t* calls, uint64_t* pages, uint64_t* ns) { *calls = *pages = *ns = 0; }
void nvn_dirty_debug(const void* span, const void* addr, size_t len, uint64_t stamp, char* out, size_t n) {
    (void)span; (void)addr; (void)len; (void)stamp;
    if (n) out[0] = 0;
}

// Upstream's present (vka_present.c, not built): Lichii presents (nvn_window.c).
struct tex_t;
uint64_t vka_present_reserve(void) { return 0; }
void vka_present_fill(uint64_t tk, const struct tex_t* t, double period_s, const int crop[4]) {
    (void)tk; (void)t; (void)period_s; (void)crop;
}
uint64_t vka_present_inline(const struct tex_t* t, double period_s, const int crop[4]) {
    (void)t; (void)period_s; (void)crop;
    return 0;
}
void mtl_present_flush(void) {}
