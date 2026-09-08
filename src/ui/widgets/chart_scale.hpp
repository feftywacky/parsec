#pragma once
// Axis arithmetic for the candle chart: where the price gridlines land, which bars get a time
// label, and the price<->fraction mapping the plot and the price axis share.
//
// Deliberately free of ImGui/ImPlot so it can be unit-tested on its own
// (tests/cpp/test_chart_scale.cpp) -- the tick maths is where an off-by-one shows up as a
// mislabelled gridline, which is the one chart bug a trader cannot see.
#include <cstddef>
#include <cstdint>

#include "core/units.hpp"
#include "parsec/parsec.h"

namespace pc::ui {

// --- price transform ---------------------------------------------------------------------

// Where `price` sits between `lo` (0.0) and `hi` (1.0), linearly or on a log10 axis. The one
// definition of the price mapping: pixel placement, tick thinning and the inverse below all go
// through it, so a label can never land on a different pixel from its own gridline.
[[nodiscard]] double price_frac(double price, double lo, double hi, bool log_scale) noexcept;
[[nodiscard]] double frac_price(double frac, double lo, double hi, bool log_scale) noexcept;

// --- price gridlines ---------------------------------------------------------------------

// Tick values are Px (1e8 fixed point), not doubles. An axis label is a price, and a price
// that round-trips through a double is wrong in exactly the low digits a trader is reading it
// for; keeping the ladder on the venue's own grid means every label is exact at `decimals`.
struct PriceTicks {
    static constexpr int kMax = 40;
    Px values[kMax]{};
    int count{};
    int decimals{};  // decimals the chosen step needs -- what every label prints at
};

// Nice-number gridlines (1, 2, 2.5, 5 x 10^k) inside [lo, hi], never closer together than
// `min_gap_px` on an axis `height_px` tall. `max_decimals` is the venue's price precision for
// the asset: the step is never finer than one unit at that precision, so no label can imply a
// price the exchange would reject.
[[nodiscard]] PriceTicks price_ticks(double lo, double hi, float height_px, float min_gap_px,
                                     bool log_scale, int max_decimals) noexcept;

// --- time axis -----------------------------------------------------------------------------

// The largest calendar boundary a bar opens on, relative to the bar before it. Ticks are placed
// largest-first, so a day boundary survives the thinning that drops the hours around it -- which
// is what makes the axis read as a calendar rather than as evenly spaced numbers.
enum class TimeTickWeight : uint8_t { Minor = 0, Day, Week, Month, Year };

struct TimeTick {
    size_t index{};  // bar index the tick sits on
    TimeTickWeight weight{};
    char label[12]{};
};

struct TimeTicks {
    static constexpr int kMax = 64;
    TimeTick ticks[kMax]{};
    int count{};
};

// Labels for the bars in [first, last], laid out so no two land closer than `min_gap_px` at the
// given `bar_spacing`. Day/month/year boundaries are placed first and win every collision; the
// space left over is filled from a time ladder (1s..12h) picked to fit `bar_spacing`.
//
// Ticks are chosen per *bar*, not per unit of time, so a gap in the series (a venue outage, a
// weekend, an unsubscribed stretch) collapses on the axis the way it does in the plot.
[[nodiscard]] TimeTicks time_ticks(const pc_candle* candles, size_t first, size_t last,
                                   uint64_t interval_ms, float bar_spacing,
                                   float min_gap_px) noexcept;

// The crosshair's time readout: "2026-09-07 14:30" at a precision that follows the timeframe
// rather than always printing seconds. UTC, like the axis (docs/05 §2.3).
const char* format_bar_time(uint64_t open_ms, uint64_t interval_ms, char* out, size_t cap) noexcept;

// Seconds left in the bar that opened at `open_ms`, or 0 once it has closed.
[[nodiscard]] uint64_t bar_seconds_left(uint64_t open_ms, uint64_t interval_ms,
                                        uint64_t now_ms) noexcept;

}  // namespace pc::ui
