#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "core/units.hpp"

// chart_scale.cpp lives under src/ui/widgets/ but has no ImGui/ImPlot/GLFW dependency -- it is
// the chart's pure axis arithmetic, deliberately split out so it can be tested without a
// window. parsec_tests links only parsec_core, so (following tests/cpp/test_number_fmt.cpp)
// the implementation is compiled straight into this translation unit rather than editing the
// shared CMakeLists.txt to pull OpenGL into the test binary.
#include "ui/widgets/chart_scale.cpp"

using namespace pc;
using namespace pc::ui;

namespace {

constexpr uint64_t kMinute = 60'000;
constexpr uint64_t kHour = 60 * kMinute;
constexpr uint64_t kDay = 24 * kHour;

// 2026-09-07T00:00:00Z, a Monday -- so a series starting here crosses a week boundary on its
// first bar and a month boundary 24 days later.
constexpr uint64_t kSep7 = 1'788'739'200'000ULL;

std::vector<pc_candle> series(uint64_t start_ms, uint64_t step_ms, size_t count) {
    std::vector<pc_candle> out(count);
    for (size_t i = 0; i < count; ++i) {
        out[i].open_ms = start_ms + step_ms * i;
        out[i].close_ms = out[i].open_ms + step_ms;
    }
    return out;
}

bool has_label(const TimeTicks& ticks, const char* text) {
    for (int i = 0; i < ticks.count; ++i)
        if (std::strcmp(ticks.ticks[i].label, text) == 0)
            return true;
    return false;
}

}  // namespace

TEST_CASE("price_frac and frac_price invert each other") {
    SUBCASE("linear") {
        CHECK(price_frac(100.0, 100.0, 200.0, false) == doctest::Approx(0.0));
        CHECK(price_frac(150.0, 100.0, 200.0, false) == doctest::Approx(0.5));
        CHECK(price_frac(200.0, 100.0, 200.0, false) == doctest::Approx(1.0));
        CHECK(frac_price(0.25, 100.0, 200.0, false) == doctest::Approx(125.0));
    }
    SUBCASE("log10 -- the midpoint is the geometric mean, not the arithmetic one") {
        CHECK(price_frac(10.0, 1.0, 100.0, true) == doctest::Approx(0.5));
        CHECK(frac_price(0.5, 1.0, 100.0, true) == doctest::Approx(10.0));
        CHECK(price_frac(frac_price(0.31, 80.0, 120.0, true), 80.0, 120.0, true) ==
              doctest::Approx(0.31));
    }
    SUBCASE("a degenerate range does not divide by zero") {
        CHECK(price_frac(5.0, 5.0, 5.0, false) == doctest::Approx(0.5));
    }
}

TEST_CASE("price_ticks lands on a 1/2/2.5/5 ladder") {
    // A ~$300 range over 600px with 40px between labels asks for at most 15 ticks; the ladder
    // rung that fits is $25.
    const PriceTicks ticks = price_ticks(79000.0, 79300.0, 600.0F, 40.0F, false, 1);
    REQUIRE(ticks.count > 3);
    const Px step = ticks.values[1] - ticks.values[0];
    CHECK(step == 20 * kScale);
    for (int i = 1; i < ticks.count; ++i)
        CHECK(ticks.values[i] - ticks.values[i - 1] == step);
    // Every tick is a multiple of the step and inside the range.
    for (int i = 0; i < ticks.count; ++i) {
        CHECK(ticks.values[i] % step == 0);
        CHECK(ticks.values[i] >= 79000 * kScale);
        CHECK(ticks.values[i] <= 79300 * kScale);
    }
    CHECK(ticks.decimals == 0);
}

TEST_CASE("price_ticks honours the pixel budget") {
    const PriceTicks sparse = price_ticks(100.0, 200.0, 400.0F, 100.0F, false, 4);
    const PriceTicks dense = price_ticks(100.0, 200.0, 400.0F, 20.0F, false, 4);
    CHECK(sparse.count < dense.count);
    // No two labels closer than the requested gap, on either.
    for (int i = 1; i < sparse.count; ++i) {
        const double gap = static_cast<double>(sparse.values[i] - sparse.values[i - 1]) /
                           static_cast<double>(kScale) / 100.0 * 400.0;
        CHECK(gap >= 100.0);
    }
}

