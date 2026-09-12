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

// Realized P&L of the position currently held, as opposed to everything the coin has done.
// `complete` is false when the fills ran out before reaching the one that opened the position,
// i.e. the open predates the fill history held -- the figure is then a lower bound on history.
struct PositionRealized {
    Usd pnl{};   // net of fees, like realized_pnl_from_fill
    Usd fees{};
    uint32_t fills{};
    bool complete{};
};

// Walks `fills` (this coin only, NEWEST first) backwards from the current signed size `szi`,
// undoing each fill, and stops at the one that opened the position: the fill whose pre-fill
// size was flat, or the fill that flipped it through zero. Only that stretch belongs to this
// position -- a previous position's closes (and the closing half of a flip, whose closed_pnl
// is the OLD position's) must not be charged to it. A flip's fee is split pro rata, with the
// share for the size it opened counted here.
//
// `Fill` is anything with is_buy, qty (unsigned), closed_pnl and fee -- pc_fill or a UI row.
template <class Fill>
PositionRealized realized_since_open(Qty szi, const Fill* const* fills, size_t n) noexcept {
    PositionRealized out{};
    if (szi == 0) {
        out.complete = true;
        return out;
    }
    Qty size = szi;
    for (size_t i = 0; i < n; ++i) {
        const Fill& f = *fills[i];
        if (f.qty <= 0)
            continue;
        const Qty before = size - (f.is_buy ? f.qty : -f.qty);
        ++out.fills;
        if (before != 0 && (before > 0) != (size > 0)) {
            const Qty opened = size < 0 ? -size : size;
            const Usd fee = static_cast<Usd>(static_cast<__int128>(f.fee) * opened / f.qty);
            out.pnl -= fee;
            out.fees += fee;
            out.complete = true;
            return out;
        }
        out.pnl += f.closed_pnl - f.fee;  // realized_pnl_from_fill, for any Fill type
        out.fees += f.fee;
        if (before == 0) {
            out.complete = true;
            return out;
        }
        size = before;
    }
    return out;
}

}  // namespace pc::portfolio
