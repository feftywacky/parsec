#pragma once
#include <cstdint>

#include "core/units.hpp"
#include "md/l2_book.hpp"
namespace pc::exec {

// The modelled outcome of sweeping the visible book for a taker order, as shown in the ticket
// "Est / Max slippage" line before Buy/Sell becomes clickable (docs/02 SS6.3, SS7).
struct SlippageEstimate {
    Px avg_px{};     // volume-weighted average fill price the sweep would achieve
    Qty filled{};    // size the visible book can actually absorb (<= requested)
    Qty unfilled{};  // requested - filled; > 0 means the book can't fill it all right now
    Usd notional{};
    int32_t slippage_bps{};  // avg_px vs `reference`, positive = worse for the trader
    bool full_fill{};
};

// What a *limit* order would actually do against the visible book right now. A limit order is
// not a sweep: only the depth at or better than its own limit price is marketable, and whatever
// is left over rests at the limit rather than paying up for the next level. Modelling it as a
// sweep (which is what the ticket used to do) prints a fill price the order can never get --
// for a buy limit below the market it prints the ask, which is both wrong and the wrong
// direction.
struct LimitFillEstimate {
    Px avg_px{};      // VWAP of the marketable portion; 0 when the order does not cross at all
    Qty immediate{};  // size that crosses right now
    Qty resting{};    // requested - immediate: size that would sit on the book at `limit_px`
    Usd notional{};   // cost/proceeds of the marketable portion only
    int32_t slippage_bps{};  // avg_px vs `reference`, positive = worse for the trader
};

// Wraps md::L2Book::sweep() (owned by md/, read-only here) with the reference-price comparison
// the ticket needs. `reference` should be the `bbo` mid per docs/02 SS6.1 -- callers own
// picking that price; this model only turns a sweep into a slippage number.
class SlippageModel {
public:
    static SlippageEstimate estimate(const md::L2Book& book, Side side, Qty requested,
                                     Px reference) noexcept;

    // `limit_px` caps which levels are marketable. `reference` is only used for the slippage
    // figure and should be the same execution touch/mid the rest of the ticket prices against.
    static LimitFillEstimate estimate_limit(const md::L2Book& book, Side side, Qty requested,
                                            Px limit_px, Px reference) noexcept;
};

}  // namespace pc::exec
