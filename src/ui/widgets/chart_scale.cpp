#include "ui/widgets/chart_scale.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pc::ui {
namespace {

constexpr int64_t kPow10[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

constexpr uint64_t kMinuteMs = 60'000;
constexpr uint64_t kHourMs = 60 * kMinuteMs;
constexpr uint64_t kDayMs = 24 * kHourMs;

constexpr const char* kMonthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct CivilDate {
    int32_t year;
    uint32_t month;  // 1-12
    uint32_t day;    // 1-31
};

// Howard Hinnant's days->civil algorithm, verbatim in spirit. Used instead of gmtime_r because
// the axis re-derives dates for every visible bar every frame: this is branch-free integer
// work with no libc lock, no locale and no thread-safety caveat.
CivilDate civil_from_days(int64_t z) noexcept {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint64_t doe = static_cast<uint64_t>(z - era * 146097);
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp = (5 * doy + 2) / 153;
    const uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint64_t m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2 ? 1 : 0);
    return CivilDate{static_cast<int32_t>(y), static_cast<uint32_t>(m), static_cast<uint32_t>(d)};
}

int64_t day_index(uint64_t ms) noexcept {
    return static_cast<int64_t>(ms / kDayMs);
}

// Monday-start week number. Epoch day 0 is a Thursday, so +3 puts the boundary on Mondays.
int64_t week_index(int64_t day) noexcept {
    return (day + 3) / 7;
}

// Decimals needed to print `value` exactly on the 1e8 grid: 8 minus its trailing zeros.
int decimals_for(Px value) noexcept {
    if (value == 0)
        return 0;
    int64_t magnitude = value < 0 ? -value : value;
    int trailing = 0;
    while (trailing < 8 && magnitude % 10 == 0) {
        magnitude /= 10;
        ++trailing;
    }
    return 8 - trailing;
}

void push_price(PriceTicks& out, Px value, int max_decimals) noexcept {
    if (out.count >= PriceTicks::kMax)
        return;
    out.values[out.count++] = value;
    out.decimals = std::min(std::max(out.decimals, decimals_for(value)), max_decimals);
}

// Ticks by decade (1..9 x 10^k), thinned to the pixel budget. Only used when the visible range
// spans a decade or more -- below that a linear ladder is both denser and easier to read, and
// on a log axis under one decade the two are visually indistinguishable anyway.
PriceTicks log_decade_ticks(double lo, double hi, float height_px, float min_gap_px,
                            int max_decimals) noexcept {
    PriceTicks out{};
    const int first_exp = static_cast<int>(std::floor(std::log10(lo)));
    const int last_exp = static_cast<int>(std::floor(std::log10(hi)));
    double last_y = -1e30;
    for (int e = first_exp; e <= last_exp && out.count < PriceTicks::kMax; ++e) {
        const double decade = std::pow(10.0, e);
        for (int m = 1; m <= 9; ++m) {
            const double value = static_cast<double>(m) * decade;
            if (value < lo)
                continue;
            if (value > hi)
                break;
            const double y = price_frac(value, lo, hi, true) * static_cast<double>(height_px);
            if (out.count > 0 && y - last_y < static_cast<double>(min_gap_px))
                continue;
            last_y = y;
            push_price(out, static_cast<Px>(std::llround(value * static_cast<double>(kScale))),
                       max_decimals);
        }
    }
    return out;
}

// --- time ladder ---------------------------------------------------------------------------
// The steps a minor time tick is allowed to land on. Everything coarser than 12h is left to the
// day/month/year boundary passes, which carry their own calendar labels.
constexpr uint64_t kTimeLadder[] = {
    1'000,     2'000,         5'000,         10'000,         15'000,         30'000,
    kMinuteMs, 2 * kMinuteMs, 5 * kMinuteMs, 10 * kMinuteMs, 15 * kMinuteMs, 30 * kMinuteMs,
    kHourMs,   2 * kHourMs,   3 * kHourMs,   4 * kHourMs,    6 * kHourMs,    12 * kHourMs,
};
constexpr int kTimeLadderCount = static_cast<int>(sizeof(kTimeLadder) / sizeof(kTimeLadder[0]));

TimeTickWeight boundary_weight(uint64_t prev_ms, uint64_t ms) noexcept {
    const int64_t d0 = day_index(prev_ms);
    const int64_t d1 = day_index(ms);
    if (d0 == d1)
        return TimeTickWeight::Minor;
    const CivilDate c0 = civil_from_days(d0);
    const CivilDate c1 = civil_from_days(d1);
    if (c0.year != c1.year)
        return TimeTickWeight::Year;
    if (c0.month != c1.month)
        return TimeTickWeight::Month;
    if (week_index(d0) != week_index(d1))
        return TimeTickWeight::Week;
    return TimeTickWeight::Day;
}

void label_for(TimeTick& tick, uint64_t open_ms, uint64_t minor_step_ms) noexcept {
    const int64_t day = day_index(open_ms);
    const CivilDate date = civil_from_days(day);
    switch (tick.weight) {
        case TimeTickWeight::Year:
            std::snprintf(tick.label, sizeof(tick.label), "%d", date.year);
            break;
        case TimeTickWeight::Month:
            std::snprintf(tick.label, sizeof(tick.label), "%s", kMonthNames[date.month - 1]);
            break;
        case TimeTickWeight::Week:
        case TimeTickWeight::Day:
            std::snprintf(tick.label, sizeof(tick.label), "%u", date.day);
            break;
        case TimeTickWeight::Minor: {
            const uint64_t day_ms = open_ms % kDayMs;
            const unsigned h = static_cast<unsigned>(day_ms / kHourMs);
            const unsigned m = static_cast<unsigned>((day_ms % kHourMs) / kMinuteMs);
            if (minor_step_ms < kMinuteMs) {
                const unsigned s = static_cast<unsigned>((day_ms % kMinuteMs) / 1000);
                std::snprintf(tick.label, sizeof(tick.label), "%02u:%02u:%02u", h, m, s);
            } else {
                std::snprintf(tick.label, sizeof(tick.label), "%02u:%02u", h, m);
            }
            break;
        }
    }
}

bool too_close(const TimeTicks& placed, size_t index, size_t stride) noexcept {
    for (int i = 0; i < placed.count; ++i) {
        const size_t other = placed.ticks[i].index;
        const size_t gap = other > index ? other - index : index - other;
        if (gap < stride)
            return true;
    }
    return false;
}

}  // namespace

