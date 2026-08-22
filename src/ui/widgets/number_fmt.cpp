#include "ui/widgets/number_fmt.hpp"

#include <algorithm>
#include <cstdint>

namespace pc::ui {
namespace {

constexpr uint64_t kPow10[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

// Writes the base-10 digits of `v` into `out` (which must have room for at least 20 bytes),
// most-significant digit first, and returns the digit count. `v == 0` writes a single '0'.
int write_digits(uint64_t v, char* out) noexcept {
    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    char tmp[20];
    int n = 0;
    while (v > 0) {
        tmp[n++] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    for (int i = 0; i < n; ++i)
        out[i] = tmp[n - 1 - i];
    return n;
}

// Writes `v`'s digits into `out` with ',' inserted every three digits from the right (e.g.
// 1234567 -> "1,234,567"). Returns the number of characters written.
int write_grouped(uint64_t v, char* out) noexcept {
    char digits[20];
    const int n = write_digits(v, digits);
    int pos = 0;
    for (int i = 0; i < n; ++i) {
        if (i > 0 && (n - i) % 3 == 0)
            out[pos++] = ',';
        out[pos++] = digits[i];
    }
    return pos;
}

// Copies `len` bytes of `buf` into `out`, truncating to fit `cap` (including the NUL) rather
// than overflowing it. Always NUL-terminates unless cap == 0.
void safe_terminate(char* out, size_t cap, size_t len) noexcept {
    if (cap == 0)
        return;
    const size_t n = std::min(len, cap - 1);
    out[n] = '\0';
    (void)n;
}

// The shared formatter behind format_px/format_qty/format_usd: splits `value` (int64, scaled
// by pc::kScale) into a sign, an integer part, and a `decimals`-digit fractional part rounded
// to nearest (ties away from zero), all via integer arithmetic -- never via double. Optionally
// groups the integer part with thousands separators and/or prefixes '$'.
const char* format_scaled(int64_t value, int decimals, bool grouped, bool dollar_sign, char* out,
                          size_t cap) noexcept {
    decimals = std::clamp(decimals, 0, 8);

    const bool negative = value < 0;
    // Avoid UB on INT64_MIN: negate via `-(value + 1)` (always representable) then add 1 in
    // the unsigned domain, rather than negating `value` directly.
    uint64_t magnitude =
        negative ? static_cast<uint64_t>(-(value + 1)) + 1 : static_cast<uint64_t>(value);

    uint64_t int_part = magnitude / static_cast<uint64_t>(kScale);
    uint64_t frac_part = magnitude % static_cast<uint64_t>(kScale);

    // Round the 8-digit fractional part down to `decimals` digits, nearest with ties away from
    // zero, carrying into int_part if rounding pushes it up to 10^decimals.
    const uint64_t divisor = kPow10[8 - decimals];
    uint64_t rounded_frac = (frac_part + divisor / 2) / divisor;
    const uint64_t frac_mod = kPow10[decimals];
    if (rounded_frac >= frac_mod) {
        rounded_frac -= frac_mod;
        int_part += 1;
    }

    // Longest possible: '-' + '$' + 20 int digits + up to 6 group commas + '.' + 8 decimals.
    char buf[48];
    size_t pos = 0;
    if (negative)
        buf[pos++] = '-';
    if (dollar_sign)
        buf[pos++] = '$';
    pos += static_cast<size_t>(grouped ? write_grouped(int_part, buf + pos)
                                       : write_digits(int_part, buf + pos));
    if (decimals > 0) {
        buf[pos++] = '.';
        char frac_digits[9];
        const int fn = write_digits(rounded_frac, frac_digits);
        for (int i = 0; i < decimals - fn; ++i)  // left-zero-pad to `decimals` width
            buf[pos++] = '0';
        for (int i = 0; i < fn; ++i)
            buf[pos++] = frac_digits[i];
    }

    const size_t n = std::min(pos, cap == 0 ? size_t{0} : cap - 1);
    for (size_t i = 0; i < n; ++i)
        out[i] = buf[i];
    safe_terminate(out, cap, pos);
    return out;
}

}  // namespace

const char* format_px(Px value, uint8_t sz_decimals, char* out, size_t cap) noexcept {
    const int decimals = 6 - static_cast<int>(std::min<uint8_t>(sz_decimals, 6));
    return format_scaled(value, decimals, /*grouped=*/false, /*dollar_sign=*/false, out, cap);
}

const char* format_qty(Qty value, uint8_t sz_decimals, char* out, size_t cap) noexcept {
    const int decimals = static_cast<int>(std::min<uint8_t>(sz_decimals, 8));
    return format_scaled(value, decimals, /*grouped=*/false, /*dollar_sign=*/false, out, cap);
}

const char* format_usd(Usd value, char* out, size_t cap) noexcept {
    return format_scaled(value, /*decimals=*/2, /*grouped=*/true, /*dollar_sign=*/true, out, cap);
}

const char* format_pct(int64_t value_1e8, int decimals, char* out, size_t cap) noexcept {
    // value_1e8 is a fraction scaled by kScale (e.g. 0.0125% == 0.000125 -> 12500 at 1e8); the
    // percentage itself is that fraction x 100, still on the same 1e8 grid, computed in 128
    // bits so a large 24h-change input can't silently overflow the multiply.
    const __int128 wide = static_cast<__int128>(value_1e8) * 100;
    constexpr __int128 kMax = static_cast<__int128>(INT64_MAX);
    constexpr __int128 kMin = static_cast<__int128>(INT64_MIN);
    const int64_t pct_1e8 = static_cast<int64_t>(std::clamp(wide, kMin, kMax));

    // Reserve the trailing '%' by formatting into a scratch buffer first.
    char scratch[40];
    format_scaled(pct_1e8, decimals, /*grouped=*/false, /*dollar_sign=*/false, scratch,
                  sizeof(scratch));
    size_t len = 0;
    while (scratch[len] != '\0')
        ++len;
    if (cap == 0)
        return out;
    const size_t copy_n = std::min(len, cap - 1);
    for (size_t i = 0; i < copy_n; ++i)
        out[i] = scratch[i];
    if (copy_n < cap - 1) {
        out[copy_n] = '%';
        out[copy_n + 1] = '\0';
    } else {
        out[copy_n] = '\0';
    }
    return out;
}

}  // namespace pc::ui
