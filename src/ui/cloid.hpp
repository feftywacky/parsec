#pragma once
// Client order id generation for panels that submit orders.
//
// Every order needs a cloid (docs/02 §6.2), but the stateful session-id/sequence scheme that
// makes cloids collision-free and reconciliation-friendly lives in exec::OrderRouter, which is
// engine-owned state a panel must not instantiate a second copy of (that would violate the
// "panels hold no state" rule and desync from the engine's own bookkeeping). This generates a
// locally-unique-enough tag instead so an order is never sent with an all-zero cloid; the
// engine/OrderRouter remains the authority for cloid-based reconciliation.
//
// Shared between the ticket and the positions panel so the two cannot drift into different
// notions of what a UI-sourced cloid looks like.
#include <cstdint>
#include <cstring>
#include <random>

namespace pc::ui {

inline void make_local_cloid(uint8_t out[16], uint64_t now_ms) noexcept {
    static std::mt19937_64 rng{std::random_device{}()};
    const uint64_t r = rng();
    std::memcpy(out, &now_ms, 8);
    std::memcpy(out + 8, &r, 8);
}

}  // namespace pc::ui
