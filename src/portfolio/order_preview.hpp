#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include "core/units.hpp"

namespace pc::portfolio {

// The venue expresses margin and fees as USDC fixed-point values. Keeping these small preview
// helpers in the core (rather than in ImGui code) makes the ticket's arithmetic testable and
// keeps float rounding out of a mainnet order decision.
inline Usd initial_margin(Usd order_value, uint32_t leverage) noexcept {
    if (order_value <= 0 || leverage == 0)
        return 0;
    return static_cast<Usd>(static_cast<__int128>(order_value) / leverage);
}

inline Usd fee_estimate(Usd order_value, int64_t rate_1e8) noexcept {
    if (order_value == 0 || rate_1e8 == 0)
        return 0;
    const __int128 value = static_cast<__int128>(order_value) * rate_1e8 / kScale;
    if (value > std::numeric_limits<Usd>::max())
        return std::numeric_limits<Usd>::max();
    if (value < std::numeric_limits<Usd>::min())
        return std::numeric_limits<Usd>::min();
    return static_cast<Usd>(value);
}

// Convert a USDC notional budget into base-asset quantity -- used by the ticket's USD-
// denominated size entry.
//
// NB: the venue's `availableToTrade` is NOT a notional; it is the free MARGIN in USDC, which
// is why `maxTradeSzs * markPx` comes out at `availableToTrade * leverage` in Hyperliquid's
// own activeAssetData examples (docs/03 §W8 #6). Pass a notional here, not availableToTrade.
inline Qty qty_from_notional(Usd value, Px price) noexcept {
    if (value <= 0 || price <= 0)
        return 0;
    const __int128 qty = static_cast<__int128>(value) * kScale / price;
    if (qty > std::numeric_limits<Qty>::max())
        return std::numeric_limits<Qty>::max();
    return static_cast<Qty>(qty);
}

// Hyperliquid documents maintenance margin as half the initial margin at max leverage for the
// base tier. Margin tiers can make the exact rate lower/higher for a large order, so the ticket
// labels the result as an estimate and uses the selected asset's max leverage as its input.
inline Usd maintenance_rate_for_max_leverage(uint32_t max_leverage) noexcept {
    if (max_leverage == 0)
        return 0;
    const uint64_t denominator = static_cast<uint64_t>(max_leverage) * 2;
    return static_cast<Usd>(static_cast<uint64_t>(kScale) / denominator);
}

// Scales a move of the coin's price into the move the POSITION sees. `pct_1e8` is a
// kScale-scaled signed fraction of the entry price; the position moves `leverage` times as far,
// because the margin behind it is only order_value / leverage while the PnL is order_value *
// pct. This is what the ticket's TP/SL `%` field is denominated in: at 10x, "stop at 10%" fires
// on a 1% move in the coin, and a "15%" stop would need a 1.5% move -- while liquidation
// arrives at roughly 100%, which is the confusion this exists to remove.
inline int64_t roe_from_price_move(int64_t pct_1e8, uint32_t leverage) noexcept {
    if (leverage == 0)
        return pct_1e8;
    const __int128 value = static_cast<__int128>(pct_1e8) * leverage;
    if (value > std::numeric_limits<int64_t>::max())
        return std::numeric_limits<int64_t>::max();
    if (value < std::numeric_limits<int64_t>::min())
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(value);
}

// True when a protective stop sits at or beyond the liquidation price, i.e. the venue closes
// the position before the stop can ever trigger. `signed_size` is positive for a long.
// A zero/unknown liquidation estimate is never reported as unsafe.
inline bool stop_beyond_liquidation(Px stop_px, Px liquidation_px, Qty signed_size) noexcept {
    if (stop_px <= 0 || liquidation_px <= 0 || signed_size == 0)
        return false;
    return signed_size > 0 ? stop_px <= liquidation_px : stop_px >= liquidation_px;
}

// `margin_available` is the account/isolated margin left after the estimated maintenance
// requirement. The formula is the venue's documented liquidation-price formula, expressed in
// fixed-point arithmetic. `signed_size` is positive for a long and negative for a short.
inline Px estimated_liquidation_price(Px entry_px, Qty signed_size, Usd margin_available,
                                      Usd maintenance_rate_1e8) noexcept {
    if (entry_px <= 0 || signed_size == 0 || margin_available < 0 ||
        maintenance_rate_1e8 < 0 || maintenance_rate_1e8 >= kScale)
        return 0;

    const __int128 size = signed_size < 0 ? -static_cast<__int128>(signed_size)
                                          : static_cast<__int128>(signed_size);
    const __int128 side = signed_size > 0 ? 1 : -1;
    const __int128 denominator = static_cast<__int128>(kScale) - side * maintenance_rate_1e8;
    if (denominator <= 0)
        return 0;

    // margin/size has price units. The two scale conversions turn the raw USDC and raw size
    // into a raw price, without passing through a lossy double.
    const __int128 move = static_cast<__int128>(margin_available) * kScale * kScale /
                          (size * denominator);
    const __int128 result = static_cast<__int128>(entry_px) - side * move;
    if (result <= 0 || result > std::numeric_limits<Px>::max())
        return 0;
    return static_cast<Px>(result);
}

}  // namespace pc::portfolio
