# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Git REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${LICHII_NXNATIVE_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE nxnative_head OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
if (NOT nxnative_head STREQUAL "7ebb76eda8ec1c532a4fc52170f2c5e13b18e80f")
    message(FATAL_ERROR "The NVN Metal bridge test needs nxnative at 7ebb76eda8ec1c532a4fc52170f2c5e13b18e80f")
endif()

set(dm "${LICHII_NXNATIVE_SOURCE_DIR}/dm")
set(nvn "${dm}/gfx/nvn")
set(direct "${CMAKE_CURRENT_SOURCE_DIR}/../nvn_direct")
set(generated "${CMAKE_CURRENT_BINARY_DIR}/nvn_metal_bridge")
set(profile "${generated}/synthetic")
file(CONFIGURE OUTPUT "${profile}/game.tsv" CONTENT [=[
title_id	0000000000000000
program_id	0000000000000000
name	Lichii NVN bridge test
modules	main
romfs_marker	unused
mem_reserve	0
mem_alias	0
mem_stack	0
app_pool	0
]=])
add_custom_command(OUTPUT "${generated}/game_profile.h"
    COMMAND "${Python3_EXECUTABLE}" "${dm}/games/game.py" header "${profile}" "${generated}/game_profile.h"
    DEPENDS "${dm}/games/game.py" "${profile}/game.tsv"
    VERBATIM)
file(GLOB host_shaders CONFIGURE_DEPENDS "${dm}/gfx/msl/*.metal")
add_custom_command(OUTPUT "${generated}/host_msl_gen.h"
    COMMAND "${Python3_EXECUTABLE}" "${dm}/gfx/tools/host_msl.py"
        --sdk macosx --min 26.0 "${generated}/host_msl_gen.h" ${host_shaders}
    DEPENDS "${dm}/gfx/tools/host_msl.py" ${host_shaders}
    VERBATIM)
add_custom_command(OUTPUT "${generated}/nvn_metal_dispatch.inc"
    COMMAND "${Python3_EXECUTABLE}" "${direct}/gen_dispatch.py"
        --nvn-dir "${nvn}" --output "${generated}/nvn_metal_dispatch.inc" --only
        nvnDeviceBuilderSetDefaults nvnDeviceInitialize nvnDeviceFinalize
        nvnMemoryPoolBuilderSetDefaults nvnMemoryPoolBuilderSetFlags nvnMemoryPoolBuilderSetStorage
        nvnMemoryPoolInitialize nvnMemoryPoolGetBufferAddress nvnMemoryPoolFinalize
        nvnCommandBufferInitialize nvnCommandBufferAddCommandMemory nvnCommandBufferAddControlMemory
        nvnCommandBufferBeginRecording nvnCommandBufferCopyBufferToBuffer nvnCommandBufferEndRecording
        nvnCommandBufferFinalize nvnQueueBuilderSetDefaults nvnQueueBuilderSetDevice
        nvnQueueBuilderGetQueueMemorySize nvnQueueBuilderSetQueueMemory nvnQueueInitialize
        nvnQueueSubmitCommands nvnQueueFenceSync nvnQueueFinalize
        nvnSyncInitialize nvnSyncWait nvnSyncFinalize
        nvnTextureBuilderSetDefaults nvnTextureBuilderSetDevice nvnTextureBuilderSetFlags
        nvnTextureBuilderSetSize2D nvnTextureBuilderSetFormat nvnTextureBuilderGetStorageSize
        nvnTextureBuilderSetStorage nvnTextureInitialize nvnTextureReadTexels nvnTextureFinalize
        nvnCommandBufferSetRenderTargets nvnCommandBufferClearColor
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
    "${dm}/gfx/tests/t_host_stubs.c"
    "${dm}/host/hle_bigstack.c")
set_source_files_properties(${nxnative_sources} PROPERTIES COMPILE_OPTIONS -w)
add_executable(nvn_metal_bridge_test
    ../../tests/video_core/nvn_metal_bridge.mm
    ../../tests/video_core/nvn_metal_bridge_host.c
    "${generated}/game_profile.h"
    "${generated}/host_msl_gen.h"
    "${generated}/nvn_metal_dispatch.inc"
    "${direct}/nvn_dispatch.c"
    ${nxnative_sources})
set_source_files_properties("${direct}/nvn_dispatch.c" PROPERTIES COMPILE_OPTIONS -w)
target_include_directories(nvn_metal_bridge_test PRIVATE
    "${generated}" "${direct}" "${nvn}/include" "${nvn}" "${dm}/host")
target_compile_definitions(nvn_metal_bridge_test PRIVATE LICHII_NVN_METAL_BRIDGE)
target_compile_options(nvn_metal_bridge_test PRIVATE
    $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>)
set_target_properties(nvn_metal_bridge_test PROPERTIES
    C_STANDARD 17 C_EXTENSIONS ON OBJC_STANDARD 17 OBJC_EXTENSIONS ON
    OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_link_libraries(nvn_metal_bridge_test PRIVATE common dynarmic::dynarmic
    "-framework Metal" "-framework MetalFX" "-framework Foundation" "-framework IOKit"
    "-framework QuartzCore" "-framework ImageIO" "-framework CoreGraphics"
    "-framework UniformTypeIdentifiers")
add_test(NAME nvn_metal_bridge_test COMMAND nvn_metal_bridge_test)
set_tests_properties(nvn_metal_bridge_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20
    WORKING_DIRECTORY "${generated}" ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_SHADER_VALIDATION=1")
