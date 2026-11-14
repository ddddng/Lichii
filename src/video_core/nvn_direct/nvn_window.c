// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVN windows in direct mode: what upstream's window block of hle_nvn.c did (left out there),
// without its pacing, frame statistics and present thread. A present only counts here;
// Lichii's hook does the present's bookkeeping and pacing (core/nvn_hook.cpp) and, once the
// replay presents through Lichii (dotfiles-25x.7), hands it the image. The window and sync
// layouts are the front end's (nvn_obj.h).

#include <stdatomic.h>
#include <string.h>

#include "hle.h"
#include "nvn_protos.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"

typedef struct { const char* name; void* fn; } nvn_impl;
// As hle_nvn.c's NVN(): an implementation with its signature checked against the prototype,
// listed in the front end's table (section swnvn).
#define NVN(name, ret, ...)                                                                        \
    static ret h_##name(__VA_ARGS__);                                                              \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                        \
    PLAT_TABLE(swnvn) static const nvn_impl e_##name = {#name, (void*)h_##name};                   \
    static ret h_##name(__VA_ARGS__)

#define WB(b) ((win_builder_t*)(b))
#define CTX(t) ((const tex_t*)(t))

static _Atomic uint64_t g_frame;
uint64_t dm_nvn_frame_count(void) { return atomic_load(&g_frame); }

NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder* b) {
    NVNdevice* dev = WB(b)->dev;
    memset(b, 0, sizeof(win_builder_t));
    WB(b)->dev = dev;
    WB(b)->interval = 1;
}
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder* b, NVNdevice* d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder* b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder* b, int n, NVNtexture* const* t) {
    if (n < 0 || n > 4) {
        LOG("[RED] WindowBuilderSetTextures count %d", n);
        n = n < 0 ? 0 : 4;
    }
    WB(b)->ntex = n;
    for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder* b, int v) { WB(b)->interval = v; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow* w, const NVNwindowBuilder* b) {
    const win_builder_t* s = (const win_builder_t*)b;
    win_t* o = (win_t*)w;
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('W', 'I', 'N', 'D');
    o->native = s->native;
    o->ntex = s->ntex;
    o->interval = s->interval;
    memcpy(o->tex, s->tex, sizeof o->tex);
    LOG("[nvn] WindowInitialize swap chain %d textures, interval %d, first %dx%d format %#x", o->ntex, o->interval,
        o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0, o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0,
        o->ntex && o->tex[0] ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowGetPresentInterval, int, const NVNwindow* w) { return ((const win_t*)w)->interval; }
NVN(nvnWindowSetPresentInterval, void, NVNwindow* w, int v) {
    if (((win_t*)w)->interval != v) LOG("[nvn] the game sets present interval %d -> %d", ((win_t*)w)->interval, v);
    ((win_t*)w)->interval = v;
}
NVN(nvnWindowSetCrop, void, NVNwindow* w, int x, int y, int ww, int hh) {
    win_t* o = (win_t*)w;
    const int v[4] = {x, y, ww, hh};
    memcpy(o->crop, v, sizeof v);
}
// Before any SetCrop it is the whole first texture, as NVN starts it.
NVN(nvnWindowGetCrop, void, const NVNwindow* w, NVNrectangle* r) {
    const win_t* o = (const win_t*)w;
    if (o->crop[2] > 0 && o->crop[3] > 0) {
        r->x = o->crop[0]; r->y = o->crop[1]; r->width = o->crop[2]; r->height = o->crop[3];
        return;
    }
    r->x = r->y = 0;
    r->width = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0;
    r->height = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0;
}
// The images go round in order; an acquire's sync is reached when every submission before it is
// (as upstream's default mtl_acquire_fence).
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow* w, NVNsync* s, int* idx) {
    win_t* o = (win_t*)w;
    *idx = o->ntex ? o->next++ % o->ntex : 0;
    if (s) {
        ((sync_t*)s)->fenced = 2;
        ((sync_t*)s)->fence = mtl_last_fence();
    }
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue* q, NVNwindow* w, int* idx) {
    (void)q;
    return h_nvnWindowAcquireTexture(w, NULL, idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
               ? (NVNqueueAcquireTextureResult)0
               : (NVNqueueAcquireTextureResult)1;
}
NVN(nvnQueuePresentTexture, void, NVNqueue* q, NVNwindow* w, int idx) {
    (void)q; (void)idx;
    ((win_t*)w)->presents++;
    atomic_fetch_add(&g_frame, 1);
}
