# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# NVN2 direct renderer (lab, dotfiles-25x): Banana Native's NVN front end and Vulkan replay (MIT,
# bananative/) as a C library, built the way the lab's x86-64 port builds them (__ANDROID__
# selects their Linux code paths; the Android headers they include come from bananative/shim).
# Linux x86-64 only.

set(BN ${CMAKE_CURRENT_SOURCE_DIR}/bananative/dm)

add_library(nvn_direct STATIC
    gen/nvn_dispatch.inc
    gen_dispatch.py
    nvn_direct.h
    nvn_dispatch.c
    nvn_direct_front.h
    nvn_direct_host.c
    nvn_present.c
    nvn_tracking.c
    nvn_window.c
    ${BN}/gfx/nvn/android_stub.c
    ${BN}/gfx/nvn/hle_nvn.c
    ${BN}/gfx/nvn/nvn_capture.c
    ${BN}/gfx/nvn/nvn_tex.c
    ${BN}/gfx/tex/nvn_deswizzle.c
    ${BN}/gfx/tex/nvn_fmt_mtl.c
    ${BN}/gfx/vk_android/vka_bind.c
    ${BN}/gfx/vk_android/vka_capture.c
    ${BN}/gfx/vk_android/vka_ctl.c
    ${BN}/gfx/vk_android/vka_dev.c
    ${BN}/gfx/vk_android/vka_draw.c
    ${BN}/gfx/vk_android/vka_enc.c
    ${BN}/gfx/vk_android/vka_exec.c
    ${BN}/gfx/vk_android/vka_fps.c
    ${BN}/gfx/vk_android/vka_gprof.c
    ${BN}/gfx/vk_android/vka_imgmem.c
    ${BN}/gfx/vk_android/vka_mem.c
    ${BN}/gfx/vk_android/vka_nanscan.c
    ${BN}/gfx/vk_android/vka_perf.c
    ${BN}/gfx/vk_android/vka_pso.c
    ${BN}/gfx/vk_android/vka_readiness.c
    ${BN}/gfx/vk_android/vka_selftest.c
    ${BN}/gfx/vk_android/vka_shader.c
    ${BN}/gfx/vk_android/vka_shlib.c
    ${BN}/gfx/vk_android/vka_sparse.c
    ${BN}/gfx/vk_android/vka_spvpatch.c
    ${BN}/gfx/vk_android/vka_tex.c
    ${BN}/gfx/vk_android/vka_worker.c
    ${BN}/host/plat/plat_android.c
)

target_include_directories(nvn_direct PRIVATE
    bananative/shim
    gen
    .
    bananative/gen
    ${BN}/gfx/nvn/include
    ${BN}/gfx/nvn
    ${BN}/host
)
set_target_properties(nvn_direct PROPERTIES C_STANDARD 17 C_EXTENSIONS ON)
target_compile_definitions(nvn_direct PRIVATE __ANDROID__ _GNU_SOURCE)
# Upstream's code is kept as it is: its warnings are not Lichii's to fix.
target_compile_options(nvn_direct PRIVATE -w)
target_compile_definitions(nvn_direct INTERFACE LICHII_NVN_DIRECT)
target_link_libraries(nvn_direct PUBLIC Vulkan::Headers PRIVATE zstd::zstd ZLIB::ZLIB Threads::Threads ${CMAKE_DL_LIBS} m)
