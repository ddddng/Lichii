// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The dispatch boundary between a guest NVN call (its registers) and the front end's
// implementation: one constant offset turns the game's addresses into host pointers into
// Lichii's fastmem arena (object pointers, data pointers, buffer / texture addresses, command
// handles, and the addresses inside the arrays listed in gen_dispatch.py) and turns returned
// host pointers back. Objects and command records stay in place in the game's memory (nxnative's
// model). Values at or past the guest address space are host addresses the front end made itself
// (host-lent command segments, the shared empty recording, virtual pool ranges): they pass both
// ways unchanged.

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "nvn_count.h"
#include "nvn_direct.h"
#include "nvn_direct_front.h"
#include "nvn_protos.h"

typedef struct { NVNbufferAddress data; const void* control; uint64_t ext; } shader_data_ext;
typedef struct { NVNmemoryPool* phys; int64_t poff, voff; uint64_t size; int32_t storage_class, pad; } mapping_request;
_Static_assert(sizeof(mapping_request) == 0x28, "NVNmappingRequest");

static uintptr_t g_arena;
static uint64_t g_guest_size;

static inline uint64_t host_addr(uint64_t a) {
    return a != 0 && a < g_guest_size ? (uint64_t)g_arena + a : a;
}
static inline void* host_ptr(uint64_t a) { return (void*)(uintptr_t)host_addr(a); }
static inline uint64_t guest_addr(uint64_t h) {
    return h >= g_arena && h - g_arena < g_guest_size ? h - g_arena : h;
}
static inline float F_float(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }
static inline double F_double(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline uint64_t R_float(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline uint64_t R_double(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

// Host copies of the arrays a call passes, per thread: at most two arrays per call (gen_dispatch.py
// checks), each copy valid until the thread's next call. kept: the front end keeps the array
// past the call (a program builder's shader list): a copy of its own, never freed.
static _Thread_local struct { void* p; size_t cap; } g_scratch[2];
static void* scratch(int k, size_t bytes, int kept) {
    if (kept) return malloc(bytes ? bytes : 1);
    if (g_scratch[k].cap < bytes) {
        free(g_scratch[k].p);
        g_scratch[k].cap = bytes < 256 ? 256 : bytes;
        g_scratch[k].p = malloc(g_scratch[k].cap);
    }
    return g_scratch[k].p;
}
static void* xl_pointers(uint64_t a, int n, int k, int kept) {
    if (!a || n <= 0) return host_ptr(a);
    const uint64_t* in = host_ptr(a);
    uint64_t* out = scratch(k, sizeof *out * (size_t)n, kept);
    for (int i = 0; i < n; i++) out[i] = host_addr(in[i]);
    return out;
}
static void* xl_handles(uint64_t a, int n, int k, int kept) { return xl_pointers(a, n, k, kept); }
static void* xl_buffer_ranges(uint64_t a, int n, int k, int kept) {
    if (!a || n <= 0) return host_ptr(a);
    const NVNbufferRange* in = host_ptr(a);
    NVNbufferRange* out = scratch(k, sizeof *out * (size_t)n, kept);
    for (int i = 0; i < n; i++) out[i] = (NVNbufferRange){host_addr(in[i].address), in[i].size};
    return out;
}
static void* xl_shader_data(uint64_t a, int n, int k, int kept) {
    if (!a || n <= 0) return host_ptr(a);
    const NVNshaderData* in = host_ptr(a);
    NVNshaderData* out = scratch(k, sizeof *out * (size_t)n, kept);
    for (int i = 0; i < n; i++) out[i] = (NVNshaderData){host_addr(in[i].data), host_ptr((uint64_t)(uintptr_t)in[i].control)};
    return out;
}
static void* xl_shader_data_ext(uint64_t a, int n, int k, int kept) {
    if (!a || n <= 0) return host_ptr(a);
    const shader_data_ext* in = host_ptr(a);
    shader_data_ext* out = scratch(k, sizeof *out * (size_t)n, kept);
    for (int i = 0; i < n; i++) out[i] = (shader_data_ext){host_addr(in[i].data), host_ptr((uint64_t)(uintptr_t)in[i].control), in[i].ext};
    return out;
}
static void* xl_mapping_requests(uint64_t a, int n, int k, int kept) {
    if (!a || n <= 0) return host_ptr(a);
    const mapping_request* in = host_ptr(a);
    mapping_request* out = scratch(k, sizeof *out * (size_t)n, kept);
    for (int i = 0; i < n; i++) {
        out[i] = in[i];
        out[i].phys = host_ptr((uint64_t)(uintptr_t)in[i].phys);
    }
    return out;
}

typedef uint64_t (*dispatch_fn)(void* fn, const uint64_t* x, const uint64_t* v, uint64_t* v0);
#define D(name, flags, body)                                                                       \
    static uint64_t d_##name(void* fn, const uint64_t* x, const uint64_t* v, uint64_t* v0) {      \
        (void)x; (void)v; (void)v0;                                                                \
        body                                                                                       \
    }
#include "nvn_dispatch.inc"
#undef D
typedef struct { const char* name; dispatch_fn d; int flags; } dispatch_entry;
#define D(name, flags, body) {#name, d_##name, flags},
static const dispatch_entry g_dispatch[] = {
#include "nvn_dispatch.inc"
};
#undef D

// The front end's slot table (hle_nvn.c)
typedef struct { _Atomic uint64_t cnt; void* fn; } nvn_slot_t;
extern nvn_slot_t nvn_slot[NVN_NFUNC];
extern void* nvn_big_real[NVN_NFUNC];
extern void* nvn_real[NVN_NFUNC];
extern const char* nvn_names[NVN_NFUNC];
void nvn_unimpl_entry(void);
void nvn_prof_entry(void);
void nvn_big_entry(void);
void nvn_nops_register(void);
void* nvn_nop_fn(void);

_Thread_local uint64_t nvn_direct_guest_lr;
static struct { void* fn; const dispatch_entry* d; } g_slot[NVN_NFUNC];
// The empty implementations of names without a prototype (hle_nvn.c g_nops: the shading-rate calls):
// they take no arguments and return nothing.
static uint64_t d_empty(void* fn, const uint64_t* x, const uint64_t* v, uint64_t* v0) {
    (void)x; (void)v; (void)v0;
    ((void (*)(void))fn)();
    return 0;
}
static const dispatch_entry g_empty = {"(empty)", d_empty, 0};

void nvn_direct_init(uint8_t* arena, uint64_t guest_size) {
    g_arena = (uintptr_t)arena;
    g_guest_size = guest_size;
    nvn_nops_register();
    for (int i = 0; i < NVN_NFUNC; i++) {
        void* fn = nvn_slot[i].fn;
        if (fn == (void*)nvn_prof_entry) fn = nvn_real[i];
        if (fn == (void*)nvn_big_entry) fn = nvn_big_real[i];
        if (fn == (void*)nvn_unimpl_entry) fn = NULL;
        for (size_t k = 0; fn && k < sizeof g_dispatch / sizeof *g_dispatch; k++)
            if (!strcmp(g_dispatch[k].name, nvn_names[i])) g_slot[i].d = &g_dispatch[k];
        // Without a dispatch entry: an empty implementation, or a name the hook answers itself
        // (nvnBootstrapLoader, nvnDeviceGetProcAddress: gen_dispatch.py HOOK_OWN).
        if (fn && !g_slot[i].d && fn == nvn_nop_fn()) g_slot[i].d = &g_empty;
        g_slot[i].fn = g_slot[i].d ? fn : NULL;
    }
}

int nvn_direct_name_count(void) { return NVN_NFUNC; }
const char* nvn_direct_name(int slot) { return nvn_names[slot]; }
int nvn_direct_implemented(int slot) { return g_slot[slot].fn != NULL; }
int nvn_direct_float_args(int slot) {
    return g_slot[slot].d && (g_slot[slot].d->flags & (NVN_DISPATCH_FLOAT_ARGS | NVN_DISPATCH_FLOAT_RETURN));
}

uint64_t nvn_direct_call(int slot, const uint64_t x[8], const uint64_t v[8], uint64_t lr, uint64_t* v0) {
    nvn_direct_guest_lr = lr;
    return g_slot[slot].d->d(g_slot[slot].fn, x, v, v0);
}

// nvnDeviceGetInteger: the game's own NVN2 driver's answers, as Lichii captured them
// (hle_nvn.c asks dm_sdk_nvn2_item before its table).
enum { DEVICE_ITEMS = 0x100 };
static int g_item_known[DEVICE_ITEMS], g_item_value[DEVICE_ITEMS];
void nvn_direct_set_device_integer(unsigned item, int value) {
    if (item >= DEVICE_ITEMS) return;
    g_item_value[item] = value;
    g_item_known[item] = 1;
}
int dm_sdk_nvn2_item(unsigned item, int* out) {
    if (item >= DEVICE_ITEMS || !g_item_known[item]) return 0;
    *out = g_item_value[item];
    return 1;
}
