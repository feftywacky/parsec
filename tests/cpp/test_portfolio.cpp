#include <doctest/doctest.h>

#include "core/units.hpp"
#include "portfolio/account_state.hpp"
#include "portfolio/pnl.hpp"
#include "portfolio/position_book.hpp"
#include "portfolio/reconciler.hpp"

using namespace pc;
using namespace pc::portfolio;

// --- pnl.hpp -----------------------------------------------------------------------------

TEST_CASE("unrealized_pnl: hand-computed long and short cases") {
    // Long 2 BTC, entry 100000, mark 101000 -> +2000 usd.
    CHECK(unrealized_pnl(2 * kScale, 100'000 * kScale, 101'000 * kScale) == 2'000 * kScale);
    // Short 2 BTC (szi negative), entry 100000, mark 101000 -> -2000 usd (loses on a rally).
    CHECK(unrealized_pnl(-2 * kScale, 100'000 * kScale, 101'000 * kScale) == -2'000 * kScale);
    // Short 2 BTC, mark drops to 99000 -> +2000 usd.
    CHECK(unrealized_pnl(-2 * kScale, 100'000 * kScale, 99'000 * kScale) == 2'000 * kScale);
}

TEST_CASE("roe_bps: hand-computed") {
    // $500 pnl on $2000 margin = 25% = 2500bps.
    CHECK(roe_bps(500 * kScale, 2'000 * kScale) == 2'500);
    CHECK(roe_bps(100 * kScale, 0) == 0);  // no margin -> defined as 0, not a div-by-zero trap
}

TEST_CASE("isolated_margin: hand-computed") {
    // $10000 position at 10x leverage -> $1000 margin.
    CHECK(isolated_margin(10'000 * kScale, 10) == 1'000 * kScale);
    CHECK(isolated_margin(-10'000 * kScale, 10) == 1'000 * kScale);  // sign doesn't matter
}

TEST_CASE("realized_pnl_from_fill: fee already includes builderFee, no double count") {
    pc_fill fill{};
    fill.closed_pnl = 100 * kScale;
    fill.fee = 3 * kScale;  // this already includes any builderFee component
    CHECK(realized_pnl_from_fill(fill) == 97 * kScale);

    // A maker rebate (negative fee) increases realized P&L.
    fill.fee = -1 * kScale;
    CHECK(realized_pnl_from_fill(fill) == 101 * kScale);
}

// --- account_state.hpp ---------------------------------------------------------------------

namespace {
pc_fill test_fill(bool is_buy, Qty qty, Usd closed_pnl, Usd fee) {
    pc_fill f{};
    f.is_buy = is_buy;
    f.qty = qty;
    f.closed_pnl = closed_pnl;
    f.fee = fee;
    return f;
}
}  // namespace

TEST_CASE("realized_since_open: a prior position's closes are not charged to this one") {
    // Newest first: two opening sells for a 0.1452 short, then (older) the close of a long
    // that lost $1000. Only the two opening fees belong to the short.
    const pc_fill a = test_fill(false, kScale / 10, 0, kScale);
    const pc_fill b = test_fill(false, 452 * kScale / 10'000, 0, kScale / 2);
    const pc_fill c = test_fill(false, kScale, -1'000 * kScale, 2 * kScale);
    const pc_fill* fills[] = {&a, &b, &c};
    const PositionRealized r = realized_since_open(-1452 * kScale / 10'000, fills, 3);
    CHECK(r.pnl == -(kScale + kScale / 2));
    CHECK(r.fees == kScale + kScale / 2);
    CHECK(r.fills == 2);
    CHECK(r.complete);
}

TEST_CASE("realized_since_open: a flip keeps only its opening share of the fee") {
    // Sold 3 while long 2 -> short 1. closed_pnl is the long's; 1/3 of the fee is the short's.
    const pc_fill flip = test_fill(false, 3 * kScale, 50 * kScale, 3 * kScale);
    const pc_fill* fills[] = {&flip};
    const PositionRealized r = realized_since_open(-kScale, fills, 1);
    CHECK(r.pnl == -kScale);
    CHECK(r.fees == kScale);
    CHECK(r.complete);
}

TEST_CASE("realized_since_open: history that stops short of the open is flagged partial") {
    const pc_fill add = test_fill(false, kScale, 0, kScale);
    const pc_fill* fills[] = {&add};
    const PositionRealized r = realized_since_open(-2 * kScale, fills, 1);
    CHECK(r.pnl == -kScale);
    CHECK_FALSE(r.complete);
}

TEST_CASE("AccountState: authoritative snapshot re-bases the optimistic view") {
    AccountState acct;
    CHECK_FALSE(acct.has_snapshot());

    pc_account snap{};
    snap.account_value = 10'000 * kScale;
    snap.withdrawable = 8'000 * kScale;
    acct.apply_authoritative(snap);

    CHECK(acct.has_snapshot());
    CHECK(acct.optimistic().account_value == 10'000 * kScale);

    acct.apply_fill_delta(/*realized_pnl_delta=*/50 * kScale, /*fee=*/2 * kScale);
    CHECK(acct.optimistic().account_value == 10'048 * kScale);
    // The authoritative copy is untouched by the optimistic nudge, by design.
    CHECK(acct.authoritative()->account_value == 10'000 * kScale);
}

// --- reconciler.hpp -------------------------------------------------------------------------

namespace {
pc_position make_pos(Qty szi) {
    pc_position p{};
    p.szi = szi;
    return p;
}
}  // namespace

TEST_CASE("Reconciler: within tolerance is a no-op, nothing reported") {
    Reconciler r;
    pc_position local = make_pos(100 * kScale);
    const pc_position venue = make_pos(101 * kScale);
    const auto log = r.reconcile(/*asset=*/3, local, venue, /*tolerance=*/5 * kScale);
    CHECK_FALSE(log.has_value());
    CHECK(local.szi == 100 * kScale);  // untouched
}

TEST_CASE("Reconciler: first divergence snaps to venue and logs both values, no alarm yet") {
    Reconciler r;
    pc_position local = make_pos(100 * kScale);
    const pc_position venue = make_pos(150 * kScale);
    const auto log = r.reconcile(/*asset=*/3, local, venue, /*tolerance=*/kScale);
    REQUIRE(log.has_value());
    CHECK(log->local_szi == 100 * kScale);
    CHECK(log->venue_szi == 150 * kScale);
    CHECK(log->size_diff == 50 * kScale);
    CHECK_FALSE(log->alarm);
    CHECK(local.szi == 150 * kScale);  // snapped to venue truth
}

TEST_CASE("Reconciler: second consecutive divergence for the same asset raises the alarm") {
    Reconciler r;
    pc_position local = make_pos(100 * kScale);
    pc_position venue = make_pos(150 * kScale);
    auto first = r.reconcile(3, local, venue, kScale);
    REQUIRE(first.has_value());
    CHECK_FALSE(first->alarm);

    // Local drifts again (e.g. another local fill projected before the next snapshot).
    local.szi = 150 * kScale + 40 * kScale;
    venue.szi = 150 * kScale;
    auto second = r.reconcile(3, local, venue, kScale);
    REQUIRE(second.has_value());
    CHECK(second->alarm);
}

TEST_CASE("Reconciler: a clean check in between resets the consecutive counter") {
    Reconciler r;
    pc_position local = make_pos(100 * kScale);
    pc_position venue = make_pos(150 * kScale);
    REQUIRE(r.reconcile(3, local, venue, kScale).has_value());  // 1st divergence

    // Clean reconciliation: local now matches venue within tolerance.
    local.szi = venue.szi;
    CHECK_FALSE(r.reconcile(3, local, venue, kScale).has_value());

    // A fresh divergence after a clean check is again "first", not "second".
    venue.szi = local.szi + 40 * kScale;
    auto third = r.reconcile(3, local, venue, kScale);
    REQUIRE(third.has_value());
    CHECK_FALSE(third->alarm);
}

TEST_CASE("Reconciler: divergence is tracked independently per asset") {
    Reconciler r;
    pc_position local_a = make_pos(0), local_b = make_pos(0);
    pc_position venue_a = make_pos(50 * kScale), venue_b = make_pos(50 * kScale);

    auto a1 = r.reconcile(1, local_a, venue_a, kScale);
    auto b1 = r.reconcile(2, local_b, venue_b, kScale);
    REQUIRE(a1.has_value());
    REQUIRE(b1.has_value());
    CHECK_FALSE(a1->alarm);
    CHECK_FALSE(b1->alarm);  // asset 2's counter must not have been bumped by asset 1
}

// --- position_book.hpp -----------------------------------------------------------------------

TEST_CASE("PositionBook: apply/find/clear round-trip") {
    PositionBook book;
    CHECK(book.find(5) == nullptr);
    pc_position p{};
    p.szi = 42;
    book.apply(5, p);
    REQUIRE(book.find(5) != nullptr);
    CHECK(book.find(5)->value.szi == 42);
    book.clear();
    CHECK(book.find(5) == nullptr);
}
