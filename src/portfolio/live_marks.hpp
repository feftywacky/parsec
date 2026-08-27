#pragma once

#include "core/units.hpp"
#include "parsec/parsec.h"
#include "portfolio/order_preview.hpp"

namespace pc::portfolio {

// A `clearinghouseState` account snapshot re-marked to a price newer than the snapshot.
//
// The snapshot lands about every 4 s, but the Positions tab re-marks its rows from the live
// `activeAssetCtx` mark on every frame. Printing the raw snapshot beside it meant the two
// disagreed at the same instant -- Positions saying mark 78,869 / unrealized $935 while
// Balances still carried mark 78,911 / unrealized $904, a ~$31 gap in account value that read
// as "the Balances tab is not updating". Applying the same re-mark here keeps them together.
struct AccountMarks {
    Usd account_value{};
    Usd cross_account_value{};
    Usd total_margin_used{};
    Usd total_ntl_pos{};
    Usd withdrawable{};
    Usd cross_maintenance{};
};

inline AccountMarks account_marks(const pc_account& a) noexcept {
    return {a.account_value, a.cross_account_value, a.total_margin_used,
            a.total_ntl_pos, a.withdrawable,        a.cross_maintenance_margin};
}

// Moves `marks` from the snapshot mark implied by `p.position_value` to `live_mark`.
//
// Only the instrument on screen has a live mark, so this is called for that position alone;
// every other position's snapshot mark is still the venue's own and contributes nothing.
// `maintenance_rate_1e8` is the position asset's rate (maintenance_rate_for_max_leverage).
//
// Cross and isolated are different accounting and must not share a branch:
//
//   cross     `marginUsed` is marked to market as position_value / leverage, and withdrawable
//             is what equity has left once that requirement grows. Both move.
//   isolated  `marginUsed` is the position's whole equity -- the venue reports
//             `positionValue + rawUsd` with `rawUsd` fixed at entry (docs/03 §2) -- so it
//             tracks the mark one-for-one rather than at 1/leverage. And the collateral is
//             walled off: its PnL is not cross free cash, so `withdrawable` must not move at
//             all, nor may `crossMarginSummary` / `crossMaintenanceMarginUsed`.
inline void apply_live_mark(AccountMarks& marks, const pc_position& p, Px live_mark,
                            Usd maintenance_rate_1e8) noexcept {
    const Qty abs_size = p.szi < 0 ? -p.szi : p.szi;
    if (live_mark <= 0 || abs_size <= 0 || p.position_value == 0 || p.leverage == 0)
        return;

    const Usd snap_value = p.position_value < 0 ? -p.position_value : p.position_value;
    const Usd live_value = notional(live_mark, abs_size);
    const Usd d_value = live_value - snap_value;
    if (d_value == 0)
        return;

    // A long gains when the position is worth more; a short loses by the same amount.
    const Usd d_unrealized = p.szi > 0 ? d_value : -d_value;

    // `marginSummary` spans both margin modes, so these move either way.
    marks.account_value += d_unrealized;
    marks.total_ntl_pos += d_value;

    if (p.is_cross == 0) {
        marks.total_margin_used += d_unrealized;
        return;
    }

    const Usd d_margin =
        initial_margin(live_value, p.leverage) - initial_margin(snap_value, p.leverage);
    marks.cross_account_value += d_unrealized;
    marks.total_margin_used += d_margin;
    marks.withdrawable += d_unrealized - d_margin;
    if (maintenance_rate_1e8 > 0)
        marks.cross_maintenance += maintenance_margin(live_value, maintenance_rate_1e8) -
                                   maintenance_margin(snap_value, maintenance_rate_1e8);
}

}  // namespace pc::portfolio
