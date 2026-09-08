#pragma once
// The chart widget: a bar-indexed candlestick pane with its own price and time axes, drawn
// entirely with ImDrawList.
//
// It does NOT use ImPlot. ImPlot models X as a continuous quantity, and a trading chart does
// not: it steps by BAR, so a weekend, a venue outage or a stretch the client was unsubscribed
// for collapses to nothing instead of opening a hole, zoom is "pixels per bar" rather than
// "seconds per pixel", and every bar is the same width whatever its timestamp says. That is
// how TradingView's own engine is built (its entire view state is bar spacing plus a right
// offset), and the auto-fit/seed/gutter workarounds the ImPlot version needed here were all
// this one mismatch surfacing in different places.
#include <imgui.h>

#include <cstddef>
#include <cstdint>

#include "core/units.hpp"
#include "parsec/parsec.h"

namespace pc::ui {

enum class ChartStyle : uint8_t { Candles = 0, Hollow, Bars, Line, Area, kCount };
const char* chart_style_label(ChartStyle style) noexcept;

// The ruler: drag out a box and read what the move was worth. Held in the view state because a
// measurement survives the drag that made it -- the point is to read it afterwards.
struct ChartMeasure {
    bool armed{false};  // the toolbar button: the next drag measures instead of panning
    bool dragging{false};
    bool shown{false};
    // Bar indices, which may fall outside the series: measuring into the empty pane to the
    // right of the last bar is a normal thing to want (how long until this reaches that level).
    long from_index{};
    long to_index{};
    Px from_price{};
    Px to_price{};
};

// Everything the view remembers between frames. Owned by the panel so the toolbar can drive it.
struct ChartState {
    // Zoom, as pixels per bar -- the unit the whole view is built on. Panning is then a whole
    // number of these, so bars never shimmer between subpixel positions while dragging.
    float bar_spacing{8.0F};
    // Pan, as bars of empty pane past the NEWEST bar. Measuring it from the newest bar rather
    // than in absolute index space is what makes the chart follow a live market for free: a
    // new bar moves the newest index, and the window comes with it. Scrolled back into
    // history the offset is compensated instead, so the view holds still (§ chart-model.ts).
    double right_offset{6.0};

    bool auto_price{true};  // price axis re-fits the visible bars every frame
    bool log_price{false};
    double price_lo{0.0};  // committed axis range; only authoritative while !auto_price
    double price_hi{1.0};

    ChartStyle style{ChartStyle::Candles};
    bool show_volume{true};
    ChartMeasure measure{};

    // --- bookkeeping the widget owns; the panel should not touch these -------------------
    uint32_t framed_asset{0xFFFFFFFFU};
    uint8_t framed_interval{0xFFU};
    bool framed{false};
    uint64_t last_open_ms{0};  // newest bar last frame, to count what has printed since
    bool last_bar_visible{true};
    double drag_price_lo{};  // price range snapshotted at the start of an axis drag, so a
    double drag_price_hi{};  // long drag stays a function of total travel, not of frames
    float drag_bar_spacing{};
    double scroll_from{};  // jump-to-realtime animation
    double scroll_to{};
    double scroll_t0{-1.0};
};

// A labelled horizontal price line: a rule across some or all of the pane, a name chip at its
// right-hand end, and a price tag in the axis gutter. The position's entry and liquidation and
// every resting order go through this one shape so they cannot drift apart in weight or
// geometry, and so they share one collision pass over the gutter.
struct ChartLevel {
    Px price{};
    char label[32]{};  // "ENTRY", "LIQ EST", "BUY 0.10" -- the name, never the price
    ImVec4 color{};
    float thickness{1.0F};
    // How much of the pane width the rule covers, measured from the right edge (TradingView's
    // setLineLength). Every level drawn today runs the full width; the knob stays for levels
    // that should read as local annotations rather than as prices the whole history is against.
    float span{1.0F};
    // Who keeps their tag when two land on the same strip of gutter. The trader placed the
    // orders deliberately, so they outrank the live price, which is legible from the last bar
    // anyway.
    int rank{};
};

struct ChartInput {
    const pc_candle* candles{};
    size_t count{};
    // Identity of the series, so the widget can tell "the same chart, one bar later" from "a
    // different instrument" and re-frame only for the latter.
    uint32_t asset{};
    uint8_t interval{};
    uint64_t interval_ms{};
    uint64_t now_ms{};
    uint8_t sz_decimals{};
    const char* symbol{};
    const char* interval_label{};
    const ChartLevel* levels{};
    size_t level_count{};
};

// Draws the chart filling the current window's remaining content region, and applies this
// frame's mouse input to `state`. Call between ImGui::Begin()/End().
void draw_chart_widget(ChartState& state, const ChartInput& in);

// Re-frame: default zoom, back at the live edge, auto price. What the toolbar's reset does,
// and what a coin or timeframe switch does implicitly.
void reset_chart_view(ChartState& state) noexcept;

}  // namespace pc::ui