double price_frac(double price, double lo, double hi, bool log_scale) noexcept {
    if (log_scale && lo > 0.0 && hi > lo) {
        const double l0 = std::log10(lo);
        const double l1 = std::log10(hi);
        return (std::log10(std::max(price, 1e-12)) - l0) / (l1 - l0);
    }
    return hi > lo ? (price - lo) / (hi - lo) : 0.5;
}

double frac_price(double frac, double lo, double hi, bool log_scale) noexcept {
    if (log_scale && lo > 0.0 && hi > lo) {
        const double l0 = std::log10(lo);
        const double l1 = std::log10(hi);
        return std::pow(10.0, l0 + frac * (l1 - l0));
    }
    return lo + frac * (hi - lo);
}

PriceTicks price_ticks(double lo, double hi, float height_px, float min_gap_px, bool log_scale,
                       int max_decimals) noexcept {
    PriceTicks out{};
    if (!(hi > lo) || height_px <= 0.0F || min_gap_px <= 0.0F)
        return out;
    // Ticks are int64 on the 1e8 grid, so a range past ~9e9 cannot be represented there at
    // all. Nothing the venue lists comes close; this is here so a runaway axis produces no
    // gridlines rather than undefined behaviour in the cast below.
    if (!std::isfinite(lo) || !std::isfinite(hi) || hi > 9.0e9 || lo < -9.0e9)
        return out;
    max_decimals = std::clamp(max_decimals, 0, 8);

    if (log_scale && lo > 0.0 && hi / lo >= 10.0)
        return log_decade_ticks(lo, hi, height_px, min_gap_px, max_decimals);

    // The step is never finer than one unit at the venue's precision for this asset, so a label
    // can never quote a price the exchange would not accept.
    const int64_t min_step = kPow10[8 - max_decimals];
    const int max_ticks = std::max(1, static_cast<int>(height_px / min_gap_px));
    const double raw = (hi - lo) * static_cast<double>(kScale) / static_cast<double>(max_ticks);
    int64_t raw_i = raw > 9.0e17 ? static_cast<int64_t>(9.0e17) : static_cast<int64_t>(raw);
    raw_i = std::max(raw_i, min_step);

    int64_t p10 = min_step;  // largest power of ten <= raw_i, floored at min_step
    while (p10 <= raw_i / 10)
        p10 *= 10;

    int64_t step;
    if (raw_i <= p10)
        step = p10;
    else if (raw_i <= 2 * p10)
        step = 2 * p10;
    else if (p10 >= 10 * min_step && raw_i <= 5 * p10 / 2)
        step = 5 * p10 / 2;  // the 2.5 rung, only where it stays on the venue's grid
    else if (raw_i <= 5 * p10)
        step = 5 * p10;
    else
        step = 10 * p10;

    const int64_t lo_i = static_cast<int64_t>(std::floor(lo * static_cast<double>(kScale)));
    const int64_t hi_i = static_cast<int64_t>(std::ceil(hi * static_cast<double>(kScale)));
    // Ceil-divide towards +inf so the first tick is the first multiple of `step` at or above lo.
    int64_t first = (lo_i >= 0 ? (lo_i + step - 1) / step : lo_i / step) * step;
    for (int64_t v = first; v <= hi_i && out.count < PriceTicks::kMax; v += step)
        push_price(out, v, max_decimals);
    // A log axis under one decade uses the linear ladder but not its uniform spacing: thin
    // anything the transform has crowded together.
    if (log_scale && out.count > 2) {
        PriceTicks thinned{};
        double last_y = -1e30;
        for (int i = 0; i < out.count; ++i) {
            const double y =
                price_frac(static_cast<double>(out.values[i]) / static_cast<double>(kScale), lo, hi,
                           true) *
                static_cast<double>(height_px);
            if (thinned.count > 0 && y - last_y < static_cast<double>(min_gap_px))
                continue;
            last_y = y;
            push_price(thinned, out.values[i], max_decimals);
        }
        return thinned;
    }
    return out;
}

