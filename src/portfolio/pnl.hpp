#pragma once
#include "core/units.hpp"
#include "parsec/parsec.h"
namespace pc::portfolio {

// szi is the signed position size (positive long, negative short), so mark - entry naturally
// flips sign for shorts. Mark-to-market, not last-trade, per docs/02 SS6.4: "recomputed from
// AssetCtx::mark on every tick ... keeps the UI's unrealized P&L identical to the venue's
// definition".
inline Usd unrealized_pnl(Qty szi, Px entry, Px mark) noexcept {
    return notional(mark - entry, szi);
}

inline int32_t roe_bps(Usd pnl, Usd margin) noexcept {
    return margin ? static_cast<int32_t>(static_cast<__int128>(pnl) * 10'000 / margin) : 0;
}

inline Usd isolated_margin(Usd position_value, uint32_t leverage) noexcept {
    return leverage ? abs_px(position_value) / static_cast<Usd>(leverage) : 0;
}

// Net realized-P&L effect of a single fill, in Usd. `fill.fee` (the wire `fee` field) already
// includes `builderFee` -- adding builderFee again here would double-count it
// (docs/03-hyperliquid-api.md SS W2.9). `fee` is subtracted: a positive fee reduces realized
// P&L, a negative fee (a maker rebate) increases it. `fill.is_taker` mirrors the wire's
// `crossed` flag -- true means this fill was the taker leg, which is the cheapest available
// maker/taker attribution for fee reporting.
inline Usd realized_pnl_from_fill(const pc_fill& fill) noexcept {
    return fill.closed_pnl - fill.fee;
}

}  // namespace pc::portfolio
