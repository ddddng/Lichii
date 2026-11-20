// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The dispatch boundary between a guest NVN call (its registers) and the front end's
// implementation: one constant offset turns the game's addresses into host pointers into
// Lichii's fastmem arena (object pointers, data pointers, buffer / texture addresses, command
// handles, and the addresses inside the arrays listed in gen_dispatch.py) and turns returned
// host pointers back. Objects and command records stay in place in the game's memory (nxnative's
// model). Values at or past the guest address space are host addresses the front end made itself
// (host-lent command segments, the shared empty recording, virtual pool ranges): they pass both
// ways unchanged. Native Metal on 16 KiB host pages reaches guest pages outside the arena through
// their backing memory (nvn_guest_address.h).

#ifndef LICHII_NVN_METAL_BRIDGE
#include <sys/mman.h>   // (Windows: bananative/shim/win32)
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "nvn_count.h"
#include "nvn_direct_dispatch.h"
#include "nvn_guest_address.h"
#include "nvn_protos.h"
#ifndef LICHII_NVN_METAL_BRIDGE
#include "nvn_direct_front.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#endif

typedef struct { NVNbufferAddress data; const void* control; uint64_t ext; } shader_data_ext;
typedef struct { NVNmemoryPool* phys; int64_t poff, voff; uint64_t size; int32_t storage_class, pad; } mapping_request;
_Static_assert(sizeof(mapping_request) == 0x28, "NVNmappingRequest");

