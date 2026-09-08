#include <imgui.h>

#include <cstdarg>
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
#include "ui/widgets/chart.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// Interval id -> duration in ms. Indexed by PC_IV_* (parsec.h) -- same table as
// md::kIntervalMs, kept here so the panel does not reach into md:: for a display constant.
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
constexpr const char* kIntervalLabels[PC_IV_COUNT] = {"1s",  "5s",  "10s", "30s", "1m", "5m",
                                                      "15m", "30m", "1h",  "4h",  "D"};

// Backing storage for CandleSeries::copy_recent()'s output. Function-local static rather than
// a per-frame allocation (docs/05 §5.3: no allocation on the hot path) or a stack array (at
// kCapacity x ~48B this is too large -- ~240KB -- to put on the stack every frame). Purely a
// scratch buffer for the read-in-place copy, not panel business state.
pc_candle g_candle_buf[md::CandleSeries::kCapacity];

// Fills the remaining panel body with a centred spinner and one line of text. Used while a
// timeframe/instrument switch is waiting on its REST backfill: the alternative the chart used
// to show was the two or three candles the live feed had folded so far, which reads as a
// broken chart rather than as "still loading".
void draw_loading_body(const char* text) noexcept {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x <= 0.0F || avail.y <= 0.0F)
        return;
    const ImVec2 centre(origin.x + avail.x * 0.5F, origin.y + avail.y * 0.5F - 10.0F);

    // A single sweeping arc rather than the usual dot ring: one path, no per-dot alpha, and it
    // stays legible at the small radius a panel this size affords.
    constexpr float kRadius = 13.0F;
    constexpr float kThickness = 2.5F;
    const float t = static_cast<float>(ImGui::GetTime());
    const float start = t * 3.0F;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->PathClear();
    dl->PathArcTo(centre, kRadius, start, start + 3.14159265F * 1.4F, 24);
    dl->PathStroke(col, ImDrawFlags_None, kThickness);

    const ImVec2 text_size = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(centre.x - text_size.x * 0.5F, centre.y + kRadius + 10.0F), col, text);
    // Claim the space so anything drawn after this does not overlap the spinner.
    ImGui::Dummy(avail);
}

// A toolbar toggle: same footprint as the timeframe pills, lit when on.
bool toolbar_toggle(const char* label, bool* value) {
    const bool on = *value;
    if (on)
        ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
    const bool clicked = ImGui::SmallButton(label);
    if (on)
        ImGui::PopStyleColor();
    if (clicked)
        *value = !*value;
    return clicked;
}

void push_level(ChartLevel* levels, size_t& count, size_t cap, Px price, const ImVec4& color,
                float thickness, float span, int rank, const char* fmt, ...) noexcept {
    if (count >= cap || price <= 0)
        return;
    ChartLevel& level = levels[count++];
    level.price = price;
    level.color = color;
    level.thickness = thickness;
    level.span = span;
    level.rank = rank;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(level.label, sizeof(level.label), fmt, args);
    va_end(args);
}

}  // namespace

