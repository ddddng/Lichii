// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <time.h>

#include "dm.h"

void* front_metal_layer(void);
void crash_set_wait_dump(void (*callback)(void));

module_t* g_mod;
int g_nmod;
static void (*write_prepare)(void*, size_t);

module_t* module_of(const void* address) {
    (void)address;
    return NULL;
}

void* front_metal_layer(void) {
    return NULL;
}

void svc_thread_dump(void) {}

void crash_set_wait_dump(void (*callback)(void)) {
    (void)callback;
}

void dm_backtrace(uint64_t fp) {
    (void)fp;
}

void dm_res_report_register(void (*callback)(uint32_t, const char*)) {
    (void)callback;
}

uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

void dm_fault_hook_register(int (*callback)(void*, int, void*)) {
    (void)callback;
}

void dm_guest_write_prep_register(void (*callback)(void*, size_t)) {
    write_prepare = callback;
}

void dm_guest_write_prepare(void* address, size_t size) {
    if (write_prepare) {
        write_prepare(address, size);
    }
}
