#include <doctest/doctest.h>

#include "core/units.hpp"
#include "portfolio/collateral.hpp"
#include "portfolio/live_marks.hpp"

using namespace pc;
using namespace pc::portfolio;

namespace {

// One coherent mainnet snapshot of the account the collateral bug was measured on -- taken
// between two clearinghouseState pulls that agreed, so all three payloads describe the same
// instant (docs/03 §2 shapes):
//   clearinghouseState     accountValue 9820.495181  totalMarginUsed 5871.93276
//                          withdrawable 37.463671    crossMaintenanceMarginUsed 733.991595
//   spotClearinghouseState USDC total 11254.28527    hold 9822.142497
//   activeAssetData BTC    availableToTrade [buy 17109.xx, sell 1471.23762]
// The account is a single 10x cross short of 0.74878 BTC, so the SELL side is the one that
// opens exposure -- its availableToTrade is the venue's own free-margin answer, while the buy
// side is inflated by the margin that closing the short would release.
constexpr Usd kMeasuredAccountValue = 982'049'518'100;  //  9,820.495181
constexpr Usd kMeasuredWithdrawable = 3'746'367'100;    //     37.463671
constexpr Usd kMeasuredSpotTotal = 1'125'428'527'000;   // 11,254.28527
constexpr Usd kMeasuredSpotHold = 982'214'249'700;      //  9,822.142497
constexpr Usd kVenueAvailable = 147'123'762'000;        //  1,471.23762

pc_spot measured_spot() {
    pc_spot s{};
    s.total = kMeasuredSpotTotal;
    s.hold = kMeasuredSpotHold;
    return s;
}

pc_position position(Qty szi, Usd position_value, uint32_t leverage, bool cross) {
    pc_position p{};
    p.szi = szi;
    p.position_value = position_value;
    p.leverage = leverage;
    p.is_cross = cross ? 1 : 0;
    return p;
}

}  // namespace

// --- collateral.hpp --------------------------------------------------------------------

TEST_CASE("free_collateral matches the venue's own availableToTrade on a measured account") {
    // The bug: `withdrawable` alone was reported as free margin, so the ticket's slider
    // offered a max size worth ~$1,471 of margin and the gate below it then blocked that
    // exact size for needing "more margin than is available" -- $37.46.
    CHECK(kMeasuredWithdrawable < kVenueAvailable / 30);

    const Usd free =
        free_collateral(kMeasuredAccountValue, kMeasuredWithdrawable, measured_spot(), true);
    // Within 5 cents of what the venue says the opening side can spend.
    CHECK(free > kVenueAvailable - kScale / 20);
    CHECK(free < kVenueAvailable + kScale / 20);
}

