// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The host services nxnative's NVN front end and Metal backend call (dm/host in nxnative), as
// Lichii provides them. front_metal_layer is presentation_host.cpp's; the pools' device sharing is
// MetalPoolPins' (core/nvn_metal_pool_pins.h).

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#include "dm.h"

module_t* g_mod;
int g_nmod;
uint64_t g_t0_ns;
_Atomic uint64_t dm_present_ns;

uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

__attribute__((constructor)) static void start_time(void) {
    g_t0_ns = now_ns();
}

module_t* module_of(const void* p) {
    (void)p;
    return NULL;
}

int dm_guest_as_contains(uintptr_t address) {
    (void)address;
    return 0;
}

void dm_vi_layer_crop(int c[4]) {
    c[0] = c[1] = c[2] = c[3] = 0;
}

void dm_device_share(const void* p, uint64_t size, int share) {
    (void)p;
    (void)size;
    (void)share;
}

void dm_vi_frame_queued(uint64_t frame) {
    (void)frame;
}

void crash_set_wait_dump(void (*fn)(void)) {
    (void)fn;
}

void dm_backtrace(uint64_t fp) {
    (void)fp;
}

void dm_res_report_register(void (*fn)(uint32_t tick, const char* why)) {
    (void)fn;
}

void dm_guest_write_prepare(void* addr, size_t len) {
    (void)addr;
    (void)len;
}