TimeTicks time_ticks(const pc_candle* candles, size_t first, size_t last, uint64_t interval_ms,
                     float bar_spacing, float min_gap_px) noexcept {
    TimeTicks out{};
    if (candles == nullptr || last < first || bar_spacing <= 0.0F || interval_ms == 0)
        return out;

    const size_t span = last - first + 1;
    // Also floored so the whole visible range cannot exhaust the tick budget on its left-hand
    // end: the tiers below fill left to right, and running out mid-axis reads as a chart whose
    // time labels simply stop.
    size_t stride = std::max<size_t>(1, static_cast<size_t>(std::ceil(min_gap_px / bar_spacing)));
    stride = std::max(stride, (span + TimeTicks::kMax - 1) / TimeTicks::kMax);

    // --- calendar boundaries, coarsest tier first so a month never loses its slot to the day
    // that happens to sit two bars away from it ---
    for (int tier = static_cast<int>(TimeTickWeight::Year);
         tier >= static_cast<int>(TimeTickWeight::Day); --tier) {
        for (size_t i = first; i <= last && out.count < TimeTicks::kMax; ++i) {
            const TimeTickWeight weight =
                i == 0 ? TimeTickWeight::Day
                       : boundary_weight(candles[i - 1].open_ms, candles[i].open_ms);
            if (static_cast<int>(weight) != tier)
                continue;
            if (too_close(out, i, stride))
                continue;
            TimeTick& tick = out.ticks[out.count++];
            tick.index = i;
            tick.weight = weight;
            label_for(tick, candles[i].open_ms, 0);
        }
    }

    // --- fill what is left from the time ladder ---
    uint64_t minor_step = 0;
    for (int i = 0; i < kTimeLadderCount; ++i) {
        if (kTimeLadder[i] < interval_ms)
            continue;
        const double bars = static_cast<double>(kTimeLadder[i]) / static_cast<double>(interval_ms);
        if (bars * static_cast<double>(bar_spacing) >= static_cast<double>(min_gap_px)) {
            minor_step = kTimeLadder[i];
            break;
        }
    }
    if (minor_step != 0) {
        for (size_t i = std::max(first, size_t{1}); i <= last && out.count < TimeTicks::kMax; ++i) {
            // The first bar of a new step bucket, which is where the round time actually falls
            // -- rounding each bar's own timestamp would put the label on whichever bar
            // happened to print, and a gap in the series would silently move it.
            if (candles[i].open_ms / minor_step == candles[i - 1].open_ms / minor_step)
                continue;
            if (too_close(out, i, stride))
                continue;
            TimeTick& tick = out.ticks[out.count++];
            tick.index = i;
            tick.weight = TimeTickWeight::Minor;
            label_for(tick, candles[i].open_ms, minor_step);
        }
    }

    // Placed out of order by tier; the renderer walks them left to right.
    for (int i = 1; i < out.count; ++i) {
        TimeTick key = out.ticks[i];
        int j = i - 1;
        while (j >= 0 && out.ticks[j].index > key.index) {
            out.ticks[j + 1] = out.ticks[j];
            --j;
        }
        out.ticks[j + 1] = key;
    }
    return out;
}

const char* format_bar_time(uint64_t open_ms, uint64_t interval_ms, char* out,
                            size_t cap) noexcept {
    if (cap == 0)
        return out;
    const CivilDate date = civil_from_days(day_index(open_ms));
    const uint64_t day_ms = open_ms % kDayMs;
    const unsigned h = static_cast<unsigned>(day_ms / kHourMs);
    const unsigned m = static_cast<unsigned>((day_ms % kHourMs) / kMinuteMs);
    const unsigned s = static_cast<unsigned>((day_ms % kMinuteMs) / 1000);
    const char* month = kMonthNames[date.month - 1];
    if (interval_ms >= kDayMs)
        std::snprintf(out, cap, "%u %s %d", date.day, month, date.year);
    else if (interval_ms >= kMinuteMs)
        std::snprintf(out, cap, "%u %s %02u:%02u", date.day, month, h, m);
    else
        std::snprintf(out, cap, "%u %s %02u:%02u:%02u", date.day, month, h, m, s);
    return out;
}

uint64_t bar_seconds_left(uint64_t open_ms, uint64_t interval_ms, uint64_t now_ms) noexcept {
    if (interval_ms == 0)
        return 0;
    const uint64_t close_ms = open_ms + interval_ms;
    if (now_ms >= close_ms)
        return 0;
    return (close_ms - now_ms + 999) / 1000;
}

}  // namespace pc::ui
