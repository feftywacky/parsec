#include <doctest/doctest.h>

#include "portfolio/order_preview.hpp"

using namespace pc;
using namespace pc::portfolio;

TEST_CASE("order preview reports initial margin and effective fee in fixed-point USDC") {
    const Usd value = 100 * kScale;
    CHECK(initial_margin(value, 10) == 10 * kScale);
    CHECK(fee_estimate(value, 45'000) == 4'500'000);  // 0.045% = $0.045 on $100
    CHECK(fee_estimate(value, -15'000) == -1'500'000);  // maker rebate
    CHECK(maintenance_rate_for_max_leverage(50) == kScale / 100);
}

TEST_CASE("order preview converts a USDC notional to base quantity") {
    CHECK(qty_from_notional(100 * kScale, 20 * kScale) == 5 * kScale);
    CHECK(qty_from_notional(0, 20 * kScale) == 0);
    CHECK(qty_from_notional(100 * kScale, 0) == 0);
}

TEST_CASE("order preview liquidation formula handles long and short positions") {
    const Px entry = 100 * kScale;
    const Qty one = kScale;
    const Usd margin_after_maintenance = 9 * kScale;
    const Usd maintenance = kScale / 100;  // 1%

    const Px long_liq = estimated_liquidation_price(entry, one, margin_after_maintenance,
                                                     maintenance);
    const Px short_liq = estimated_liquidation_price(entry, -one, margin_after_maintenance,
                                                      maintenance);

    // The exact integer result is deliberately retained; the bounds make the test readable
    // while allowing the fixed-point division order to change by a few 1e-8 units.
    CHECK(long_liq > 90 * kScale);
    CHECK(long_liq < 91 * kScale);
    CHECK(short_liq > 108 * kScale);
    CHECK(short_liq < 109 * kScale);
}

TEST_CASE("order preview refuses liquidation estimates without usable inputs") {
    CHECK(estimated_liquidation_price(100 * kScale, 0, 9 * kScale, kScale / 100) == 0);
    CHECK(estimated_liquidation_price(100 * kScale, kScale, 9 * kScale, -1) == 0);
    CHECK(estimated_liquidation_price(100 * kScale, kScale, -1, kScale / 100) == 0);
}

TEST_CASE("order preview scales a price move into return on posted margin") {
    // The ticket's headline case: a 10.666...% take-profit on a 10x long is a ~106.67% return
    // on the margin actually posted, not a 10.67% one.
    const int64_t ten_pct = kScale / 10;
    CHECK(roe_from_price_move(ten_pct, 10) == kScale);
    CHECK(roe_from_price_move(ten_pct, 1) == ten_pct);
    // Leverage 0 is "unknown", not "multiply by zero" -- returning 0 would print a levered
    // trade as having no return at all.
    CHECK(roe_from_price_move(ten_pct, 0) == ten_pct);
    // Signs are preserved, so a stop still reads as a negative return.
    CHECK(roe_from_price_move(-ten_pct, 10) == -kScale);
}

TEST_CASE("order preview flags a stop the venue liquidates through first") {
    const Px liq_long = 68'354 * kScale;
    const Px liq_short = 81'000 * kScale;
    const Qty long_sz = kScale;
    const Qty short_sz = -kScale;

    // A -15% stop on a 10x long from 75,000 lands at 63,750, well below liquidation.
    CHECK(stop_beyond_liquidation(63'750 * kScale, liq_long, long_sz));
    CHECK(stop_beyond_liquidation(liq_long, liq_long, long_sz));  // exactly at it still loses
    CHECK_FALSE(stop_beyond_liquidation(70'000 * kScale, liq_long, long_sz));

    // Mirrored for a short: the stop is above entry, and liquidation is above the stop.
    CHECK(stop_beyond_liquidation(85'000 * kScale, liq_short, short_sz));
    CHECK_FALSE(stop_beyond_liquidation(79'000 * kScale, liq_short, short_sz));

    // An unknown liquidation price is never reported as unsafe.
    CHECK_FALSE(stop_beyond_liquidation(63'750 * kScale, 0, long_sz));
    CHECK_FALSE(stop_beyond_liquidation(0, liq_long, long_sz));
    CHECK_FALSE(stop_beyond_liquidation(63'750 * kScale, liq_long, 0));
}
