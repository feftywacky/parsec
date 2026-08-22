#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <limits>

#include "core/units.hpp"

// number_fmt.cpp lives under src/ui/widgets/ (parsec_ui, per the Phase 2 brief's required
// file layout) and has no ImGui/ImPlot/GLFW dependency of its own -- only <core/units.hpp> --
// but parsec_tests links only parsec_core (CMakeLists.txt, which this phase does not own).
// Rather than editing that shared, multi-agent-owned CMakeLists.txt to add a parsec_ui link
// (pulling OpenGL/GLFW into the test binary for a formatter that needs neither), this test
// compiles the implementation directly into its own translation unit.
#include "ui/widgets/number_fmt.cpp"

using namespace pc;
using namespace pc::ui;

namespace {
// Small helper so assertions read as string comparisons rather than char* pointer noise.
bool eq(const char* a, const char* b) {
    return std::strcmp(a, b) == 0;
}
}  // namespace

TEST_CASE("format_px renders at 6 - szDecimals decimal places") {
    char buf[64];
    // szDecimals 0 -> 6 price decimals.
    CHECK(eq(format_px(11834250000000LL, 0, buf, sizeof(buf)), "118342.500000"));
    // szDecimals 5 -> 1 price decimal (BTC-style perp with 5 sig fig sizing).
    CHECK(eq(format_px(11834250000000LL, 5, buf, sizeof(buf)), "118342.5"));
    // szDecimals 6 -> 0 price decimals (integer price only).
    CHECK(eq(format_px(11834250000000LL, 6, buf, sizeof(buf)), "118343"));  // rounds .5 up
    // szDecimals clamped above 6 behaves the same as 6.
    CHECK(eq(format_px(11834250000000LL, 8, buf, sizeof(buf)), "118343"));
}

TEST_CASE("format_qty renders at exactly szDecimals decimal places") {
    char buf[64];
    for (uint8_t sz = 0; sz <= 6; ++sz) {
        // 1.23456789 scaled by 1e8.
        format_qty(123456789LL, sz, buf, sizeof(buf));
        // Decimal point present iff sz > 0, and the string length matches sz decimals exactly.
        const char* dot = std::strchr(buf, '.');
        if (sz == 0) {
            CHECK(dot == nullptr);
        } else {
            REQUIRE(dot != nullptr);
            CHECK(std::strlen(dot + 1) == sz);
        }
    }
    CHECK(eq(format_qty(123000000LL, 0, buf, sizeof(buf)), "1"));
    CHECK(eq(format_qty(123000000LL, 2, buf, sizeof(buf)), "1.23"));
    CHECK(eq(format_qty(123456789LL, 6, buf, sizeof(buf)), "1.234568"));  // rounds last digit
}

TEST_CASE("format_qty handles szDecimals 0 through 6 boundary values exactly") {
    char buf[64];
    CHECK(eq(format_qty(kScale, 0, buf, sizeof(buf)), "1"));
    CHECK(eq(format_qty(kScale, 1, buf, sizeof(buf)), "1.0"));
    CHECK(eq(format_qty(kScale, 6, buf, sizeof(buf)), "1.000000"));
    // Smallest representable unit at szDecimals 6 is 1e-6, i.e. raw value 100 (100 * 1e-8).
    CHECK(eq(format_qty(100LL, 6, buf, sizeof(buf)), "0.000001"));
    // A raw value below that unit (here 1, i.e. 1e-8) rounds down to zero at 6 decimals.
    CHECK(eq(format_qty(1LL, 6, buf, sizeof(buf)), "0.000000"));
}

TEST_CASE("negatives") {
    char buf[64];
    CHECK(eq(format_px(-11834250000000LL, 5, buf, sizeof(buf)), "-118342.5"));
    CHECK(eq(format_qty(-123000000LL, 2, buf, sizeof(buf)), "-1.23"));
    CHECK(eq(format_usd(-123456LL, buf, sizeof(buf)), "-$0.00"));  // sub-cent negative rounds to 0
    CHECK(eq(format_usd(-100000000LL, buf, sizeof(buf)), "-$1.00"));
}

