#pragma once
#include <cstdint>

#include "core/units.hpp"
namespace pc::exec {

// Passive rounds towards being less aggressive (buy down, sell up); Aggressive rounds towards
// guaranteeing marketability (buy up, sell down). See Rounder::round_px.
enum class RoundMode : uint8_t { Passive, Aggressive };

struct AssetPrecision {
    uint8_t sz_decimals{};
};

// Applies the venue's per-asset size/price precision and significant-figure rules at order
// entry. Market data itself is stored at full resolution; only orders go through this.
//
// The price rule (docs/03-hyperliquid-api.md SS6) is a conjunction of two constraints that
// must BOTH hold, plus one exemption:
//   1. at most 5 significant figures, AND
//   2. at most (6 - szDecimals) decimal places (MAX_DECIMALS = 6 for perps),
//   3. EXCEPT an integer price is always allowed regardless of significant figures.
//
// Everything here is done in the Px integer domain (price * 1e8, see core/units.hpp) -- no
// float, no double, no decimal-string round-tripping. See rounder.cpp for the derivation.
class Rounder {
public:
    static Qty round_sz(Qty raw, AssetPrecision precision) noexcept;
    static Px round_px(Px raw, Side side, AssetPrecision precision,
                       RoundMode mode = RoundMode::Passive) noexcept;
    static bool check_min_notional(Px px, Qty qty) noexcept {
        return notional(px, qty) >= 10 * kScale;
    }
    // Independent check of the price rule above, usable on any candidate price -- including
    // ones that never went through round_px -- so tests can property-check round_px's output
    // against a definition that does not share its rounding code path.
    static bool is_valid_px(Px px, AssetPrecision precision) noexcept;

    // IOC "market order" price: `reference` moved `slippage_bps` against the trader (up for a
    // buy, down for a sell) and rounded Aggressive so the order is guaranteed marketable at
    // that price. Default 500 bps (5%), matching the venue's own SDKs (docs/03).
    static Px marketable_px(Px reference, Side side, uint32_t slippage_bps,
                            AssetPrecision precision) noexcept;
    static Px marketable_px(Px reference, Side side, AssetPrecision precision) noexcept {
        return marketable_px(reference, side, kDefaultSlippageBps, precision);
    }

    static constexpr uint32_t kDefaultSlippageBps = 500;
};

}  // namespace pc::exec