uint64_t nvn_direct_guest_of(uint64_t host) { return guest_addr(host); }
uint64_t nvn_direct_host_of(uint64_t guest) { return host_addr(guest); }
#ifdef LICHII_NVN_METAL_BRIDGE
uint64_t nvn_direct_host_object(uint64_t guest, uint64_t size) { return host_addr_n(guest, size); }
const char* nvn_direct_failure(void) {
    return atomic_load_explicit(&g_failure_ready, memory_order_acquire) ? g_failure : NULL;
}
// nvnMemoryPoolInitialize (nxnative-pool-lease.patch): a pool's storage must be in the arena on every
// host page. nxnative finds pools by GPU address (= CPU address) and the game gets those addresses
// back through guest_addr, so a pool reached through backing memory would not be found again from
// the arena addresses of its other pages; the zero-copy lease needs the same 16 KiB alignment anyway.
int nvn_direct_pool_storage(const void* mem, uint64_t size) {
    if (!g_memory.arena_pages || !size) return 1;
    const uint64_t guest = guest_addr((uint64_t)(uintptr_t)mem);
    if (guest == 0 || guest >= g_guest_size || size > g_guest_size - guest) {
        xl_fail("memory pool %p+%#llx: storage is not guest memory", mem, (unsigned long long)size);
        return 0;
    }
    const unsigned shift = g_memory.arena_page_shift;
    const uint64_t first = guest >> shift, last = (guest + size - 1) >> shift;
    uint64_t unmapped = 0;
    for (uint64_t page = first; page <= last; page++) unmapped += !arena_mapped(page << shift, 1);
    if (!unmapped && (uintptr_t)mem == g_arena + guest) return 1;
    xl_fail("memory pool guest %#llx+%#llx: %llu of %llu host pages (%#x bytes) are not in the fastmem arena; "
            "pools need storage whose guest pages are contiguous and aligned in backing memory",
            (unsigned long long)guest, (unsigned long long)size, (unsigned long long)unmapped,
            (unsigned long long)(last - first + 1), 1u << shift);
    return 0;
}
#endif
#ifndef LICHII_NVN_METAL_BRIDGE
void* nvn_direct_map_view(uint64_t address, uint64_t size, uint64_t* unmapped) {
    *unmapped = 0;
    return g_memory.map_view ? g_memory.map_view(g_memory.context, address, size, unmapped) : NULL;
}
void nvn_direct_unmap_view(void* view, uint64_t size) { g_memory.unmap_view(g_memory.context, view, size); }
void nvn_direct_track_pages(uint64_t address, uint64_t size, int armed) {
    if (g_memory.track_pages) g_memory.track_pages(g_memory.context, address, size, armed);
}
int nvn_direct_native_image(uint64_t handle, uint64_t* address, uint64_t* size) {
    return g_memory.native_image ? g_memory.native_image(g_memory.context, handle, address, size) : 0;
}
int nvn_direct_backing_file(uint64_t address, uint64_t* offset) {
    if (g_memory.backing_fd < 0 || !g_memory.backing) return -1;
    const uint8_t* const backing = g_memory.backing(g_memory.context, address);
    if (!backing) return -1;
    *offset = (uint64_t)(backing - g_memory.backing_base);
    return g_memory.backing_fd;
}
#if defined(_WIN32)
// Windows: the range is one of HostMemory's alias regions, and each run of pages that follow each
// other in the backing memory is one view of its section.
void* nvn_direct_alias_reserve(uint64_t size) { return g_memory.alias_reserve(g_memory.context, size); }
void nvn_direct_alias_unmap(void* at, uint64_t size) { g_memory.alias_unmap(g_memory.context, at, size); }
void nvn_direct_alias_release(void* range) { g_memory.alias_release(g_memory.context, range); }
uint64_t nvn_direct_alias_pages(void* to, uint64_t host, uint64_t size) {
    const int guest = host >= g_arena && host - g_arena < g_guest_size && g_memory.backing;
    uint64_t missing = 0;
    g_memory.alias_unmap(g_memory.context, to, size);
    for (uint64_t o = 0, n; o < size; o += n) {
        const uint8_t* const backing = guest ? g_memory.backing(g_memory.context, host - g_arena + o) : NULL;
        n = 0x1000;
        while (backing && o + n < size && g_memory.backing(g_memory.context, host - g_arena + o + n) == backing + n) {
            n += 0x1000;
        }
        if (!backing || !g_memory.alias_map(g_memory.context, (uint8_t*)to + o, (uint64_t)(backing - g_memory.backing_base), n)) {
            missing += n;
        }
    }
    return missing;
}
#else
uint64_t nvn_direct_alias_pages(void* to, uint64_t host, uint64_t size) {
    const int guest = host >= g_arena && host - g_arena < g_guest_size;
    uint64_t missing = 0;
    for (uint64_t o = 0, n; o < size; o += n) {
        uint64_t offset = 0, next = 0;
        const int fd = guest ? nvn_direct_backing_file(host - g_arena + o, &offset) : -1;
        n = 0x1000;
        while (fd >= 0 && o + n < size && nvn_direct_backing_file(host - g_arena + o + n, &next) == fd &&
               next == offset + n) {
            n += 0x1000;
        }
        uint8_t* const at = (uint8_t*)to + o;
        if (fd < 0 || mmap(at, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, (off_t)offset) ==
                          MAP_FAILED) {
            mmap(at, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            missing += n;
        }
    }
    return missing;
}
#endif
void* nvn_direct_guest_backing(uint64_t host) {
    if (host < g_arena || host - g_arena >= g_guest_size || !g_memory.backing) return NULL;
    return g_memory.backing(g_memory.context, host - g_arena);
}

void* nvn_direct_writable(uint64_t host) {
    const uint64_t guest = guest_addr(host);
    if (guest >= g_guest_size || !g_memory.backing) return (void*)(uintptr_t)host_addr(host);
    void* const backing = g_memory.backing(g_memory.context, guest);
    return backing ? backing : (void*)(uintptr_t)host_addr(host);
}
int nvn_vpool_page_unmapped(uint64_t a); // hle_nvn.c
const void* nvn_direct_readable(uint64_t host) {
    if (host < g_arena || host - g_arena >= g_guest_size) {
        return nvn_vpool_page_unmapped(host) ? NULL : (const void*)(uintptr_t)host;
    }
    if (!g_memory.backing) return (const void*)(uintptr_t)host;
    return g_memory.backing(g_memory.context, host - g_arena);
}
#endif
static inline float F_float(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }
static inline double F_double(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline uint64_t R_float(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline uint64_t R_double(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

// Host copies of the arrays a call passes, per thread: at most two arrays per call (gen_dispatch.py
// checks), each copy valid until the thread's next call.
static _Thread_local struct { void* p; size_t cap; } g_scratch[2];
static void* scratch(int k, size_t bytes) {
    if (g_scratch[k].cap < bytes) {
        free(g_scratch[k].p);
        g_scratch[k].cap = bytes < 256 ? 256 : bytes;
        g_scratch[k].p = malloc(g_scratch[k].cap);
    }
    return g_scratch[k].p;
}
// size: the bytes of each object the pointers name (0: unknown)
static void* xl_pointers_n(uint64_t a, int n, int k, uint64_t size) {
    if (!a || n <= 0) return host_ptr(a);
    const uint64_t* in = host_ptr_n(a, sizeof *in * (uint64_t)n);
    if (XL_FAILED()) return NULL;
    uint64_t* out = scratch(k, sizeof *out * (size_t)n);
    for (int i = 0; i < n; i++) out[i] = host_addr_n(in[i], size);
    return out;
}
static void* xl_pointers(uint64_t a, int n, int k) { return xl_pointers_n(a, n, k, 0); }
static void* xl_handles(uint64_t a, int n, int k) { return xl_pointers(a, n, k); }
static void* xl_buffer_ranges(uint64_t a, int n, int k) {
    if (!a || n <= 0) return host_ptr(a);
    const NVNbufferRange* in = host_ptr_n(a, sizeof *in * (uint64_t)n);
    if (XL_FAILED()) return NULL;
    NVNbufferRange* out = scratch(k, sizeof *out * (size_t)n);
    for (int i = 0; i < n; i++) out[i] = (NVNbufferRange){host_addr(in[i].address), in[i].size};
    return out;
}
static void* xl_shader_data(uint64_t a, int n, int k) {
    if (!a || n <= 0) return host_ptr(a);
    const NVNshaderData* in = host_ptr_n(a, sizeof *in * (uint64_t)n);
    if (XL_FAILED()) return NULL;
    NVNshaderData* out = scratch(k, sizeof *out * (size_t)n);
    for (int i = 0; i < n; i++) out[i] = (NVNshaderData){host_addr(in[i].data), host_ptr((uint64_t)(uintptr_t)in[i].control)};
    return out;
}
static void* xl_shader_data_ext(uint64_t a, int n, int k) {
    if (!a || n <= 0) return host_ptr(a);
    const shader_data_ext* in = host_ptr_n(a, sizeof *in * (uint64_t)n);
    if (XL_FAILED()) return NULL;
    shader_data_ext* out = scratch(k, sizeof *out * (size_t)n);
    for (int i = 0; i < n; i++) out[i] = (shader_data_ext){host_addr(in[i].data), host_ptr((uint64_t)(uintptr_t)in[i].control), in[i].ext};
    return out;
}
static void* xl_mapping_requests(uint64_t a, int n, int k) {
    if (!a || n <= 0) return host_ptr(a);
    const mapping_request* in = host_ptr_n(a, sizeof *in * (uint64_t)n);
    if (XL_FAILED()) return NULL;
    mapping_request* out = scratch(k, sizeof *out * (size_t)n);
    for (int i = 0; i < n; i++) {
        out[i] = in[i];
        out[i].phys = host_ptr((uint64_t)(uintptr_t)in[i].phys);
    }
    return out;
}

// s: the guest's stack at the call (its arguments past x7 / v7), as a host pointer.
typedef uint64_t (*dispatch_fn)(void* fn, const uint64_t* x, const uint64_t* v, const uint64_t* s, uint64_t* v0);
#define D(name, flags, body)                                                                       \
    static uint64_t d_##name(void* fn, const uint64_t* x, const uint64_t* v, const uint64_t* s,   \
                             uint64_t* v0) {                                                       \
        (void)x; (void)v; (void)s; (void)v0;                                                       \
        body                                                                                       \
    }
#ifdef LICHII_NVN_METAL_BRIDGE
#define NVN_DISPATCH_INC "nvn_metal_dispatch.inc"
#else
#define NVN_DISPATCH_INC "nvn_dispatch.inc"
#endif
#include NVN_DISPATCH_INC
#undef D
typedef struct { const char* name; dispatch_fn d; int flags; } dispatch_entry;
#define D(name, flags, body) {#name, d_##name, flags},
static const dispatch_entry g_dispatch[] = {
#include NVN_DISPATCH_INC
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
#ifndef LICHII_NVN_METAL_BRIDGE
void nvn_nops_register(void);
void* nvn_nop_fn(void);
#endif

_Thread_local uint64_t nvn_direct_guest_lr;
static struct { void* fn; const dispatch_entry* d; } g_slot[NVN_NFUNC];
// The empty implementations of names without a prototype (hle_nvn.c g_nops: the shading-rate calls):
// they take no arguments and return nothing.
#ifndef LICHII_NVN_METAL_BRIDGE
static uint64_t d_empty(void* fn, const uint64_t* x, const uint64_t* v, const uint64_t* s, uint64_t* v0) {
    (void)x; (void)v; (void)s; (void)v0;
    ((void (*)(void))fn)();
    return 0;
}
static const dispatch_entry g_empty = {"(empty)", d_empty, 0};
#endif

// The game's NVN (nvn_direct_init): the front end and the replay choose NVN2's behaviour by
// dm_sdk_ounce and NVN1's API version by dm_sdk_major (upstream's loader.c reads both from the sdk).
static int g_switch2, g_sdk_major;
int dm_sdk_ounce(void) { return g_switch2; }
int dm_sdk_major(void) { return g_sdk_major; }

void nvn_direct_init(uint8_t* arena, uint64_t guest_size, const nvn_direct_memory* memory,
                     int switch2, int sdk_major) {
    g_arena = (uintptr_t)arena;
    g_guest_size = guest_size;
    g_memory = *memory;
    g_switch2 = switch2 != 0;
    g_sdk_major = sdk_major;
#ifdef LICHII_NVN_METAL_BRIDGE
    if (g_memory.arena_pages) {
        g_guest_page_of = !g_memory.backing || !g_memory.backing_size || (guest_size >> GUEST_PAGE_SHIFT) >= UINT32_MAX
                              ? NULL
                              : calloc(g_memory.backing_size >> GUEST_PAGE_SHIFT, sizeof *g_guest_page_of);
        if (!g_guest_page_of) {
            g_memory.arena_pages = NULL;
            xl_fail("no backing memory map for guest pages outside the fastmem arena (16 KiB host pages)");
        }
    }
#else
    nvn_nops_register();
#endif
    for (int i = 0; i < NVN_NFUNC; i++) {
        g_slot[i].d = NULL;
        void* fn = nvn_slot[i].fn;
        if (fn == (void*)nvn_prof_entry) fn = nvn_real[i];
        if (fn == (void*)nvn_big_entry) fn = nvn_big_real[i];
        if (fn == (void*)nvn_unimpl_entry) fn = NULL;
        for (size_t k = 0; fn && k < sizeof g_dispatch / sizeof *g_dispatch; k++)
            if (!strcmp(g_dispatch[k].name, nvn_names[i])) g_slot[i].d = &g_dispatch[k];
        // Without a dispatch entry: an empty implementation, or a name the hook answers itself
        // (nvnBootstrapLoader, nvnDeviceGetProcAddress: gen_dispatch.py HOOK_OWN).
#ifndef LICHII_NVN_METAL_BRIDGE
        if (fn && !g_slot[i].d && fn == nvn_nop_fn()) g_slot[i].d = &g_empty;
#endif
        g_slot[i].fn = g_slot[i].d ? fn : NULL;
    }
}

int nvn_direct_name_count(void) { return NVN_NFUNC; }
const char* nvn_direct_name(int slot) { return nvn_names[slot]; }
int nvn_direct_implemented(int slot) { return g_slot[slot].fn != NULL; }
int nvn_direct_float_args(int slot) {
    return g_slot[slot].d && (g_slot[slot].d->flags & (NVN_DISPATCH_FLOAT_ARGS | NVN_DISPATCH_FLOAT_RETURN));
}

#ifndef LICHII_NVN_METAL_BRIDGE
int nvn_direct_call_blocks(int slot, const uint64_t x[8]) {
    const char* const name = nvn_direct_name(slot);
    if (!strcmp(name, "nvnSyncWait")) {
        const sync_t* const s = host_ptr(x[0]);
        return s->fenced && !(s->fenced == 2 ? mtl_wait_acq : mtl_wait)(s->fence, 0);   // (one not fenced yet answers at once: hle_nvn.c)
    }
    if (!strcmp(name, "nvnQueueFinish")) return !mtl_wait(((const queue_t*)host_ptr(x[0]))->last_fence, 0);
    // A texture the GPU wrote is read back once everything submitted is done (the game waited for its own work; the replay may still be behind it)
    if (!strcmp(name, "nvnTextureReadTexels") || !strcmp(name, "nvnTextureReadTexelsStrided"))
        return mtl_tex_gpu_owned(((const tex_t*)host_ptr(x[0]))->serial) && !mtl_wait_acq(mtl_last_fence(), 0);
    return 0;
}
#endif
uint64_t nvn_direct_call(int slot, const uint64_t x[8], const uint64_t v[8], uint64_t sp, uint64_t lr, uint64_t* v0) {
    nvn_direct_guest_lr = lr;
#ifdef LICHII_NVN_METAL_BRIDGE
    if (XL_FAILED()) return 0;
    const uint64_t stack = (uint64_t)(g_slot[slot].d->flags >> NVN_DISPATCH_STACK_SHIFT) * 8;
    const uint64_t* const s = stack ? (const uint64_t*)host_ptr_n(sp, stack) : NULL;
    if (XL_FAILED()) return 0;
    return g_slot[slot].d->d(g_slot[slot].fn, x, v, s, v0);
#else
    return g_slot[slot].d->d(g_slot[slot].fn, x, v, (const uint64_t*)host_ptr(sp), v0);
#endif
}

// nvnDeviceGetInteger: the game's own NVN2 driver's answers, as Lichii captured them or read the
// constants among them from its code (hle_nvn.c asks dm_sdk_nvn2_item before its table).
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
