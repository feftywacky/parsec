#include "exec/rounder.hpp"

#include <algorithm>
#include <cstdint>

namespace pc::exec {
namespace {

// Number of base-10 digits in the decimal representation of |x| (at least 1, e.g.
// digits_px(0) == 1). This is floor(log10(x)) + 1 for x > 0, computed without float/log10 so
// it stays exact for every Px value we can represent.
int digits_px(Px x) noexcept {
    x = abs_px(x);
    int n = 1;
    while (x >= 10) {
        x /= 10;
        ++n;
    }
    return n;
}

// 10^n as a Px. Clamped at 18 (10^18 fits comfortably in an int64_t, and no price this venue
// trades needs an exponent anywhere near that) so a pathological input can never overflow.
Px pow10(int n) noexcept {
    n = std::clamp(n, 0, 18);
    Px r = 1;
    while (n--)
        r *= 10;
    return r;
}

Px floor_to(Px x, Px step) noexcept {
    return x / step * step;
}

Px ceil_to(Px x, Px step) noexcept {
    return ((x + step - 1) / step) * step;
}

// Decimal-place constraint alone: at most (6 - szDecimals) decimal places, i.e. px must be a
// multiple of 10^(8 - allowed_decimals) in the 1e8-scaled Px domain.
Px decimal_step_for(AssetPrecision p) noexcept {
    const unsigned allowed_decimals = 6u - std::min<unsigned>(p.sz_decimals, 6);
    return pow10(8 - static_cast<int>(allowed_decimals));
}

// Significant-figure constraint alone, evaluated at the magnitude of `x`: at most 5
// significant figures means x must be a multiple of 10^(digits_px(x) - 5). A value with 5 or
// fewer digits already satisfies this trivially (sig_step == 1, i.e. no extra restriction
// beyond the Px grid itself).
Px sig_fig_step_for(Px x) noexcept {
    const int d = digits_px(x);
    return d > 5 ? pow10(d - 5) : 1;
}

}  // namespace

Qty Rounder::round_sz(Qty raw, AssetPrecision p) noexcept {
    const auto decimals = std::min<uint8_t>(p.sz_decimals, 8);
    const Px step = pow10(8 - decimals);
    return floor_to(raw, step);
}

// Independent of round_px: a price is valid iff it satisfies the decimal-place constraint,
// AND (it is an integer number of dollars OR it satisfies the significant-figure constraint).
// This is exactly docs/03-hyperliquid-api.md SS6's conjunction-plus-exemption, expressed
// directly as integer divisibility.
bool Rounder::is_valid_px(Px px, AssetPrecision p) noexcept {
    if (px <= 0)
        return false;
    if (px % decimal_step_for(p) != 0)
        return false;
    if (px % kScale == 0)
        return true;  // integer price: sig-fig rule exempted
    return px % sig_fig_step_for(px) == 0;
}

// Rounds `raw` towards `mode`'s direction (floor for a passive buy / aggressive sell, ceil for
// a passive sell / aggressive buy) to the coarsest grid that keeps the result *valid*.
//
// This mirrors the venue SDK's two-stage process ("round to 5 sig figs, then to N decimals")
// in spirit, but not literally: doing the stages in that fixed order can needlessly destroy
// the integer exemption (e.g. 123456 would get chopped to 123450 by a naive sig-fig-first
// pass, even though 123456 itself is already valid). Instead this rounds directly to whichever
// grid keeps the result valid, recomputing the significant-figure step from the *candidate's*
// own magnitude, since digit count can shift by one at power-of-ten boundaries (999 -> 1000)
// while rounding. The loop is bounded and converges in a handful of iterations because the
// step sequence is monotonically non-decreasing and Px's digit count is bounded.
Px Rounder::round_px(Px raw, Side side, AssetPrecision p, RoundMode mode) noexcept {
    if (raw <= 0)
        return 0;

    const bool round_up = (side == Side::Buy) == (mode == RoundMode::Aggressive);
    const Px decimal_step = decimal_step_for(p);

    Px candidate = round_up ? ceil_to(raw, decimal_step) : floor_to(raw, decimal_step);
    if (candidate <= 0)
        return round_up ? decimal_step : 0;

    for (int iter = 0; iter < 8; ++iter) {
        if (candidate % kScale == 0)
            break;  // integer price: always valid, regardless of significant figures
        const Px sig_step = sig_fig_step_for(candidate);
        if (candidate % sig_step == 0)
            break;  // already within 5 significant figures at this magnitude

        // Coarser of the two constraints; always a power of ten and therefore always a
        // multiple of decimal_step too, so this can never re-violate the decimal rule.
        const Px combined_step = std::max(decimal_step, sig_step);
        const Px next = round_up ? ceil_to(raw, combined_step) : floor_to(raw, combined_step);
        if (next == candidate)
            break;  // converged
        candidate = next;
    }
    return candidate;
}

Px Rounder::marketable_px(Px ref, Side side, uint32_t bps, AssetPrecision p) noexcept {
    const __int128 delta = static_cast<__int128>(ref) * bps / 10'000;
    const Px raw = side == Side::Buy ? ref + static_cast<Px>(delta) : ref - static_cast<Px>(delta);
    return round_px(raw, side, p, RoundMode::Aggressive);
}

}  // namespace pc::exec
