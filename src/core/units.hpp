#pragma once
#include <cstdint>
#include <limits>

namespace pc {

// Fixed-point scale shared by every price/size/notional quantity (1e8, same as the venue's
// wire precision). A single global scale removes a whole class of unit-mixing bugs.
inline constexpr int64_t kScale = 100'000'000;

using Px = int64_t;   // price, scaled by kScale (BTC @ 118342.5 -> 11834250000000)
using Qty = int64_t;  // size, scaled by kScale (0.00123 BTC -> 123000)
using Usd = int64_t;  // notional, scaled by kScale

// notional = px * qty / kScale, computed in 128 bits to avoid overflow, saturating at the
// Usd range instead of wrapping.
inline Usd notional(Px px, Qty qty) noexcept {
    const __int128 value = static_cast<__int128>(px) * qty / kScale;
    if (value > std::numeric_limits<Usd>::max())
        return std::numeric_limits<Usd>::max();
    if (value < std::numeric_limits<Usd>::min())
        return std::numeric_limits<Usd>::min();
    return static_cast<Usd>(value);
}
inline Px abs_px(Px value) noexcept {
    return value < 0 ? -value : value;
}
enum class Side : uint8_t { Buy, Sell };
}  // namespace pc
