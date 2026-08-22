#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "core/units.hpp"
#include "md/candle_series.hpp"
#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/candle_renderer.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// Interval id -> duration in ms, used both for the candle body width and for the volume
// bars' width. Indexed by PC_IV_* (parsec.h) -- same table as md::kIntervalMs, kept here so
// the panel does not reach into md:: for a display constant.
constexpr uint64_t kIntervalMs[PC_IV_COUNT] = {
    1'000,             // PC_IV_1S
    5'000,             // PC_IV_5S
    10'000,            // PC_IV_10S
    30'000,            // PC_IV_30S
    60'000,            // PC_IV_1M
    5 * 60'000,        // PC_IV_5M
    15 * 60'000,       // PC_IV_15M
    30 * 60'000,       // PC_IV_30M
    60 * 60'000,       // PC_IV_1H
    4 * 60 * 60'000,   // PC_IV_4H
    24 * 60 * 60'000,  // PC_IV_1D
};
constexpr const char* kIntervalLabels[PC_IV_COUNT] = {"1s", "5s", "10s", "30s", "1m",  "5m",
                                                      "15m", "30m", "1h", "4h", "D"};

// Backing storage for CandleSeries::copy_recent()'s output. Function-local static rather than
// a per-frame allocation (docs/05 §5.3: no allocation on the hot path) or a stack array (at
// kCapacity x ~48B this is too large -- ~240KB -- to put on the stack every frame). Purely a
// scratch buffer for the read-in-place copy, not panel business state.
pc_candle g_candle_buf[md::CandleSeries::kCapacity];
double g_volume_x[md::CandleSeries::kCapacity];
double g_volume_y[md::CandleSeries::kCapacity];

}  // namespace

