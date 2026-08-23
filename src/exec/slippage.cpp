#include "exec/slippage.hpp"

namespace pc::exec {

SlippageEstimate SlippageModel::estimate(const md::L2Book& book, Side side, Qty requested,
                                         Px reference) noexcept {
    SlippageEstimate est{};
    const auto sweep = book.sweep(side, requested);
    est.avg_px = sweep.avg_px;
    est.filled = sweep.filled;
    est.unfilled = sweep.unfilled;
    est.notional = sweep.notional;
    est.full_fill = sweep.filled >= requested;

    if (sweep.filled > 0 && reference > 0) {
        // Positive means the sweep costs more than the reference price would suggest: for a
        // buy that's avg > reference, for a sell it's avg < reference. Computed in 128 bits
        // since (avg_px - reference) * 10000 can exceed the Px range at high magnitudes.
        const __int128 diff = side == Side::Buy ? static_cast<__int128>(sweep.avg_px) - reference
                                                : static_cast<__int128>(reference) - sweep.avg_px;
        est.slippage_bps = static_cast<int32_t>(diff * 10'000 / reference);
    }
    return est;
}

LimitFillEstimate SlippageModel::estimate_limit(const md::L2Book& book, Side side, Qty requested,
                                                Px limit_px, Px reference) noexcept {
    LimitFillEstimate est{};
    if (requested <= 0 || limit_px <= 0)
        return est;

    // depth_within() counts exactly the levels a limit at `limit_px` is allowed to take, so
    // sweeping that much can never walk past the limit price -- the sweep consumes levels from
    // the touch outwards and stops once it has taken `immediate`.
    const Qty crossable = book.depth_within(limit_px, side);
    const Qty immediate = crossable < requested ? crossable : requested;
    est.resting = requested - (immediate > 0 ? immediate : 0);
    if (immediate <= 0)
        return est;

    const auto sweep = book.sweep(side, immediate);
    est.avg_px = sweep.avg_px;
    est.immediate = sweep.filled;
    est.resting = requested - sweep.filled;
    est.notional = sweep.notional;
    if (sweep.filled > 0 && reference > 0) {
        const __int128 diff = side == Side::Buy ? static_cast<__int128>(sweep.avg_px) - reference
                                                : static_cast<__int128>(reference) - sweep.avg_px;
        est.slippage_bps = static_cast<int32_t>(diff * 10'000 / reference);
    }
    return est;
}

}  // namespace pc::exec