TEST_CASE("price_ticks never quotes finer than the venue's precision") {
    // max_decimals = 0 means the asset trades in whole units, so no tick may be fractional --
    // a $0.50 gridline on a whole-dollar instrument is a price the exchange would reject.
    const PriceTicks ticks = price_ticks(100.0, 103.0, 600.0F, 30.0F, false, 0);
    REQUIRE(ticks.count > 0);
    CHECK(ticks.decimals == 0);
    for (int i = 0; i < ticks.count; ++i)
        CHECK(ticks.values[i] % kScale == 0);
}

TEST_CASE("price_ticks reports the decimals its own values need") {
    const PriceTicks ticks = price_ticks(1.2000, 1.2100, 400.0F, 40.0F, false, 4);
    REQUIRE(ticks.count > 2);
    CHECK(ticks.decimals > 0);
    CHECK(ticks.decimals <= 4);
    // The whole point of keeping ticks on the fixed-point grid: every value must be exactly
    // representable at the precision the labels are printed with, with nothing rounded away.
    Px unit = kScale;
    for (int d = 0; d < ticks.decimals; ++d)
        unit /= 10;
    for (int i = 0; i < ticks.count; ++i)
        CHECK(ticks.values[i] % unit == 0);
}

TEST_CASE("price_ticks over a decade or more steps by decade") {
    const PriceTicks ticks = price_ticks(1.0, 1000.0, 600.0F, 40.0F, true, 4);
    REQUIRE(ticks.count > 3);
    // Every value is m x 10^k for m in 1..9.
    for (int i = 0; i < ticks.count; ++i) {
        const double v = static_cast<double>(ticks.values[i]) / static_cast<double>(kScale);
        const double decade = std::pow(10.0, std::floor(std::log10(v)));
        const double mantissa = v / decade;
        CHECK(std::abs(mantissa - std::round(mantissa)) < 1e-6);
    }
}

TEST_CASE("price_ticks tolerates junk input") {
    CHECK(price_ticks(100.0, 100.0, 600.0F, 30.0F, false, 2).count == 0);
    CHECK(price_ticks(200.0, 100.0, 600.0F, 30.0F, false, 2).count == 0);
    CHECK(price_ticks(100.0, 200.0, 0.0F, 30.0F, false, 2).count == 0);
    CHECK(price_ticks(100.0, 200.0, 600.0F, 0.0F, false, 2).count == 0);
    // A range that cannot be put on the 1e8 int64 grid yields no gridlines rather than a
    // wrapped cast. Nothing the venue lists comes near this; a runaway manual axis could.
    CHECK(price_ticks(1.0, 1.0e30, 600.0F, 30.0F, false, 2).count == 0);
    CHECK(
        price_ticks(0.0, std::numeric_limits<double>::infinity(), 600.0F, 30.0F, false, 2).count ==
        0);
}

TEST_CASE("time_ticks puts the day boundary above the hours around it") {
    // 48 hourly bars from Sep 7 00:00 -- one day boundary, at index 24.
    const auto bars = series(kSep7, kHour, 48);
    const TimeTicks ticks = time_ticks(bars.data(), 0, 47, kHour, 30.0F, 80.0F);
    REQUIRE(ticks.count > 1);
    bool day_placed = false;
    for (int i = 0; i < ticks.count; ++i) {
        if (ticks.ticks[i].index == 24) {
            day_placed = true;
            CHECK(ticks.ticks[i].weight == TimeTickWeight::Day);
            CHECK(std::strcmp(ticks.ticks[i].label, "8") == 0);
        }
    }
    CHECK(day_placed);
    // Everything else on an hourly chart reads as a clock time, off a ladder step that fits
    // the spacing -- here 3h, the first rung at which 80px of label has room.
    CHECK(has_label(ticks, "03:00"));
}

TEST_CASE("time_ticks marks month and year rollovers") {
    SUBCASE("month") {
        // 40 daily bars from Sep 7 crosses into October at index 24.
        const auto bars = series(kSep7, kDay, 40);
        const TimeTicks ticks = time_ticks(bars.data(), 0, 39, kDay, 24.0F, 80.0F);
        CHECK(has_label(ticks, "Oct"));
    }
    SUBCASE("year") {
        // Start Dec 20 2026 so 30 daily bars cross into 2027.
        const auto bars = series(kSep7 + 104 * kDay, kDay, 30);
        const TimeTicks ticks = time_ticks(bars.data(), 0, 29, kDay, 24.0F, 80.0F);
        CHECK(has_label(ticks, "2027"));
    }
}

