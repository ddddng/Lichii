# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# Native Metal NVN (ENABLE_NVN_METAL): nxnative's NVN front end and Metal backend (MIT, seedds) at
# its pinned revision, copied into the build directory with Lichii's patches and shader overlays,
# and driven by the full native dispatcher. The checkout itself is never modified.

set(nxnative_pin 7ebb76eda8ec1c532a4fc52170f2c5e13b18e80f)
set(LICHII_NVN_METAL_GAME_PROFILE "" CACHE PATH "The game's local nxnative profile directory (game.tsv)")
if (NOT LICHII_NXNATIVE_SOURCE_DIR OR NOT EXISTS "${LICHII_NVN_METAL_GAME_PROFILE}/game.tsv")
    message(FATAL_ERROR "ENABLE_NVN_METAL needs LICHII_NXNATIVE_SOURCE_DIR (nxnative at ${nxnative_pin}) "
                        "and LICHII_NVN_METAL_GAME_PROFILE (a local profile directory with game.tsv)")
endif()
find_package(Git REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${LICHII_NXNATIVE_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE nxnative_head OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if (NOT nxnative_head STREQUAL nxnative_pin)
    message(FATAL_ERROR "Native Metal NVN needs nxnative at ${nxnative_pin}, not ${nxnative_head}")
endif()

set(overlay "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/nx2/metal_shader_overlay.py")
set(patches
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/nxnative-queue-stop.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/nxnative-pool-lease.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/nxnative-presented.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/nxnative-write-tracking.patch"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/nxnative-memory-callback.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${patches} "${overlay}")
set(stamp "${nxnative_pin}")
foreach (input IN LISTS patches overlay)
    file(SHA256 "${input}" hash)
    string(APPEND stamp " ${hash}")
endforeach()

set(root "${CMAKE_CURRENT_BINARY_DIR}/nxnative")
set(dm "${root}/dm")
set(nvn "${dm}/gfx/nvn")
set(previous "")
if (EXISTS "${root}/lichii-stamp.txt")
    file(READ "${root}/lichii-stamp.txt" previous)
endif()
if (NOT previous STREQUAL stamp)
    message(STATUS "Native Metal NVN: patching nxnative ${nxnative_pin} into ${root}")
    file(REMOVE_RECURSE "${root}")
    file(MAKE_DIRECTORY "${root}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${LICHII_NXNATIVE_SOURCE_DIR}" archive --format=tar
            "${nxnative_pin}" dm/gfx dm/host dm/games/game.py
        COMMAND tar -x -C "${root}"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" init -q WORKING_DIRECTORY "${root}" COMMAND_ERROR_IS_FATAL ANY)
    foreach (patch IN LISTS patches)
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${patch}"
            WORKING_DIRECTORY "${root}" COMMAND_ERROR_IS_FATAL ANY)
    endforeach()
    execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${overlay}" "${nvn}/nvn_mtl.m" "${nvn}/nvn_mtl.m"
        COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -I "${overlay}" --hle "${nvn}/hle_nvn.c" "${nvn}/hle_nvn.c"
        COMMAND_ERROR_IS_FATAL ANY)
    file(WRITE "${root}/lichii-stamp.txt" "${stamp}")
endif()

set(generated "${CMAKE_CURRENT_BINARY_DIR}/nvn_metal")
set(direct "${CMAKE_CURRENT_SOURCE_DIR}/../nvn_direct")
file(GLOB profile_inputs CONFIGURE_DEPENDS "${LICHII_NVN_METAL_GAME_PROFILE}/*.tsv")
add_custom_command(OUTPUT "${generated}/game_profile.h"
    COMMAND "${Python3_EXECUTABLE}" -I "${dm}/games/game.py" header "${LICHII_NVN_METAL_GAME_PROFILE}"
        "${generated}/game_profile.h"
    DEPENDS "${dm}/games/game.py" ${profile_inputs}
    VERBATIM)
file(GLOB host_shaders CONFIGURE_DEPENDS "${dm}/gfx/msl/*.metal")
add_custom_command(OUTPUT "${generated}/host_msl_gen.h"
    COMMAND "${Python3_EXECUTABLE}" -I "${dm}/gfx/tools/host_msl.py"
        --sdk macosx --min 26.0 "${generated}/host_msl_gen.h" ${host_shaders}
    DEPENDS "${dm}/gfx/tools/host_msl.py" ${host_shaders}
    VERBATIM)
add_custom_command(OUTPUT "${generated}/nvn_metal_dispatch.inc"
    COMMAND "${Python3_EXECUTABLE}" -I "${direct}/gen_dispatch.py" --native --nvn-dir "${nvn}"
        --output "${generated}/nvn_metal_dispatch.inc"
    DEPENDS "${direct}/gen_dispatch.py" "${nvn}/nvn_protos.h" "${nvn}/hle_nvn.c" "${nvn}/include/nvn.h"
    VERBATIM)

set(nxnative_sources
    "${nvn}/hle_nvn.c"
    "${nvn}/host_msl.m"
    "${nvn}/nvn_capture.c"
    "${nvn}/nvn_dirty.c"
    "${nvn}/nvn_entry.S"
    "${nvn}/nvn_mtl.m"
    "${nvn}/nvn_owner.m"
    "${nvn}/nvn_refl.m"
    "${nvn}/nvn_tex.c"
    "${nvn}/nvn_thunks.S"
    "${dm}/gfx/tex/nvn_deswizzle.c"
    "${dm}/gfx/tex/nvn_deswizzle_gpu.m"
    "${dm}/gfx/tex/nvn_fmt_mtl.c"
    "${dm}/host/hle_bigstack.c")
# Upstream's code is kept as it is: its warnings are not Lichii's to fix.
set_source_files_properties(${nxnative_sources} "${direct}/nvn_dispatch.c" PROPERTIES COMPILE_OPTIONS -w)
add_library(nvn_metal STATIC
    "${generated}/game_profile.h"
    "${generated}/host_msl_gen.h"
    "${generated}/nvn_metal_dispatch.inc"
    "${direct}/nvn_dispatch.c"
    "${direct}/nvn_direct_dispatch.h"
    nvn_metal.h
    nvn_metal_calls.c
    nvn_metal_host.c
    nvn_metal_shaders.h
    nvn_metal_shaders.inc
    ${nxnative_sources})
target_include_directories(nvn_metal PRIVATE
    "${generated}" "${direct}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    "${nvn}/include" "${nvn}" "${dm}/host")
target_compile_definitions(nvn_metal PRIVATE LICHII_NVN_METAL_BRIDGE LICHII_NVN_METAL_LIVE
    INTERFACE LICHII_NVN_METAL)
target_compile_options(nvn_metal PRIVATE $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>)
set_target_properties(nvn_metal PROPERTIES C_STANDARD 17 C_EXTENSIONS ON OBJC_STANDARD 17 OBJC_EXTENSIONS ON)
target_link_libraries(nvn_metal PUBLIC metal_replay
    "-framework Metal" "-framework MetalFX" "-framework Foundation" "-framework IOKit"
    "-framework QuartzCore" "-framework ImageIO" "-framework CoreGraphics"
    "-framework UniformTypeIdentifiers")
