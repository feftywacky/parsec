#pragma once
#include <array>
#include <cstdint>

#include "core/units.hpp"
#include "parsec/parsec.h"

namespace pc::md {

using Level = pc_level;

// L2 order book for one asset -- DEPTH AND DISPLAY ONLY. apply() is a full-snapshot replace,
// not an incremental patch: the venue's L2 stream sends the whole visible depth on every
// update, so there is no diff/merge logic to maintain and no risk of the book drifting from a
// dropped delta.
//
// This type is deliberately NOT the execution price. `l2Book` default cadence coalesces
// updates over ~5s (measured 03 §W3); `md::Bbo`, fed by the separate `bbo` stream, updates
// ~32x more often and is what `exec::`/`risk::` must read (docs/02 §6.1). `best_bid()` /
// `best_ask()` below exist for the book panel and for `sweep()`'s depth walk, not for pricing
// an order -- if you are about to use them to decide what an order should cost, use
// `md::Bbo::execution_bid_px()` / `execution_ask_px()` instead.
class L2Book {
public:
    static constexpr uint8_t kMaxLevels = PC_MAX_LEVELS;

    struct Sweep {
        Px avg_px{};
        Qty filled{};
        Usd notional{};
        // Requested size still unfilled because the visible depth ran out before `filled`
        // reached the request -- the ticket needs this to show "book too thin, N left unfilled
        // at the modelled price" rather than silently understating the slippage estimate.
        Qty unfilled{};
    };

    void apply(const pc_l2& update, uint64_t exch_time_ms, uint64_t recv_time_ns) noexcept;
    [[nodiscard]] const std::array<Level, kMaxLevels>& bids() const noexcept { return bids_; }
    [[nodiscard]] const std::array<Level, kMaxLevels>& asks() const noexcept { return asks_; }
    [[nodiscard]] uint8_t bid_count() const noexcept { return n_bid_; }
    [[nodiscard]] uint8_t ask_count() const noexcept { return n_ask_; }
    [[nodiscard]] Px best_bid() const noexcept;
    [[nodiscard]] Px best_ask() const noexcept;
    [[nodiscard]] Px mid() const noexcept;
    [[nodiscard]] Px spread() const noexcept;
    [[nodiscard]] Qty depth_within(Px px, Side side) const noexcept;
    [[nodiscard]] Sweep sweep(Side side, Qty requested) const noexcept;
    [[nodiscard]] uint64_t exch_time_ms() const noexcept { return exch_time_ms_; }
    [[nodiscard]] uint64_t recv_time_ns() const noexcept { return recv_time_ns_; }

private:
    std::array<Level, kMaxLevels> bids_{};
    std::array<Level, kMaxLevels> asks_{};
    uint8_t n_bid_{};
    uint8_t n_ask_{};
    uint64_t exch_time_ms_{};
    uint64_t recv_time_ns_{};
};
static_assert(std::is_trivially_copyable_v<L2Book>);

}  // namespace pc::md
