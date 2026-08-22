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

// Wraps md::L2Book::sweep() (owned by md/, read-only here) with the reference-price comparison
// the ticket needs. `reference` should be the `bbo` mid per docs/02 SS6.1 -- callers own
// picking that price; this model only turns a sweep into a slippage number.
class SlippageModel {
public:
    static SlippageEstimate estimate(const md::L2Book& book, Side side, Qty requested,
                                     Px reference) noexcept;
};

}  // namespace pc::exec
