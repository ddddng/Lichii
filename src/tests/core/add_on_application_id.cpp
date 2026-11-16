// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "core/file_sys/common_funcs.h"

using FileSys::GetAddOnApplicationID;

TEST_CASE("Switch 2 Edition programs find the Switch 1 application's add-ons",
          "[file_sys][aoc]") {
    SECTION("Kirby and the Forgotten Land") {
        const std::array<u64, 1> add_ons{0x01004D300C5AF001};
        CHECK(GetAddOnApplicationID(0x04004D300C5AE001, add_ons) == 0x01004D300C5AE000);
        CHECK(GetAddOnApplicationID(0x01004D300C5AE000, add_ons) == 0x01004D300C5AE000);
    }
    SECTION("Super Mario Bros. Wonder") {
        const std::array<u64, 1> add_ons{0x010015100B515001};
        CHECK(GetAddOnApplicationID(0x040015100B514001, add_ons) == 0x010015100B514000);
        CHECK(GetAddOnApplicationID(0x010015100B514000, add_ons) == 0x010015100B514000);
    }
}

TEST_CASE("Add-ons under a program's own ID take precedence", "[file_sys][aoc]") {
    const std::array<u64, 2> add_ons{0x01004D300C5AF001, 0x04004D300C5AF001};
    CHECK(GetAddOnApplicationID(0x04004D300C5AE001, add_ons) == 0x04004D300C5AE000);
}

TEST_CASE("Unrelated add-ons leave the program's own ID", "[file_sys][aoc]") {
    const std::array<u64, 1> add_ons{0x0100152000023001};
    CHECK(GetAddOnApplicationID(0x04004D300C5AE001, add_ons) == 0x04004D300C5AE000);
    CHECK(GetAddOnApplicationID(0x04004D300C5AE001, {}) == 0x04004D300C5AE000);
}
