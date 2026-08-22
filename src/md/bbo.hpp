#pragma once
#include <cstdint>
#include <type_traits>

#include "core/units.hpp"
#include "parsec/parsec.h"

namespace pc::md {

// The execution-truth top of book (docs/02 §6.1). Hyperliquid's `bbo` feed updates ~32x more
// often than default `l2Book` (measured 03 §W3) at a fraction of the bytes, so it -- not
// `L2Book` -- is what `exec::` and `risk::` are required to read for pricing. `L2Book` is for
// depth/display math only; see its header comment. Accessor names are deliberately prefixed
// `execution_*` so a call site that reaches for a book's touch price instead of this type's
// reads as obviously wrong on sight, per the design doc's explicit warning that wiring the
// ticket to `L2Book::best_bid()` would quietly trade on a price up to five seconds stale.
class Bbo {
public:
    void apply(const pc_bbo& update, uint64_t exch_time_ms, uint64_t recv_time_ns) noexcept {
        value = update;
        exch_time_ms_ = exch_time_ms;
        recv_time_ns_ = recv_time_ns;
    }

    [[nodiscard]] bool has_execution_bid() const noexcept { return value.has_bid; }
    [[nodiscard]] bool has_execution_ask() const noexcept { return value.has_ask; }
    [[nodiscard]] Px execution_bid_px() const noexcept { return value.has_bid ? value.bid.px : 0; }
    [[nodiscard]] Px execution_ask_px() const noexcept { return value.has_ask ? value.ask.px : 0; }
    [[nodiscard]] Qty execution_bid_sz() const noexcept { return value.has_bid ? value.bid.sz : 0; }
    [[nodiscard]] Qty execution_ask_sz() const noexcept { return value.has_ask ? value.ask.sz : 0; }
    // Mid of the execution feed's touch -- what `exec::Rounder::marketable_px()` should offset
    // from, not `L2Book::mid()`.
    [[nodiscard]] Px execution_mid() const noexcept {
        return has_execution_bid() && has_execution_ask()
                   ? execution_bid_px() + (execution_ask_px() - execution_bid_px()) / 2
                   : 0;
    }
    [[nodiscard]] uint64_t exch_time_ms() const noexcept { return exch_time_ms_; }
    [[nodiscard]] uint64_t recv_time_ns() const noexcept { return recv_time_ns_; }

    // Public raw payload, kept accessible (not hidden behind an accessor) because it is itself
    // trivially copyable POD and callers occasionally need the whole `pc_bbo` (e.g. to hash it
    // for duplicate-payload staleness detection in md::AssetStaleness). Prefer the named
    // `execution_*` accessors above at any call site that is actually pricing something.
    pc_bbo value{};

private:
    uint64_t exch_time_ms_{};
    uint64_t recv_time_ns_{};
};
static_assert(std::is_trivially_copyable_v<Bbo>);

}  // namespace pc::md
