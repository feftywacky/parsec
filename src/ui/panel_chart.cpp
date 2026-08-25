#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "core/units.hpp"
#include "md/candle_series.hpp"
#include "ui/app_window.hpp"
#include "ui/asset_lookup.hpp"
#include "ui/panels.hpp"
#include "ui/position_levels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"
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

// ImDrawList has no dash pattern, so a dotted line is walked out by hand. Kept to whole-pixel
// steps along one axis (the crosshair is always axis-aligned) so the dashes land on the same
// pixel grid the gridlines do and never shimmer as the mouse moves.
void add_dotted_line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness = 1.0F,
                     float dash = 3.0F, float gap = 4.0F) noexcept {
    const bool horizontal = a.y == b.y;
    const float start = horizontal ? a.x : a.y;
    const float end = horizontal ? b.x : b.y;
    if (end <= start)
        return;
    for (float t = start; t < end; t += dash + gap) {
        const float stop = std::min(t + dash, end);
        if (horizontal)
            dl->AddLine(ImVec2(t, a.y), ImVec2(stop, a.y), col, thickness);
        else
            dl->AddLine(ImVec2(a.x, t), ImVec2(a.x, stop), col, thickness);
    }
}

// A labelled horizontal level: dotted line across the plot, price tag in the right-hand
// gutter. Shared by the live-price line and every order overlay so they cannot drift apart in
// weight, dash pattern or tag geometry. Returns the tag's vertical centre in pixels.
struct LevelTag {
    float y{};
    float left{};
    float half_height{};
};

LevelTag draw_level(ImDrawList* dl, ImVec2 plot_pos, ImVec2 plot_size, double price,
                    const char* label, ImU32 line_col, ImU32 tag_col, ImU32 text_col,
                    float thickness) noexcept {
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const float y = ImPlot::PlotToPixels(0.0, price).y;
    const float tag_left = plot_pos.x + plot_size.x - (text_size.x + 10.0F);

    ImPlot::PushPlotClipRect();
    add_dotted_line(dl, ImVec2(plot_pos.x, y), ImVec2(tag_left - 6.0F, y), line_col, thickness);
    ImPlot::PopPlotClipRect();

    const float half_h = text_size.y * 0.5F + 3.0F;
    dl->AddRectFilled(ImVec2(tag_left, y - half_h),
                      ImVec2(plot_pos.x + plot_size.x, y + half_h), tag_col);
    dl->AddText(ImVec2(tag_left + 5.0F, y - text_size.y * 0.5F), text_col, label);
    return LevelTag{y, tag_left, half_h};
}

}  // namespace

// How many candles the default view frames. Both axes seed against this: X spans this many
// bars back from the newest, Y fits the highs/lows within them.
constexpr size_t kSeedCandles = 120;

