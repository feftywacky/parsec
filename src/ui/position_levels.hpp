#pragma once
// The price levels a position puts on the screen: its liquidation price, and where to find the
// position for a given asset in the portfolio snapshot.
//
// Shared between the positions table and the chart's overlays specifically so the two cannot
// disagree. A liquidation line drawn at a different price from the one the table reports is
// worse than no line at all -- the trader would have no way to tell which of the two is the
// number their margin actually depends on.
#include <cstdint>

#include "app/ui_bridge.hpp"
#include "core/units.hpp"
#include "parsec/parsec.h"
#include "portfolio/order_preview.hpp"

namespace pc::ui {

// The position held on `asset`, or nullptr when flat. The snapshot compacts PositionBook's
// sparse table into a dense list, so this is a scan rather than an index.
inline const portfolio::Position* find_position(const app::PortfolioSnapshot& portfolio,
                                                uint32_t asset) noexcept {
    if (asset == PC_ASSET_NONE)
        return nullptr;
    for (uint32_t i = 0; i < portfolio.position_count; ++i) {
        if (portfolio.positions[i].asset == asset)
            return &portfolio.positions[i];
    }
    return nullptr;
}

// The position's liquidation price: the venue's own `liquidationPx` when it has published one,
// otherwise this client's estimate.
//
// The venue reports null (0 here) both for a position that cannot be liquidated at any price
// and -- far more commonly -- for the first snapshot or two after opening, which is exactly
// when a trader goes looking for it. `is_estimate` is set when the returned value is the
// fallback, so every caller can label it rather than passing a guess off as venue truth.
inline Px liquidation_px(const pc_position& p, const app::PortfolioSnapshot& portfolio,
                         uint32_t max_leverage, bool* is_estimate) noexcept {
    if (is_estimate)
        *is_estimate = false;
    if (p.liq_px > 0)
        return p.liq_px;
    if (p.szi == 0 || p.entry_px <= 0 || !portfolio.account_valid)
        return 0;

    const Usd maintenance_rate = portfolio::maintenance_rate_for_max_leverage(max_leverage);
    if (maintenance_rate <= 0)
        return 0;
    const Usd maintenance = portfolio::maintenance_margin(p.position_value, maintenance_rate);

    // Isolated: only this position's own margin stands behind it. Cross: the whole account
    // equity does, less what every cross position (including this one) must keep posted.
    const Usd margin_available =
        p.is_cross ? portfolio.account.account_value - portfolio.account.cross_maintenance_margin
                   : p.margin_used - maintenance;
    if (margin_available < 0)
        return 0;

    const Px estimate = portfolio::estimated_liquidation_price(p.entry_px, p.szi,
                                                               margin_available,
                                                               maintenance_rate);
    if (estimate > 0 && is_estimate)
        *is_estimate = true;
    return estimate;
}

}  // namespace pc::ui