void draw_chart(PanelContext& ctx) {
    // Toggle state that is genuinely just view chrome (not trading-relevant, not shared with
    // any other panel) lives as a function-local static rather than growing ViewState, which
    // is a contract owned by another agent (src/ui/panels.hpp).
    static bool log_scale = false;
    // Y follows the visible price range by default (what a trader expects); turning it off
    // frees the vertical axis so the chart can be dragged in all four directions. `reset_now`
    // is the one-shot "snap back" the Reset button raises.
    static bool auto_scale = true;
    static bool reset_now = false;

    if (ImGui::Begin(kWindowChart)) {
        // --- timeframe selector -- switching only changes which cached CandleSeries we read,
        // it never discards or refetches anything (docs/07 Phase 2 acceptance). ---
        for (uint8_t iv = 0; iv < PC_IV_COUNT; ++iv) {
            if (iv != 0)
                ImGui::SameLine();
            ImGui::PushID(static_cast<int>(iv));
            const bool selected = ctx.view.interval == iv;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(kIntervalLabels[iv])) {
                ctx.view.interval = iv;
                app::UiCommand command{};
                command.kind = app::UiCommandKind::SetInterval;
                command.asset = ctx.instrument.asset;
                command.interval = iv;
                (void)ctx.bridge.push_command(command);
            }
            if (selected)
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(12, 0));
        ImGui::SameLine();
        ImGui::Checkbox("log", &log_scale);
        ImGui::SameLine();
        // Off = free pan/zoom on both axes; on = price auto-follows the visible window.
        ImGui::Checkbox("auto", &auto_scale);
        ImGui::SameLine();
        if (ImGui::SmallButton("reset"))
            reset_now = true;

        if (ctx.market == nullptr || !ctx.instrument.valid()) {
            ImGui::TextDisabled("waiting for market data...");
            ImGui::End();
            return;
        }

        const md::CandleSeries& series = ctx.market->candles[ctx.view.interval];
        const size_t n = series.copy_recent(g_candle_buf, md::CandleSeries::kCapacity);
        if (n == 0) {
            // Sub-minute series are folded from live trades and have no history at all, so
            // "nothing yet" is the expected state right after connecting rather than a fault.
            if (ctx.view.interval < PC_IV_FIRST_VENUE)
                ImGui::TextDisabled(
                    "%s candles are built from live trades (the venue serves none below 1m) -- "
                    "waiting for the first print.",
                    kIntervalLabels[ctx.view.interval]);
            else
                ImGui::TextDisabled("no candles cached yet for this timeframe");
            ImGui::End();
            return;
        }

        const uint64_t interval_ms = kIntervalMs[ctx.view.interval];
        const float avail_h = ImGui::GetContentRegionAvail().y;

        ImPlot::GetStyle().UseLocalTime = false;  // exchange data is UTC (docs/05 §2.3)
        // Auto-fit lands the extreme high and low exactly on the frame edge, which clips wicks
        // against the border and leaves nowhere for an overlay label to sit. 8% of the fit
        // extent on each side of Y gives the same headroom the right-hand gutter gives X.
        ImPlot::GetStyle().FitPadding = ImVec2(0.0F, 0.08F);

        const double t_first = static_cast<double>(g_candle_buf[0].open_ms) / 1000.0;
        const double t_last = static_cast<double>(g_candle_buf[n - 1].open_ms) / 1000.0;

        // Price and volume share ONE plot rather than a linked subplot pair: volume rides a
        // hidden second Y axis whose range is inflated 4x so the bars occupy only the bottom
        // quarter of the frame, the way every exchange chart draws it. One plot means one
        // crosshair, one zoom gesture, and no gutter between the panes.
        if (ImPlot::BeginPlot("##candles", ImVec2(-1, avail_h), ImPlotFlags_Crosshairs)) {
            // X is NOT auto-fit: an auto-fit axis re-fits every frame, which silently undoes
            // the user's scroll-wheel zoom. It is seeded once (and re-seeded when the
            // timeframe or instrument changes) and then left under mouse control -- wheel to
            // zoom, drag to pan (docs/05 §2.3).
            static uint8_t seeded_interval = 0xFF;
            static uint32_t seeded_asset = PC_ASSET_NONE;
            const bool reseed = reset_now || seeded_interval != ctx.view.interval ||
                                seeded_asset != ctx.instrument.asset;
            seeded_interval = ctx.view.interval;
            seeded_asset = ctx.instrument.asset;

            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_None,
                              auto_scale ? (ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit)
                                         : ImPlotAxisFlags_None);
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxisScale(ImAxis_Y1, log_scale ? ImPlotScale_Log10 : ImPlotScale_Linear);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.2f");
            if (t_last > t_first) {
                // Seed to the most recent ~120 candles rather than the whole cached history,
                // so the default view is readable and zooming out is the deliberate gesture.
                const double span =
                    std::min(t_last - t_first, static_cast<double>(interval_ms) / 1000.0 * 120.0);
                // Leave a right-hand gutter (~8% of the window) past the newest candle, so
                // the live candle never sits flush against the price axis and new candles have
                // somewhere to print without the view jumping.
                const double gutter = span * 0.08;
                ImPlot::SetupAxisLimits(ImAxis_X1, t_last - span + gutter, t_last + gutter,
                                        reseed ? ImPlotCond_Always : ImPlotCond_Once);
                // Pan is bounded, but loosely: the original constraint pinned the view inside
                // [first, last] candle so a drag stopped dead at the edges; removing it
                // entirely let the chart be dragged arbitrarily far into empty space, where
                // there is nothing to draw and nothing to fit. Half the cached history of slack
                // on each side gives genuine four-direction freedom without a void to get lost
                // in -- the series only holds what was backfilled, so past the left edge there
                // is no data to show, not a rendering bug.
                const double history = std::max(t_last - t_first, 1.0);
                ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, t_first - history * 0.5,
                                                   t_last + history * 0.5);
                ImPlot::SetupAxisZoomConstraints(
                    ImAxis_X1, static_cast<double>(interval_ms) / 1000.0 * 4.0, history * 2.0);
            }

            if (!auto_scale && reseed) {
                double lo = 1e300;
                double hi = -1e300;
                for (size_t i = 0; i < n; ++i) {
                    lo = std::min(lo, static_cast<double>(g_candle_buf[i].l) /
                                          static_cast<double>(kScale));
                    hi = std::max(hi, static_cast<double>(g_candle_buf[i].h) /
                                          static_cast<double>(kScale));
                }
                const double pad = (hi - lo) * 0.05 + 1e-9;
                ImPlot::SetupAxisLimits(ImAxis_Y1, lo - pad, hi + pad, ImPlotCond_Always);
            }
            reset_now = false;

            // Hidden volume axis, rescaled every frame so the bars keep their bottom-quarter
            // footprint no matter how volume trends.
            double max_vol = 0.0;
            for (size_t i = 0; i < n; ++i) {
                g_volume_x[i] = static_cast<double>(g_candle_buf[i].open_ms) / 1000.0;
                g_volume_y[i] =
                    static_cast<double>(g_candle_buf[i].v) / static_cast<double>(kScale);
                max_vol = std::max(max_vol, g_volume_y[i]);
            }
            ImPlot::SetupAxis(ImAxis_Y2, nullptr, ImPlotAxisFlags_NoDecorations);
            ImPlot::SetupAxisLimits(ImAxis_Y2, 0.0, max_vol > 0.0 ? max_vol * 4.0 : 1.0,
                                    ImPlotCond_Always);

            // Volume bars are drawn by hand rather than via PlotBars: a bar's colour follows
            // its own candle's direction (close >= open -> bull), and PlotBars paints one
            // series in one colour. Same viewport clip the candles get -- only bars whose x is
            // inside the visible window are emitted.
            ImPlot::SetAxis(ImAxis_Y2);
            {
                const ImPlotRect lim = ImPlot::GetPlotLimits();
                double half_w = static_cast<double>(interval_ms) / 1000.0 * 0.35;
                // Same pixel cap the candle bodies use, so volume bars stay aligned in width
                // with the candles above them at every zoom level and data density.
                const double x_span = lim.X.Max - lim.X.Min;
                if (x_span > 0.0) {
                    const double px_per_sec = static_cast<double>(ImPlot::GetPlotSize().x) / x_span;
                    const double bar_px = half_w * 2.0 * px_per_sec;
                    if (bar_px > 14.0)
                        half_w *= 14.0 / bar_px;
                }
                ImDrawList* dl = ImPlot::GetPlotDrawList();
                ImVec4 bull = kColorBid;
                ImVec4 bear = kColorAsk;
                bull.w = 0.35F;
                bear.w = 0.35F;
                const ImU32 col_bull = ImGui::GetColorU32(bull);
                const ImU32 col_bear = ImGui::GetColorU32(bear);
                ImPlot::PushPlotClipRect();
                for (size_t i = 0; i < n; ++i) {
                    if (g_volume_x[i] < lim.X.Min - half_w || g_volume_x[i] > lim.X.Max + half_w)
                        continue;
                    const ImVec2 a = ImPlot::PlotToPixels(g_volume_x[i] - half_w, g_volume_y[i]);
                    const ImVec2 b = ImPlot::PlotToPixels(g_volume_x[i] + half_w, 0.0);
                    dl->AddRectFilled(a, b,
                                      g_candle_buf[i].c >= g_candle_buf[i].o ? col_bull
                                                                            : col_bear);
                }
                ImPlot::PopPlotClipRect();
            }

            ImPlot::SetAxis(ImAxis_Y1);
            plot_candlesticks("candles", g_candle_buf, n, interval_ms, kColorBid, kColorAsk);

            // --- Phase 6 overlay hooks (entry price, liquidation price, resting orders,
            // TP/SL, fill markers) attach here, between BeginPlot/EndPlot, via
            // ImPlot::GetPlotDrawList() so they share this plot's coordinate transform.
            // Not implemented in Phase 2 -- see docs/07 Phase 6. ---

            // Hand-drawn trading crosshair (ImPlotFlags_Crosshairs above only swaps the
            // mouse cursor -- docs/05 §2.3).
            if (ImPlot::IsPlotHovered()) {
                const ImPlotPoint m = ImPlot::GetPlotMousePos();
                const ImVec2 pos = ImPlot::GetPlotPos();
                const ImVec2 size = ImPlot::GetPlotSize();
                const ImVec2 px = ImPlot::PlotToPixels(m);
                ImDrawList* dl = ImPlot::GetPlotDrawList();
                ImPlot::PushPlotClipRect();
                dl->AddLine(ImVec2(pos.x, px.y), ImVec2(pos.x + size.x, px.y),
                            IM_COL32(150, 150, 150, 150));
                dl->AddLine(ImVec2(px.x, pos.y), ImVec2(px.x, pos.y + size.y),
                            IM_COL32(150, 150, 150, 150));
                ImPlot::PopPlotClipRect();

                char label[32];
                const Px price = static_cast<Px>(m.y * static_cast<double>(kScale));
                format_px(price, ctx.sz_decimals, label, sizeof(label));
                dl->AddRectFilled(ImVec2(pos.x + size.x - 84, px.y - 8),
                                  ImVec2(pos.x + size.x, px.y + 8), IM_COL32(40, 40, 40, 230));
                dl->AddText(ImVec2(pos.x + size.x - 80, px.y - 7), IM_COL32_WHITE, label);
            }

            ImPlot::EndPlot();
        }
    }
    ImGui::End();
}

}  // namespace pc::ui
