#include "md/l2_book.hpp"

#include <algorithm>

namespace pc::md {

void L2Book::apply(const pc_l2& update, uint64_t exch_time_ms, uint64_t recv_time_ns) noexcept {
    n_bid_ = std::min<uint8_t>(update.n_bid, kMaxLevels);
    n_ask_ = std::min<uint8_t>(update.n_ask, kMaxLevels);
    bids_ = {};
    asks_ = {};
    std::copy_n(update.bids, n_bid_, bids_.begin());
    std::copy_n(update.asks, n_ask_, asks_.begin());
    exch_time_ms_ = exch_time_ms;
    recv_time_ns_ = recv_time_ns;
}

Px L2Book::best_bid() const noexcept {
    return n_bid_ ? bids_[0].px : 0;
}

Px L2Book::best_ask() const noexcept {
    return n_ask_ ? asks_[0].px : 0;
}

Px L2Book::mid() const noexcept {
    const auto bid = best_bid(), ask = best_ask();
    return bid && ask ? bid + (ask - bid) / 2 : 0;
}

Px L2Book::spread() const noexcept {
    const auto bid = best_bid(), ask = best_ask();
    return bid && ask && ask >= bid ? ask - bid : 0;
}

// Cumulative resting size priced at or better than px, on the side an order of `side` would
// cross into (a Buy sweeps asks, a Sell sweeps bids).
Qty L2Book::depth_within(Px px, Side side) const noexcept {
    Qty total{};
    const auto& levels = side == Side::Buy ? asks_ : bids_;
    const auto count = side == Side::Buy ? n_ask_ : n_bid_;
    for (uint8_t i = 0; i < count; ++i) {
        if ((side == Side::Buy && levels[i].px > px) || (side == Side::Sell && levels[i].px < px))
            break;
        total += levels[i].sz;
    }
    return total;
}

// Walks the book on the crossed side and reports what filling `requested` size would cost;
// `result.filled` is less than `requested` if the visible depth runs out.
L2Book::Sweep L2Book::sweep(Side side, Qty requested) const noexcept {
    Sweep result{};
    if (requested <= 0)
        return result;
    const auto& levels = side == Side::Buy ? asks_ : bids_;
    const auto count = side == Side::Buy ? n_ask_ : n_bid_;
    __int128 cost{};
    for (uint8_t i = 0; i < count && result.filled < requested; ++i) {
        const Qty take = std::min(requested - result.filled, levels[i].sz);
        if (take <= 0)
            continue;
        cost += static_cast<__int128>(levels[i].px) * take;
        result.filled += take;
    }
    if (result.filled) {
        result.notional = static_cast<Usd>(cost / kScale);
        result.avg_px = static_cast<Px>(cost / result.filled);
    }
    result.unfilled = requested - result.filled;
    return result;
}

}  // namespace pc::md