TEST_CASE("zero") {
    char buf[64];
    CHECK(eq(format_px(0, 0, buf, sizeof(buf)), "0.000000"));
    CHECK(eq(format_qty(0, 4, buf, sizeof(buf)), "0.0000"));
    CHECK(eq(format_usd(0, buf, sizeof(buf)), "$0.00"));
    CHECK(eq(format_pct(0, 2, buf, sizeof(buf)), "0.00%"));
}

TEST_CASE("sub-cent values round rather than truncate to nothing") {
    char buf[64];
    // $0.004 rounds to $0.00; $0.005 rounds to $0.01 (round-half-up on the magnitude).
    CHECK(eq(format_usd(400000LL, buf, sizeof(buf)), "$0.00"));
    CHECK(eq(format_usd(500000LL, buf, sizeof(buf)), "$0.01"));
    CHECK(eq(format_usd(1LL, buf, sizeof(buf)), "$0.00"));  // 1e-8 dollars, far below display
}

TEST_CASE("format_usd inserts thousands separators") {
    char buf[64];
    CHECK(eq(format_usd(kScale, buf, sizeof(buf)), "$1.00"));
    CHECK(eq(format_usd(1000LL * kScale, buf, sizeof(buf)), "$1,000.00"));
    CHECK(eq(format_usd(1234567LL * kScale, buf, sizeof(buf)), "$1,234,567.00"));
    CHECK(eq(format_usd(999LL * kScale, buf, sizeof(buf)), "$999.00"));  // no spurious comma
    CHECK(eq(format_usd(-1234567LL * kScale, buf, sizeof(buf)), "-$1,234,567.00"));
}

TEST_CASE("format_pct scales a 1e8 fraction to a percentage") {
    char buf[64];
    // 0.000125 (1e8-scaled: 12500) is 0.0125%.
    CHECK(eq(format_pct(12500LL, 4, buf, sizeof(buf)), "0.0125%"));
    // 1.0 (fully scaled, i.e. 100%) -> 100.00%.
    CHECK(eq(format_pct(kScale, 2, buf, sizeof(buf)), "100.00%"));
    CHECK(eq(format_pct(-kScale / 20, 2, buf, sizeof(buf)), "-5.00%"));  // -0.05 -> -5%
}

TEST_CASE("the largest and smallest int64 values do not overflow or corrupt the buffer") {
    char buf[64];
    const char* max_str = format_px(std::numeric_limits<int64_t>::max(), 0, buf, sizeof(buf));
    CHECK(max_str == buf);
    CHECK(buf[0] != '\0');
    CHECK(std::strlen(buf) < sizeof(buf));

    const char* min_str = format_px(std::numeric_limits<int64_t>::min(), 0, buf, sizeof(buf));
    CHECK(min_str[0] == '-');
    CHECK(std::strlen(buf) < sizeof(buf));

    // INT64_MIN is the one value whose negation overflows int64 -- exercise it at every
    // formatter to make sure none of them hit the UB `-value` shortcut.
    format_qty(std::numeric_limits<int64_t>::min(), 6, buf, sizeof(buf));
    CHECK(buf[0] == '-');
    format_usd(std::numeric_limits<int64_t>::min(), buf, sizeof(buf));
    CHECK(buf[0] == '-');
}

TEST_CASE("a too-small buffer truncates safely instead of overflowing") {
    char buf[4];
    format_usd(1234567LL * kScale, buf, sizeof(buf));
    CHECK(std::strlen(buf) == 3);  // cap - 1
    // No crash / ASan violation is the actual assertion here; the content is secondary.
}

TEST_CASE("format_px never shows more decimals than the venue's price precision allows") {
    char buf[64];
    for (uint8_t sz = 0; sz <= 6; ++sz) {
        format_px(123456789LL, sz, buf, sizeof(buf));
        const char* dot = std::strchr(buf, '.');
        const int expected_decimals = 6 - sz;
        if (expected_decimals == 0) {
            CHECK(dot == nullptr);
        } else {
            REQUIRE(dot != nullptr);
            CHECK(static_cast<int>(std::strlen(dot + 1)) == expected_decimals);
        }
    }
}
