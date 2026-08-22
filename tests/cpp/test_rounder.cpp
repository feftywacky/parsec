#include <doctest/doctest.h>

#include <cstdint>
#include <limits>

#include "core/units.hpp"
#include "exec/rounder.hpp"

using namespace pc;
using namespace pc::exec;

namespace {

// Helper: build a Px from a real-valued price given as (integer_part, fractional numerator,
// fractional denominator-as-power-of-ten) would be fiddly: tests below just write the scaled
// integer directly, or use px_of() for a decimal literal expressed in units of 1e-8.
constexpr Px px_of(int64_t scaled_1e8) noexcept {
    return scaled_1e8;
}

}  // namespace

TEST_CASE("doc-verbatim examples: szDecimals = 0 (implied by the plain examples)") {
    // 1234.5 valid; 1234.56 invalid (too many significant figures)
    CHECK(Rounder::is_valid_px(px_of(123450000000LL), {0}));
    CHECK_FALSE(Rounder::is_valid_px(px_of(123456000000LL), {0}));

    // 0.001234 valid; 0.0012345 invalid (more than 6 decimals)
    CHECK(Rounder::is_valid_px(px_of(123400), {0}));
    CHECK_FALSE(Rounder::is_valid_px(px_of(123450), {0}));
}

TEST_CASE("doc-verbatim example: szDecimals = 1") {
    // 0.01234 valid; 0.012345 invalid (more than 6-1=5 decimals)
    CHECK(Rounder::is_valid_px(px_of(1234000), {1}));
    CHECK_FALSE(Rounder::is_valid_px(px_of(1234500), {1}));
}

TEST_CASE("integer prices are always valid regardless of significant figures") {
    CHECK(Rounder::is_valid_px(px_of(123456LL * kScale), {0}));  // 6 sig figs, but integer
    CHECK(Rounder::is_valid_px(px_of(1LL * kScale), {6}));
    CHECK(Rounder::is_valid_px(px_of(500000LL * kScale), {5}));
    // Non-integer with the same digit pattern is rejected.
    CHECK_FALSE(Rounder::is_valid_px(px_of(1234560000LL), {0}));  // 12345.6
}

TEST_CASE("the bug this rewrite fixes: sub-1.0 prices still obey the 5-sig-fig rule") {
    // 0.999999 has 6 significant figures and is not an integer -> invalid, even though it's
    // below 1.0. The previous implementation skipped the sig-fig check entirely below 1.0.
    CHECK_FALSE(Rounder::is_valid_px(px_of(99999900), {0}));
    // 0.99999 (5 sig figs) is fine.
    CHECK(Rounder::is_valid_px(px_of(99999000), {0}));
}

TEST_CASE("exact five-significant-figure boundaries") {
    // Five-digit integer part, no fraction allowed beyond it at szDecimals=0.
    CHECK(Rounder::is_valid_px(px_of(12345LL * kScale), {0}));
    // Adding one more significant digit without becoming an integer is invalid.
    CHECK_FALSE(Rounder::is_valid_px(px_of(1234560000LL), {0}));  // 12345.6 (6 sig figs)
    // Small magnitude, exactly 5 sig figs spanning into the fraction (needs szDecimals=0 so
    // the 6-decimal-place budget can actually reach down to the 5th significant digit).
    CHECK(Rounder::is_valid_px(px_of(1234500), {0}));  // 0.012345, 5 sig figs, 6 decimals
}

