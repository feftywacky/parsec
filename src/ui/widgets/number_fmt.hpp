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

// Formats a Px at exactly `decimals` decimal places. For the chart's price axis, whose labels
// are spaced by a chosen step rather than by the venue's precision: printing an axis at full
// venue precision buries the digit that actually changes between two gridlines in a row of
// trailing zeros.
const char* format_px_decimals(Px value, int decimals, char* out, size_t cap) noexcept;

// Formats a Qty at exactly `sz_decimals` decimal places (docs/07 Phase 2: "sizes at
// szDecimals").
const char* format_qty(Qty value, uint8_t sz_decimals, char* out, size_t cap) noexcept;

// Formats a Usd notional as e.g. "$1,234.56" -- 2 decimal places, thousands separators, sign
// (if negative) before the '$' (docs/07 Phase 2: "USD with thousands separators").
const char* format_usd(Usd value, char* out, size_t cap) noexcept;

// Formats a raw 1e8-scaled fraction (e.g. AssetCtx::funding_rate_1e8, or a computed 24h-change
// fraction) as a percentage with `decimals` places and a trailing '%', e.g. 1e8-scaled 12500
// (== 0.000125 == 0.0125%) at decimals=4 -> "0.0125%".
// Same as format_usd, but keeps small amounts legible instead of rounding them into "$0.00".
// A taker fee on a $10 order is around half a cent, and two decimal places report that as
// nothing at all -- which reads as "no fee was charged" rather than "the fee is smaller than
// this column shows". Values under $1 print with 6 decimals, everything else with 2, so the
// column still lines up for the amounts that dominate it.
const char* format_usd_fine(Usd value, char* out, size_t cap) noexcept;

const char* format_pct(int64_t value_1e8, int decimals, char* out, size_t cap) noexcept;

// Formats a plain integer with thousands separators, e.g. -1234567 -> "-1,234,567". For counts
// rather than money: the chart ruler's tick count, bar counts, print counts.
const char* format_count(int64_t value, char* out, size_t cap) noexcept;

// Formats a duration as at most two components, largest first, dropping the second when it is
// zero: "6h 45m", "3d 4h", "45m", "12s". A span is read for its magnitude, and "6h 45m 12s"
// is three numbers to get one of them.
const char* format_duration(uint64_t ms, char* out, size_t cap) noexcept;

// Formats a unix-millisecond timestamp as "M/D/YYYY - HH:MM:SS" in the viewer's LOCAL time.
// Local, not UTC, unlike the chart's time axis: an axis is read against other axes and market
// sessions, while a history row is read against the trader's own memory of when they did the
// thing. Writes "--" for a zero timestamp, which is what an event that carried no venue clock
// gets. Not thread-safe assumptions: uses localtime_r, so it is reentrant.
const char* format_time_ms(uint64_t unix_ms, char* out, size_t cap) noexcept;

// The inverse of the above, for text fields: parses a plain decimal string like "118342.5"
// into a kScale-scaled integer. Returns false (leaving *out untouched) on anything that is not
// a plain non-negative decimal, so a typo is never silently accepted as zero -- which on a
// price or size field is the difference between a rejected order and a wrong one. Digits past
// the 8th decimal place are truncated, matching the fixed-point grid the value lands on.
[[nodiscard]] bool parse_fixed(const char* text, int64_t* out) noexcept;

}  // namespace pc::ui