TEST_CASE("time_ticks respects the label spacing budget") {
    const auto bars = series(kSep7, kMinute, 600);
    // 4px per bar and an 80px budget means no two labels within 20 bars of each other.
    const TimeTicks ticks = time_ticks(bars.data(), 0, 599, kMinute, 4.0F, 80.0F);
    REQUIRE(ticks.count > 1);
    for (int i = 0; i < ticks.count; ++i) {
        for (int j = i + 1; j < ticks.count; ++j) {
            const size_t a = ticks.ticks[i].index;
            const size_t b = ticks.ticks[j].index;
            CHECK((a > b ? a - b : b - a) >= 20);
        }
    }
    // ...and they come back left to right, which is the order the renderer walks them in.
    for (int i = 1; i < ticks.count; ++i)
        CHECK(ticks.ticks[i - 1].index < ticks.ticks[i].index);
}

TEST_CASE("time_ticks stays inside its budget on a long series") {
    const auto bars = series(kSep7, kMinute, 5000);
    const TimeTicks ticks = time_ticks(bars.data(), 0, 4999, kMinute, 1.0F, 80.0F);
    CHECK(ticks.count <= TimeTicks::kMax);
    // The whole visible span gets labelled, not just its left-hand end: with the budget spent
    // on the first screenful the axis would simply stop part-way across.
    REQUIRE(ticks.count > 0);
    CHECK(ticks.ticks[ticks.count - 1].index > 2500);
}

TEST_CASE("time_ticks steps by bar, so a gap in the series collapses") {
    // Two hourly runs with a four-hour hole between them: bar 10 follows bar 9 immediately on
    // the axis even though five hours of wall clock separate them.
    auto bars = series(kSep7, kHour, 20);
    for (size_t i = 10; i < 20; ++i)
        bars[i].open_ms += 4 * kHour;
    const TimeTicks ticks = time_ticks(bars.data(), 0, 19, kHour, 40.0F, 80.0F);
    REQUIRE(ticks.count > 0);
    for (int i = 0; i < ticks.count; ++i)
        CHECK(ticks.ticks[i].index <= 19);
}

TEST_CASE("time_ticks tolerates junk input") {
    const auto bars = series(kSep7, kHour, 4);
    CHECK(time_ticks(nullptr, 0, 3, kHour, 10.0F, 80.0F).count == 0);
    CHECK(time_ticks(bars.data(), 3, 2, kHour, 10.0F, 80.0F).count == 0);
    CHECK(time_ticks(bars.data(), 0, 3, kHour, 0.0F, 80.0F).count == 0);
    CHECK(time_ticks(bars.data(), 0, 3, 0, 10.0F, 80.0F).count == 0);
}

TEST_CASE("format_bar_time follows the timeframe's own precision") {
    char buf[32];
    CHECK(std::strcmp(format_bar_time(kSep7 + 14 * kHour + 30 * kMinute, kMinute, buf, sizeof(buf)),
                      "7 Sep 14:30") == 0);
    CHECK(std::strcmp(
              format_bar_time(kSep7 + 14 * kHour + 30 * kMinute + 5000, 5000, buf, sizeof(buf)),
              "7 Sep 14:30:05") == 0);
    CHECK(std::strcmp(format_bar_time(kSep7, kDay, buf, sizeof(buf)), "7 Sep 2026") == 0);
}

TEST_CASE("bar_seconds_left counts down and then stops") {
    CHECK(bar_seconds_left(kSep7, kMinute, kSep7) == 60);
    CHECK(bar_seconds_left(kSep7, kMinute, kSep7 + 30'000) == 30);
    CHECK(bar_seconds_left(kSep7, kMinute, kSep7 + 59'500) == 1);
    CHECK(bar_seconds_left(kSep7, kMinute, kSep7 + kMinute) == 0);
    CHECK(bar_seconds_left(kSep7, kMinute, kSep7 + 10 * kMinute) == 0);
    CHECK(bar_seconds_left(kSep7, 0, kSep7) == 0);
}
