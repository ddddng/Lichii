# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(metal_replay STATIC guest_buffer.h guest_buffer.mm)
target_include_directories(metal_replay PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_compile_options(metal_replay PRIVATE -fobjc-arc)
set_target_properties(metal_replay PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
target_link_libraries(metal_replay PUBLIC "-framework Metal" "-framework Foundation")

if (BUILD_TESTING AND LICHII_TESTS)
    add_executable(metal_replay_test ../../tests/video_core/metal_replay.mm)
    set_target_properties(metal_replay_test PROPERTIES
        OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
    target_compile_options(metal_replay_test PRIVATE -fobjc-arc)
    target_link_libraries(metal_replay_test PRIVATE metal_replay common)
    add_test(NAME metal_replay_test COMMAND metal_replay_test)
    set_tests_properties(metal_replay_test PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 20)
endif()