TEST_CASE("round_sz floors to szDecimals") {
    CHECK(Rounder::round_sz(1'001'000'00, {3}) == 1'001'000'00);  // 1.001 exact at 3dp
    CHECK(Rounder::round_sz(1'000'100'00, {3}) == 1'000'000'00);  // 1.0001 floors to 1.000
    CHECK(Rounder::round_sz(0, {0}) == 0);
}

TEST_CASE("check_min_notional at $10") {
    CHECK(Rounder::check_min_notional(10 * kScale, kScale));       // $10 * 1 = $10, ok
    CHECK_FALSE(Rounder::check_min_notional(9 * kScale, kScale));  // $9, rejected
    CHECK(Rounder::check_min_notional(5 * kScale, 2 * kScale));    // $5 * 2 = $10, ok
}

TEST_CASE("RoundMode::Passive never makes a limit order more aggressive") {
    for (uint8_t szd = 0; szd <= 6; ++szd) {
        const AssetPrecision p{szd};
        for (Px raw : {px_of(1), px_of(9999), px_of(123456789), px_of(1'234'567'890'123LL),
                       px_of(500'000LL * kScale), px_of(1)}) {
            if (raw <= 0)
                continue;
            const Px buy = Rounder::round_px(raw, Side::Buy, p, RoundMode::Passive);
            const Px sell = Rounder::round_px(raw, Side::Sell, p, RoundMode::Passive);
            CHECK(buy <= raw);
            CHECK(sell >= raw);
        }
    }
}

TEST_CASE("RoundMode::Aggressive is the mirror image of Passive") {
    for (uint8_t szd = 0; szd <= 6; ++szd) {
        const AssetPrecision p{szd};
        for (Px raw : {px_of(37), px_of(999999), px_of(9'876'543'210LL)}) {
            const Px buy = Rounder::round_px(raw, Side::Buy, p, RoundMode::Aggressive);
            const Px sell = Rounder::round_px(raw, Side::Sell, p, RoundMode::Aggressive);
            CHECK(buy >= raw);
            CHECK(sell <= raw);
        }
    }
}

TEST_CASE("marketable_px moves against the trader and stays marketable") {
    const Px mid = px_of(100'000LL * kScale);  // $100,000
    const AssetPrecision p{5};
    const Px buy = Rounder::marketable_px(mid, Side::Buy, 500, p);
    const Px sell = Rounder::marketable_px(mid, Side::Sell, 500, p);
    CHECK(buy > mid);   // buy chases the price up
    CHECK(sell < mid);  // sell chases the price down
    CHECK(Rounder::is_valid_px(buy, p));
    CHECK(Rounder::is_valid_px(sell, p));

    // Default-bps overload matches the explicit 500bps call.
    CHECK(Rounder::marketable_px(mid, Side::Buy, p) == buy);
}

// --- Exhaustive / property sweep -------------------------------------------------------------
//
// This is the acceptance bar from docs/07 Phase 4: szDecimals 0..6, prices spanning
// 0.0001 -> 500000, and a doctest-strength check that round_px's output ALWAYS satisfies
// is_valid_px -- independently implemented predicate, not the rounding code path itself.

TEST_CASE("property: round_px output is always valid, across a wide sweep") {
    // Geometric-ish sweep of mantissas across many magnitudes, covering 0.0001 .. 500000 and
    // beyond, plus exact powers of ten and their neighbours (the digit-count boundaries where
    // the significant-figure step can shift).
    int64_t mantissas[] = {1,     3,     7,      9,      11,     37,     99,     100,
                           101,   999,   1000,   1001,   9999,   10000,  10001,  12345,
                           54321, 99999, 100000, 100001, 123456, 500000, 999999, 1000000};
    int exponents[] = {-4, -3, -2, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8};  // scales raw by 10^exp

    long checked = 0;
    for (uint8_t szd = 0; szd <= 6; ++szd) {
        const AssetPrecision p{szd};
        for (int64_t m : mantissas) {
            for (int e : exponents) {
                // raw (real price) = m * 10^e; convert to Px (scaled 1e8): m * 10^(e+8).
                const int shift = e + 8;
                if (shift < 0 || shift > 18)
                    continue;
                __int128 scaled = static_cast<__int128>(m);
                for (int i = 0; i < shift; ++i)
                    scaled *= 10;
                if (scaled <= 0 || scaled > std::numeric_limits<Px>::max())
                    continue;
                const Px raw = static_cast<Px>(scaled);

                for (auto side : {Side::Buy, Side::Sell}) {
                    for (auto mode : {RoundMode::Passive, RoundMode::Aggressive}) {
                        const Px rounded = Rounder::round_px(raw, side, p, mode);
                        CAPTURE(raw);
                        CAPTURE(static_cast<int>(szd));
                        CAPTURE(static_cast<int>(side));
                        CAPTURE(static_cast<int>(mode));
                        CAPTURE(rounded);
                        // rounded == 0 is the legitimate "unrepresentable in this direction"
                        // sentinel: e.g. flooring a $0.10 price towards passive with
                        // szDecimals=6 (decimal_step = $1, no sub-dollar prices exist at
                        // all) cannot produce a nonzero price without becoming more
                        // aggressive than the trader asked. Everything else must be valid.
                        CHECK((rounded == 0 || Rounder::is_valid_px(rounded, p)));
                        ++checked;
                    }
                }
            }
        }
    }
    CHECK(checked > 1000);  // sanity: the sweep actually ran a lot of cases
}