void draw_chart(PanelContext& ctx) {
    // The order overlays below read this session's resting orders; see ui_event_store.hpp --
    // draining from any panel is safe and idempotent within a frame.
    event_store().drain(ctx.bridge);

    // Pure view chrome: not trading-relevant, not shared with any other panel, so it lives as
    // a function-local static rather than growing ViewState, which is a contract owned by
    // another agent (src/ui/panels.hpp). The log toggle is the exception -- ViewState already
    // carries chart_log_scale, so that one is kept where it was declared to live.
    static ChartState chart{};

    // The chart owns the whole window: no scrollbar, and the wheel belongs to the zoom rather
    // than to the dock node underneath it.
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (ImGui::Begin(kWindowChart, nullptr, kFlags)) {
        // --- timeframe selector -- switching only changes which cached CandleSeries we read,
        // it never discards or refetches anything (docs/07 Phase 2 acceptance). ---
        for (uint8_t iv = 0; iv < PC_IV_COUNT; ++iv) {
            if (iv != 0)
                ImGui::SameLine(0.0F, 3.0F);
            ImGui::PushID(static_cast<int>(iv));
            const bool selected = ctx.view.interval == iv;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Text, kColorAccent);
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

        ImGui::SameLine(0.0F, 14.0F);
        ImGui::SetNextItemWidth(92.0F);
        // Matched to SmallButton's frame so the toolbar sits on one baseline.
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(ImGui::GetStyle().FramePadding.x, 0.0F));
        const bool style_open = ImGui::BeginCombo("##style", chart_style_label(chart.style),
                                                  ImGuiComboFlags_HeightSmall);
        ImGui::PopStyleVar();
        if (style_open) {
            for (int i = 0; i < static_cast<int>(ChartStyle::kCount); ++i) {
                const ChartStyle style = static_cast<ChartStyle>(i);
                if (ImGui::Selectable(chart_style_label(style), chart.style == style))
                    chart.style = style;
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine(0.0F, 10.0F);
        chart.log_price = ctx.view.chart_log_scale;
        if (toolbar_toggle("log", &chart.log_price))
            ctx.view.chart_log_scale = chart.log_price;
        ImGui::SameLine(0.0F, 3.0F);
        toolbar_toggle("vol", &chart.show_volume);
        ImGui::SameLine(0.0F, 3.0F);
        // Auto is the price axis re-fitting the bars on screen. Unticking it hands the axis to
        // the mouse; the axis keeps whatever range the fit last produced, so it does not jump.
        toolbar_toggle("auto", &chart.auto_price);
        ImGui::SameLine(0.0F, 3.0F);
        // Arms the ruler for one drag. Shift+drag does the same without leaving the chart.
        toolbar_toggle("measure", &chart.measure.armed);
        ImGui::SameLine(0.0F, 3.0F);
        if (ImGui::SmallButton("reset"))
            reset_chart_view(chart);

        if (ctx.market == nullptr || !ctx.instrument.valid()) {
            ImGui::TextDisabled("waiting for market data...");
            ImGui::End();
            return;
        }

        const md::CandleSeries& series = ctx.market->candles[ctx.view.interval];
        const size_t n = series.copy_recent(g_candle_buf, md::CandleSeries::kCapacity);

        // --- backfill gate --------------------------------------------------------------
        // Switching coin or timeframe fires a REST candleSnapshot that takes a few hundred ms
        // to a couple of seconds to land, and until it does the series holds only whatever the
        // live feed has folded since the subscribe -- one or two candles. Drawing that is
        // worse than drawing nothing: the chart paints a lone bar, frames itself against it,
        // then visibly rebuilds when the history arrives.
        //
        // Sub-minute timeframes are exempt: they are folded locally from the trades stream and
        // have no backfill to wait for, so they would spin until the timeout every time and
        // then show the same one candle anyway.
        constexpr size_t kMinBars = 120;
        static uint8_t loading_interval = 0xFF;
        static uint32_t loading_asset = PC_ASSET_NONE;
        static double loading_since = 0.0;
        if (loading_interval != ctx.view.interval || loading_asset != ctx.instrument.asset) {
            loading_interval = ctx.view.interval;
            loading_asset = ctx.instrument.asset;
            loading_since = ImGui::GetTime();
        }
        // The gate is time-boxed, not open-ended. A thin market (or a timeframe whose whole
        // history is shorter than kMinBars) legitimately never reaches the threshold, and a
        // chart that spins forever is a worse failure than one drawn from short history.
        constexpr double kBackfillTimeoutS = 4.0;
        const bool venue_interval = ctx.view.interval >= PC_IV_FIRST_VENUE;
        if (venue_interval && n < kMinBars &&
            ImGui::GetTime() - loading_since < kBackfillTimeoutS) {
            char label[64];
            std::snprintf(label, sizeof(label), "loading %s candles...",
                          kIntervalLabels[ctx.view.interval]);
            draw_loading_body(label);
            ImGui::End();
            return;
        }

        if (n == 0) {
            // Sub-minute series are folded from live trades and have no history at all, so
            // "nothing yet" is the expected state right after connecting rather than a fault.
            if (!venue_interval)
                ImGui::TextDisabled(
                    "%s candles are built from live trades (the venue serves none below 1m) -- "
                    "waiting for the first print.",
                    kIntervalLabels[ctx.view.interval]);
            else
                ImGui::TextDisabled("no candles cached yet for this timeframe");
            ImGui::End();
            return;
        }

        // --- price levels the chart draws over the bars ---------------------------------
        // The live price is the widget's own (it is the newest bar's close, which is the one
        // price guaranteed to sit on the bars being drawn). Everything here is account state.
        constexpr size_t kMaxLevels = 24;
        ChartLevel levels[kMaxLevels];
        size_t level_count = 0;

        if (const portfolio::Position* pos = find_position(ctx.portfolio, ctx.instrument.asset)) {
            const pc_position& p = pos->value;
            const uint32_t max_leverage =
                lookup_asset(ctx.universe, pos->asset, ctx.sz_decimals).max_leverage;

            char sz_buf[32];
            format_qty(p.szi < 0 ? -p.szi : p.szi, ctx.sz_decimals, sz_buf, sizeof(sz_buf));
            if (p.entry_px > 0) {
                // Neutral, not long/short coloured: entry is a reference line, and giving it
                // the bid/ask palette would make it compete with the P&L colouring everything
                // else on this chart uses to mean better/worse. Full width -- it is the line
                // every candle on screen is measured against.
                push_level(levels, level_count, kMaxLevels, p.entry_px, kColorTextPrimary, 1.0F,
                           1.0F, 2, "%s %s", p.szi < 0 ? "SHORT" : "LONG", sz_buf);
            }

            bool liq_is_estimate = false;
            const Px liq = liquidation_px(p, ctx.portfolio, max_leverage, &liq_is_estimate);
            if (liq > 0) {
                // Labelled "LIQ EST" when it is this client's own estimate rather than the
                // venue's published price -- the same distinction the positions table draws,
                // and not one to hide on a line about being liquidated. Heavier and full width
                // for the same reason: it is the only level here that marks the position
                // ceasing to exist.
                push_level(levels, level_count, kMaxLevels, liq, kColorAsk, 2.0F, 1.0F, 3, "%s",
                           liq_is_estimate ? "LIQ EST" : "LIQ");
            }
        }

        // Resting orders for THIS asset only -- open_order_at() spans every coin, and a stop
        // on another market drawn against these candles is worse than not drawing it.
        for (size_t i = 0; i < event_store().open_order_count(); ++i) {
            const OrderRow& order = event_store().open_order_at(i);
            if (order.asset != ctx.instrument.asset)
                continue;
            // A trigger order rests at its trigger; `px` is the limit it converts to once it
            // fires, which for a market trigger is ~5% away by design. Drawing `px` there
            // would put the stop line nowhere near the stop.
            const Px level = order.is_trigger ? order.trigger_px : order.px;
            if (level <= 0)
                continue;
            char sz_buf[32];
            format_qty(order.sz, ctx.sz_decimals, sz_buf, sizeof(sz_buf));
            const char* kind = order.tpsl == PC_TPSL_TP   ? "TP"
                               : order.tpsl == PC_TPSL_SL ? "SL"
                               : order.is_buy             ? "BUY"
                                                          : "SELL";
            // Take-profits read as the winning side and stops as the losing one whichever way
            // the position points, so they are coloured by leg rather than by side; a plain
            // limit keeps the bid/ask colour of the side it sits on.
            const ImVec4 tint = order.tpsl == PC_TPSL_TP   ? kColorBid
                                : order.tpsl == PC_TPSL_SL ? kColorWarning
                                : order.is_buy             ? kColorBid
                                                           : kColorAsk;
            // Full width, like the account-wide levels: a resting order is a price the whole
            // visible history is read against, and a stub hugging the axis reads as an
            // annotation on the last few bars rather than as a level. Dashes and the dimmer
            // tint already keep them behind the candles.
            push_level(levels, level_count, kMaxLevels, level, tint, 1.0F, 1.0F, 4, "%s %s", kind,
                       sz_buf);
        }

        ChartInput in{};
        in.candles = g_candle_buf;
        in.count = n;
        in.asset = ctx.instrument.asset;
        in.interval = ctx.view.interval;
        in.interval_ms = kIntervalMs[ctx.view.interval];
        in.now_ms = ctx.now_ms;
        in.sz_decimals = ctx.sz_decimals;
        in.symbol = lookup_asset(ctx.universe, ctx.instrument.asset, ctx.sz_decimals).coin;
        in.interval_label = kIntervalLabels[ctx.view.interval];
        in.levels = levels;
        in.level_count = level_count;
        draw_chart_widget(chart, in);
    }
    ImGui::End();
}

}  // namespace pc::ui