void draw_chart(PanelContext& ctx) {
    // The order overlays below read this session's resting orders; see ui_event_store.hpp --
    // draining from any panel is safe and idempotent within a frame.
    event_store().drain(ctx.bridge);

    // Toggle state that is genuinely just view chrome (not trading-relevant, not shared with
    // any other panel) lives as a function-local static rather than growing ViewState, which
    // is a contract owned by another agent (src/ui/panels.hpp).
    static bool log_scale = false;
    // On by default: a chart that opens already framing the price action is what a terminal
    // is expected to do, and the first thing a trader would otherwise have to click. The cost
    // is that an auto-fitting Y re-fits every frame, so it silently undoes a vertical drag --
    // which is why unticking "auto" hands the axis back and leaves it free. "reset" is the
    // one-shot "snap back to the data" either mode can ask for.
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
            if (ImGui::Button(kIntervalLabels[iv])) {
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
        if (ImGui::Checkbox("auto", &auto_scale) && !auto_scale)
            reset_now = true;  // handing the axis back: start from the data, not wherever
                               // the auto-fit happened to leave it
        ImGui::SameLine();
        if (ImGui::Button("reset"))
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
        // One grid, one weight, one colour. ImPlot ships a two-tier grid -- major lines at
        // MajorGridSize, minor lines at MinorGridSize dimmed by MinorAlpha (0.25 by default) --
        // which on a dark trading chart reads as a stray heavy line rather than as hierarchy.
        // Matching the sizes and lifting MinorAlpha to 1 collapses the two tiers into a single
        // uniform grid; the colour is set once here so it cannot drift from the theme.
        ImPlotStyle& plot_style = ImPlot::GetStyle();
        plot_style.MajorGridSize = ImVec2(1.0F, 1.0F);
        plot_style.MinorGridSize = ImVec2(1.0F, 1.0F);
        plot_style.MinorAlpha = 1.0F;
        plot_style.Colors[ImPlotCol_AxisGrid] = ImVec4(0.145F, 0.212F, 0.235F, 1.0F);
        // The plot paints its own frame/canvas by default; point both at the one ground so the
        // chart does not sit in a slightly different shade from the panel around it.
        plot_style.Colors[ImPlotCol_FrameBg] = kColorBg;
        plot_style.Colors[ImPlotCol_PlotBg] = kColorBg;
        plot_style.Colors[ImPlotCol_PlotBorder] = ImVec4(0.145F, 0.212F, 0.235F, 1.0F);
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
            static size_t seeded_n = 0;
            // The first frame that reaches this point is drawn from whatever the live feed has
            // folded so far -- often three or four candles -- because the REST backfill is
            // still in flight. Seeding X once against that history and never revisiting it
            // left the chart pinned to a several-minute window for the rest of the session,
            // even after 5000 candles landed a moment later: it read as a chart zoomed to
            // nothing. So keep re-seeding while the series is still shorter than the window we
            // actually want. Once it reaches kSeedCandles the view is final and the user's own
            // pan/zoom is never overridden again.
            const bool history_still_filling = seeded_n < kSeedCandles && n > seeded_n;
            const bool reseed = reset_now || history_still_filling ||
                                seeded_interval != ctx.view.interval ||
                                seeded_asset != ctx.instrument.asset;
            seeded_interval = ctx.view.interval;
            seeded_asset = ctx.instrument.asset;
            seeded_n = n;

            ImPlot::SetupAxes(nullptr, nullptr, ImPlotAxisFlags_None,
                              auto_scale ? (ImPlotAxisFlags_AutoFit | ImPlotAxisFlags_RangeFit)
                                         : ImPlotAxisFlags_None);
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Time);
            ImPlot::SetupAxisScale(ImAxis_Y1, log_scale ? ImPlotScale_Log10 : ImPlotScale_Linear);
            ImPlot::SetupAxisFormat(ImAxis_Y1, "$%.2f");
            {
                // Seed to the most recent ~120 candles rather than the whole cached history,
                // so the default view is readable and zooming out is the deliberate gesture.
                //
                // The lower clamp is not cosmetic. A series holding a single candle has
                // t_last == t_first, and the guard that used to wrap this block skipped seeding
                // entirely in that case -- but ImPlotCond_Once no-ops once the plot exists, so
                // the axis then kept ImPlot's default 0..1 range for the rest of the session.
                // That renders as a blank chart with a $0.00-$1.00 price axis (Y is RangeFit,
                // and no candle falls inside a bogus X window, so it has nothing to fit).
                const double interval_s = static_cast<double>(interval_ms) / 1000.0;
                const double span = std::max(
                    std::min(t_last - t_first, interval_s * static_cast<double>(kSeedCandles)),
                    interval_s * 8.0);
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

            // Seeding the free axis: without this the very first frame keeps ImPlot's default
            // 0..1 range, which renders as a blank chart with a $0.00-$1.00 price axis.
            static bool y_seeded = false;
            if (!auto_scale && (reseed || !y_seeded)) {
                y_seeded = true;
                // Over the candles the seeded X window actually shows, not the whole cached
                // history -- fitting to history the view is not on leaves the visible candles
                // squashed into a sliver of the frame.
                double lo = 1e300;
                double hi = -1e300;
                const size_t visible = std::min<size_t>(n, kSeedCandles);
                for (size_t i = n - visible; i < n; ++i) {
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

            // --- Level overlays: current price, then every resting order on this asset ---
            //
            // All of them share draw_level(), so a stop and the live price cannot end up drawn
            // with different weights or tag geometry. Drawn after the candles and before the
            // crosshair, which stays on top of everything.
            const ImVec2 plot_pos = ImPlot::GetPlotPos();
            const ImVec2 plot_size = ImPlot::GetPlotSize();
            ImDrawList* overlay_dl = ImPlot::GetPlotDrawList();

            const Px live_px = ctx.instrument.ctx.mark_px() > 0 ? ctx.instrument.ctx.mark_px()
                                                                : g_candle_buf[n - 1].c;
            bool price_tag_drawn = false;
            LevelTag price_tag{};
            if (live_px > 0) {
                // The live mark (activeAssetCtx, ~1 msg/s) rather than the newest candle's
                // close: the candle only moves when a trade prints, so on a quiet minute its
                // close is visibly behind the price the ticket is about to trade at.
                char label[32];
                format_px(live_px, ctx.sz_decimals, label, sizeof(label));
                const bool up = g_candle_buf[n - 1].c >= g_candle_buf[n - 1].o;
                ImVec4 tint = up ? kColorBid : kColorAsk;
                const ImU32 tag_col = ImGui::GetColorU32(tint);
                tint.w = 0.55F;
                price_tag = draw_level(overlay_dl, plot_pos, plot_size,
                                       static_cast<double>(live_px) /
                                           static_cast<double>(kScale),
                                       label, ImGui::GetColorU32(tint), tag_col,
                                       IM_COL32(10, 10, 10, 255), 1.0F);
                price_tag_drawn = true;
            }

            // The open position on this asset: entry, then liquidation. Entry is where the
            // P&L on screen is measured from and liquidation is where the position stops
            // existing, so both belong on the price axis rather than only in a table three
            // panels away. Both come from ui/position_levels.hpp, the same source the
            // positions table reads, so the line and the table can never disagree.
            if (const portfolio::Position* pos = find_position(ctx.portfolio,
                                                               ctx.instrument.asset)) {
                const pc_position& p = pos->value;
                const uint32_t max_leverage =
                    lookup_asset(ctx.universe, pos->asset, ctx.sz_decimals).max_leverage;

                char px_buf[32], label[64];
                if (p.entry_px > 0) {
                    format_px(p.entry_px, ctx.sz_decimals, px_buf, sizeof(px_buf));
                    std::snprintf(label, sizeof(label), "ENTRY %s", px_buf);
                    // Neutral, not long/short coloured: entry is a reference line, and giving
                    // it the bid/ask palette would make it compete with the P&L colouring
                    // everything else on this chart uses to mean better/worse.
                    ImVec4 tint = kColorTextPrimary;
                    const ImU32 tag_col = ImGui::GetColorU32(tint);
                    tint.w = 0.5F;
                    const LevelTag tag = draw_level(
                        overlay_dl, plot_pos, plot_size,
                        static_cast<double>(p.entry_px) / static_cast<double>(kScale), label,
                        ImGui::GetColorU32(tint), tag_col, IM_COL32(10, 10, 10, 255), 1.0F);
                    if (price_tag_drawn &&
                        std::abs(tag.y - price_tag.y) < tag.half_height * 2.0F)
                        price_tag_drawn = false;
                }

                bool liq_is_estimate = false;
                const Px liq = liquidation_px(p, ctx.portfolio, max_leverage, &liq_is_estimate);
                if (liq > 0) {
                    format_px(liq, ctx.sz_decimals, px_buf, sizeof(px_buf));
                    // Labelled "LIQ EST" when it is this client's own estimate rather than the
                    // venue's published price -- the same distinction the positions table
                    // draws, and not one to hide on a line about being liquidated.
                    std::snprintf(label, sizeof(label), "%s %s",
                                  liq_is_estimate ? "LIQ EST" : "LIQ", px_buf);
                    ImVec4 tint = kColorAsk;
                    const ImU32 tag_col = ImGui::GetColorU32(tint);
                    tint.w = 0.6F;
                    // Drawn heavier than the other levels on purpose: it is the only line here
                    // that marks the position ceasing to exist.
                    const LevelTag tag = draw_level(
                        overlay_dl, plot_pos, plot_size,
                        static_cast<double>(liq) / static_cast<double>(kScale), label,
                        ImGui::GetColorU32(tint), tag_col, IM_COL32(10, 10, 10, 255), 2.0F);
                    if (price_tag_drawn &&
                        std::abs(tag.y - price_tag.y) < tag.half_height * 2.0F)
                        price_tag_drawn = false;
                }
            }

            // Resting orders for THIS asset only -- open_order_at() spans every coin, and a
            // stop on another market drawn against these candles is worse than not drawing it.
            for (size_t i = 0; i < event_store().open_order_count(); ++i) {
                const OrderRow& order = event_store().open_order_at(i);
                if (order.asset != ctx.instrument.asset)
                    continue;
                // A trigger order rests at its trigger; `px` is the limit it converts to once
                // it fires, which for a market trigger is ~5% away by design. Drawing `px`
                // there would put the stop line nowhere near the stop.
                const Px level = order.is_trigger ? order.trigger_px : order.px;
                if (level <= 0)
                    continue;

                char px_buf[32], label[64];
                format_px(level, ctx.sz_decimals, px_buf, sizeof(px_buf));
                const char* kind = order.tpsl == PC_TPSL_TP   ? "TP"
                                   : order.tpsl == PC_TPSL_SL ? "SL"
                                   : order.is_buy             ? "BUY"
                                                              : "SELL";
                std::snprintf(label, sizeof(label), "%s %s", kind, px_buf);

                // Take-profits read as the winning side and stops as the losing one whichever
                // way the position points, so they are coloured by leg rather than by side;
                // a plain limit keeps the bid/ask colour of the side it sits on.
                ImVec4 tint = order.tpsl == PC_TPSL_TP   ? kColorBid
                              : order.tpsl == PC_TPSL_SL ? kColorWarning
                              : order.is_buy             ? kColorBid
                                                         : kColorAsk;
                const ImU32 tag_col = ImGui::GetColorU32(tint);
                tint.w = 0.5F;
                const LevelTag tag =
                    draw_level(overlay_dl, plot_pos, plot_size,
                               static_cast<double>(level) / static_cast<double>(kScale), label,
                               ImGui::GetColorU32(tint), tag_col, IM_COL32(10, 10, 10, 255),
                               1.0F);
                // An order tag landing on the live-price tag hides the price; the order tag is
                // the one the trader placed deliberately, so it wins and the price is repainted
                // out from under it.
                if (price_tag_drawn && std::abs(tag.y - price_tag.y) < tag.half_height * 2.0F)
                    price_tag_drawn = false;
            }

            const float price_tag_y = price_tag.y;
            const float price_tag_left = price_tag.left;

            // Hand-drawn trading crosshair (ImPlotFlags_Crosshairs above only swaps the
            // mouse cursor -- docs/05 §2.3).
            if (ImPlot::IsPlotHovered()) {
                const ImPlotPoint m = ImPlot::GetPlotMousePos();
                const ImVec2 pos = ImPlot::GetPlotPos();
                const ImVec2 size = ImPlot::GetPlotSize();
                const ImVec2 px = ImPlot::PlotToPixels(m);
                ImDrawList* dl = ImPlot::GetPlotDrawList();

                char label[32];
                const Px price = static_cast<Px>(m.y * static_cast<double>(kScale));
                format_px(price, ctx.sz_decimals, label, sizeof(label));
                const ImVec2 text_size = ImGui::CalcTextSize(label);
                const float tag_left = pos.x + size.x - (text_size.x + 10.0F);
                // Sized from the text rather than a fixed +-8px: the old box was shorter than
                // a line of this font, so the glyphs hung out of their own background and the
                // gridlines behind them showed through the digits.
                const float half_h = text_size.y * 0.5F + 3.0F;

                // Dotted, and stopped well short of the price tag. A solid line reads as chart
                // content -- it was heavier than the gridlines it crossed -- and running it
                // under the tag put a bar through the digits the crosshair exists to report.
                // White and 2px: the old grey-at-130-alpha hairline was dimmer than the
                // gridlines it crossed, which is the one thing a crosshair must never be.
                constexpr ImU32 kCrosshair = IM_COL32(255, 255, 255, 215);
                constexpr float kCrosshairThickness = 2.0F;
                ImPlot::PushPlotClipRect();
                add_dotted_line(dl, ImVec2(pos.x, px.y), ImVec2(tag_left - 6.0F, px.y),
                                kCrosshair, kCrosshairThickness);
                // The vertical leg is broken around the tag as well -- it would otherwise cut
                // straight down through the digits whenever the cursor sits near the right
                // edge, which is exactly where the price axis is.
                const bool crosses_tag = px.x > tag_left - 6.0F;
                if (crosses_tag) {
                    add_dotted_line(dl, ImVec2(px.x, pos.y), ImVec2(px.x, px.y - half_h - 2.0F),
                                    kCrosshair, kCrosshairThickness);
                    add_dotted_line(dl, ImVec2(px.x, px.y + half_h + 2.0F),
                                    ImVec2(px.x, pos.y + size.y), kCrosshair,
                                    kCrosshairThickness);
                } else {
                    add_dotted_line(dl, ImVec2(px.x, pos.y), ImVec2(px.x, pos.y + size.y),
                                    kCrosshair, kCrosshairThickness);
                }
                ImPlot::PopPlotClipRect();

                // The live-price tag occupies the same gutter, so hide it while the cursor tag
                // would overlap it -- two stacked labels in the same strip are unreadable, and
                // the one the mouse is pointing at is the one being asked for.
                if (price_tag_drawn && std::abs(px.y - price_tag_y) < half_h * 2.0F) {
                    dl->AddRectFilled(ImVec2(price_tag_left, price_tag_y - half_h),
                                      ImVec2(pos.x + size.x, price_tag_y + half_h),
                                      IM_COL32(14, 14, 15, 255));
                }

                dl->AddRectFilled(ImVec2(tag_left, px.y - half_h),
                                  ImVec2(pos.x + size.x, px.y + half_h),
                                  IM_COL32(40, 40, 40, 255));
                dl->AddText(ImVec2(tag_left + 5.0F, px.y - text_size.y * 0.5F), IM_COL32_WHITE,
                            label);
            }

            ImPlot::EndPlot();
        }
    }
    ImGui::End();
}

}  // namespace pc::ui
