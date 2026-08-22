#include "ui/widgets/candle_renderer.hpp"

#include <implot.h>
#include <implot_internal.h>  // BeginItem/EndItem/FitThisFrame/FitPoint are internal API (docs/05 §2.1)

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "core/units.hpp"

namespace pc::ui {
namespace {

double to_sec(uint64_t ms) noexcept {
    return static_cast<double>(ms) / 1000.0;
}

// Draw-call-only double conversion (docs/02 §2 permits doubles in ImPlot draw calls and the
// final display conversion, never on the trading path). ImPlot's axes are inherently double.
double to_double(pc_px v) noexcept {
    return static_cast<double>(v) / static_cast<double>(pc::kScale);
}

// First index i in [0, count) with candles[i].open_ms >= target_ms, or count if none.
size_t lower_bound_ms(const pc_candle* candles, size_t count, uint64_t target_ms) noexcept {
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (candles[mid].open_ms < target_ms)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

}  // namespace

void plot_candlesticks(const char* label_id, const pc_candle* candles, size_t count,
                       uint64_t interval_ms, const ImVec4& bull_color, const ImVec4& bear_color) {
    if (count == 0 || candles == nullptr)
        return;

    const ImPlotRect lim = ImPlot::GetPlotLimits();
    // Pad the visible window by one interval on each side so a candle that is only partially
    // in view still gets drawn.
    const double lo_ms_f = lim.X.Min * 1000.0 - static_cast<double>(interval_ms);
    const double hi_ms_f = lim.X.Max * 1000.0 + static_cast<double>(interval_ms);
    const uint64_t lo_ms = lo_ms_f > 0.0 ? static_cast<uint64_t>(lo_ms_f) : 0;
    const uint64_t hi_ms = hi_ms_f > 0.0 ? static_cast<uint64_t>(hi_ms_f) : 0;

    // --- viewport clip: binary search on open_ms (ascending but not assumed uniform -- see
    // the header comment on why this trades the doc's O(1) index arithmetic for O(log n)) ---
    if (candles[0].open_ms > hi_ms)
        return;  // entire series is to the right of the visible window
    size_t i0 = lower_bound_ms(candles, count, lo_ms);
    if (i0 > 0)
        --i0;
    size_t i1 = lower_bound_ms(candles, count, hi_ms);
    i1 = i1 >= count ? count - 1 : i1;
    if (i0 > i1)
        return;

    // --- LOD: never draw more candles than we have pixels for (docs/05 §2.4) ---
    const float plot_w = ImPlot::GetPlotSize().x;
    const size_t visible = i1 - i0 + 1;
    const size_t stride =
        std::max<size_t>(1, static_cast<size_t>(std::ceil(static_cast<double>(visible) /
                                                          std::max(plot_w * 0.5F, 1.0F))));

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    double half_w = to_sec(interval_ms) * 0.5 * 0.7;  // 70% body width (docs/05 §2.4)

    // Cap the body at kMaxBodyPx on screen. Body width is defined in *time* units, so a
    // timeframe with only a handful of cached candles (a fresh daily series, say) stretches
    // each body across a huge slice of the frame and the chart reads as if it were zoomed
    // right in. Clamping in pixel space keeps candles candle-shaped at any data density,
    // while the time-domain width still governs whenever it is the smaller of the two.
    constexpr float kMaxBodyPx = 14.0F;
    const double x_span = lim.X.Max - lim.X.Min;
    if (x_span > 0.0) {
        const double px_per_sec = static_cast<double>(ImPlot::GetPlotSize().x) / x_span;
        const double body_px = half_w * 2.0 * px_per_sec;
        if (body_px > kMaxBodyPx)
            half_w *= kMaxBodyPx / body_px;
    }

    if (ImPlot::BeginItem(label_id)) {
        if (ImPlot::FitThisFrame()) {
            for (size_t i = i0; i <= i1; ++i) {
                ImPlot::FitPoint(ImPlotPoint(to_sec(candles[i].open_ms), to_double(candles[i].l)));
                ImPlot::FitPoint(ImPlotPoint(to_sec(candles[i].open_ms), to_double(candles[i].h)));
            }
        }

        for (size_t i = i0; i <= i1; i += stride) {
            const size_t j = std::min(i + stride, i1 + 1);  // bucket end, exclusive
            const double t = to_sec(candles[i].open_ms);
            const double o = to_double(candles[i].o);
            const double c = to_double(candles[j - 1].c);
            double lo = to_double(candles[i].l);
            double hi = to_double(candles[i].h);
            for (size_t k = i + 1; k < j; ++k) {  // aggregate the bucket's high/low range
                lo = std::min(lo, to_double(candles[k].l));
                hi = std::max(hi, to_double(candles[k].h));
            }

            const ImU32 col = ImGui::GetColorU32(o > c ? bear_color : bull_color);
            const ImVec2 wick_top = ImPlot::PlotToPixels(t, hi);
            const ImVec2 wick_bottom = ImPlot::PlotToPixels(t, lo);
            dl->AddLine(wick_top, wick_bottom, col);

            ImVec2 body_a = ImPlot::PlotToPixels(t - half_w, o);
            ImVec2 body_b =
                ImPlot::PlotToPixels(t + half_w * static_cast<double>(2 * stride - 1), c);
            if (std::abs(body_b.y - body_a.y) < 1.0F)  // doji stays visible as a hairline
                body_b.y = body_a.y + (body_b.y >= body_a.y ? 1.0F : -1.0F);
            dl->AddRectFilled(body_a, body_b, col);
        }
        ImPlot::EndItem();
    }
}

}  // namespace pc::ui
