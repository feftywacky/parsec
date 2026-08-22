#pragma once
#include <cstddef>
#include <cstdint>

#include "core/units.hpp"

namespace pc::ui {

// Display formatting for pc::Px/Qty/Usd (all int64, scaled by pc::kScale = 1e8, docs/02 §2).
//
// Every function here formats by integer digit manipulation only -- no double anywhere, not
// even internally -- so a display value can never silently disagree with the fixed-point
// value it was rounded from. This matters more than it sounds: a `double` round-trip of an
// 1e8-scaled int64 is lossy exactly in the low decimal digits a trader is squinting at to spot
// a fat-fingered price (docs/07 Phase 2).
//
// All functions write a NUL-terminated string into `out` (truncating safely, never
// overflowing, if `cap` is too small) and return `out` so a call can be inlined into an
// `ImGui::Text("%s", ...)` argument list without an intermediate variable.

// Formats a Px at the venue's per-asset price precision: decimal places = 6 - min(sz_decimals,
// 6), the same decimal-place component exec::Rounder applies at order entry (docs/03 §6), so
// a displayed price never implies more precision than the venue will actually accept. (The
// significant-figure half of the venue's rule is a *validity* constraint on prices that get
// signed, not a display-width choice -- exec::Rounder owns that; this just picks how many
// decimals to show.)
const char* format_px(Px value, uint8_t sz_decimals, char* out, size_t cap) noexcept;

// Formats a Qty at exactly `sz_decimals` decimal places (docs/07 Phase 2: "sizes at
// szDecimals").
const char* format_qty(Qty value, uint8_t sz_decimals, char* out, size_t cap) noexcept;

// Formats a Usd notional as e.g. "$1,234.56" -- 2 decimal places, thousands separators, sign
// (if negative) before the '$' (docs/07 Phase 2: "USD with thousands separators").
const char* format_usd(Usd value, char* out, size_t cap) noexcept;

// Formats a raw 1e8-scaled fraction (e.g. AssetCtx::funding_rate_1e8, or a computed 24h-change
// fraction) as a percentage with `decimals` places and a trailing '%', e.g. 1e8-scaled 12500
// (== 0.000125 == 0.0125%) at decimals=4 -> "0.0125%".
const char* format_pct(int64_t value_1e8, int decimals, char* out, size_t cap) noexcept;

}  // namespace pc::ui