TEST_CASE("free_collateral: the spot pool CONTAINS perp equity, it is not added to it") {
    pc_spot spot{};
    spot.total = 11'000 * kScale;
    spot.hold = 0;
    // 10,000 of the 11,000 is already working as perp equity; only 1,000 is idle.
    CHECK(undeployed_usdc(10'000 * kScale, spot) == 1'000 * kScale);
    CHECK(free_collateral(10'000 * kScale, 250 * kScale, spot, true) == 1'250 * kScale);
}

TEST_CASE("free_collateral: degrades to withdrawable, never to a guess") {
    pc_spot spot{};
    spot.total = 11'000 * kScale;
    // No spot snapshot yet -- understating free margin blocks an order, overstating it lets
    // one through for the venue to reject, so the perp side alone is the safe fallback.
    CHECK(free_collateral(10'000 * kScale, 250 * kScale, spot, false) == 250 * kScale);

    // A spot snapshot older than the account snapshot can put perp equity above the pool.
    // That is a timing artefact, not negative money.
    pc_spot stale{};
    stale.total = 9'000 * kScale;
    CHECK(undeployed_usdc(10'000 * kScale, stale) == 0);
    CHECK(free_collateral(10'000 * kScale, 250 * kScale, stale, true) == 250 * kScale);

    // The venue never reports withdrawable below zero, but the live re-mark can.
    CHECK(free_collateral(10'000 * kScale, -400 * kScale, stale, true) == 0);
}

// --- live_marks.hpp --------------------------------------------------------------------

TEST_CASE("apply_live_mark: cross moves margin at 1/leverage and frees the remainder") {
    pc_account snap{};
    snap.account_value = 10'000 * kScale;
    snap.cross_account_value = 10'000 * kScale;
    snap.total_margin_used = 1'000 * kScale;
    snap.total_ntl_pos = 10'000 * kScale;
    snap.withdrawable = 500 * kScale;

    AccountMarks m = account_marks(snap);
    // Long 1 unit at 10x, snapshot mark 10,000, live mark 10,100 -> +100 unrealized.
    apply_live_mark(m, position(1 * kScale, 10'000 * kScale, 10, /*cross=*/true), 10'100 * kScale,
                    /*maintenance_rate_1e8=*/0);

    CHECK(m.account_value == 10'100 * kScale);
    CHECK(m.cross_account_value == 10'100 * kScale);
    CHECK(m.total_ntl_pos == 10'100 * kScale);
    CHECK(m.total_margin_used == 1'010 * kScale);        // 10,100 / 10
    CHECK(m.withdrawable == (500 + 100 - 10) * kScale);  // pnl, less the extra requirement
}

TEST_CASE("apply_live_mark: isolated moves margin by the whole PnL and never touches cross") {
    pc_account snap{};
    snap.account_value = 10'000 * kScale;
    snap.cross_account_value = 9'000 * kScale;  // 1,000 of the equity is walled off
    snap.total_margin_used = 1'000 * kScale;
    snap.total_ntl_pos = 10'000 * kScale;
    snap.withdrawable = 500 * kScale;
    snap.cross_maintenance_margin = 125 * kScale;

    AccountMarks m = account_marks(snap);
    apply_live_mark(m, position(1 * kScale, 10'000 * kScale, 10, /*cross=*/false), 10'100 * kScale,
                    /*maintenance_rate_1e8=*/kScale / 80);

    CHECK(m.account_value == 10'100 * kScale);
    CHECK(m.total_ntl_pos == 10'100 * kScale);
    // The venue reports isolated marginUsed as positionValue + rawUsd with rawUsd fixed at
    // entry, so it tracks the mark one-for-one -- not at 1/leverage, which would be +10.
    CHECK(m.total_margin_used == 1'100 * kScale);
    // Walled-off collateral: none of this is cross free cash, cross equity, or charged
    // against the cross maintenance requirement.
    CHECK(m.withdrawable == 500 * kScale);
    CHECK(m.cross_account_value == 9'000 * kScale);
    CHECK(m.cross_maintenance == 125 * kScale);
}

TEST_CASE("apply_live_mark: a short gains as the mark falls") {
    pc_account snap{};
    snap.account_value = 10'000 * kScale;
    snap.cross_account_value = 10'000 * kScale;
    snap.total_ntl_pos = 10'000 * kScale;

    AccountMarks m = account_marks(snap);
    apply_live_mark(m, position(-1 * kScale, 10'000 * kScale, 10, /*cross=*/true), 9'900 * kScale,
                    0);

    CHECK(m.account_value == 10'100 * kScale);  // +100 on the short
    CHECK(m.total_ntl_pos == 9'900 * kScale);   // exposure shrank with the mark
}

TEST_CASE("apply_live_mark: no live mark, no position, or no move leaves the snapshot alone") {
    pc_account snap{};
    snap.account_value = 10'000 * kScale;
    snap.withdrawable = 500 * kScale;
    const AccountMarks base = account_marks(snap);

    AccountMarks m = base;
    apply_live_mark(m, position(1 * kScale, 10'000 * kScale, 10, true), 0, 0);
    CHECK(m.account_value == base.account_value);

    m = base;
    apply_live_mark(m, position(0, 0, 10, true), 10'100 * kScale, 0);
    CHECK(m.account_value == base.account_value);

    // Live mark equal to the snapshot mark: nothing to re-mark.
    m = base;
    apply_live_mark(m, position(1 * kScale, 10'000 * kScale, 10, true), 10'000 * kScale, 0);
    CHECK(m.account_value == base.account_value);
    CHECK(m.withdrawable == base.withdrawable);
}
