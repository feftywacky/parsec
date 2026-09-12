#include "ui/widgets/chart.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/theme.hpp"
#include "ui/widgets/chart_scale.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// --- view limits -----------------------------------------------------------------------
// TradingView's own defaults, adjusted for a desktop pane: bar spacing 8 rather than the 6 a
// web widget ships, and a hard floor of 1px per bar. Below one pixel a candle is not a candle,
// and the alternative -- aggregating bars into buckets to fake it -- draws a chart whose bars
// are not the bars the venue published.
constexpr float kMinBarSpacing = 1.0F;
constexpr float kMaxBarSpacing = 60.0F;
constexpr float kDefaultBarSpacing = 8.0F;
constexpr double kDefaultRightOffset = 6.0;  // bars of empty pane past the newest bar
constexpr double kMinVisibleBars = 2.0;      // never let a pan leave the pane blank
constexpr float kZoomPerNotch = 1.10F;       // ±10% of bar spacing, as TradingView zooms
constexpr double kScrollAnimSeconds = 0.4;

// Price headroom, as fractions of pane height rather than of the price range: a 10% margin
// should be 10% of the screen at any volatility, which is not what padding the range gives.
// The bottom is deliberately deep enough to clear the volume histogram.
constexpr double kTopMargin = 0.10;
constexpr double kBottomMarginVolume = 0.25;
constexpr double kBottomMargin = 0.08;
constexpr float kVolumeFrac = 0.20F;

constexpr ImU32 kGrid = IM_COL32(37, 54, 60, 255);
constexpr ImU32 kAxisText = IM_COL32(112, 135, 140, 255);
constexpr ImU32 kAxisTextMajor = IM_COL32(168, 190, 194, 255);
constexpr ImU32 kCrosshair = IM_COL32(149, 152, 161, 235);  // TradingView's #9598A1
constexpr ImU32 kChipBg = IM_COL32(26, 40, 46, 255);
constexpr ImU32 kChipText = IM_COL32(226, 236, 238, 255);

// Candle green and red are NOT the app's long/short palette. Every serious terminal --
// Hyperliquid's own web app included -- paints candles in TradingView's material teal/red and
// keeps its brand green/red for buttons, book rows and P&L, because the two are read
// differently: candles are a texture you scan, chrome is a signal you act on.
constexpr ImVec4 kCandleUp{0.149F, 0.651F, 0.604F, 1.0F};    // #26A69A
constexpr ImVec4 kCandleDown{0.937F, 0.325F, 0.314F, 1.0F};  // #EF5350

// The ruler is deliberately a colour nothing else on this chart uses. It is scaffolding, not
// market data, and it must never be mistaken for a level someone put there on purpose.
constexpr ImU32 kMeasureLine = IM_COL32(41, 98, 255, 255);  // #2962FF
constexpr ImU32 kMeasureFill = IM_COL32(41, 98, 255, 38);
constexpr ImU32 kMeasureEdge = IM_COL32(41, 98, 255, 130);

ImU32 col_of(const ImVec4& c, float alpha = 1.0F) noexcept {
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

// Black or white, whichever stays readable on `bg`. TradingView's own weighting (an NTSC-ish
// luma) with its 160/255 threshold -- worth copying verbatim, because the alternative is that
// one day someone gives a level an amber tag and the price on it becomes unreadable.
ImU32 contrast_text(const ImVec4& bg) noexcept {
    const float luma = 0.199F * bg.x + 0.687F * bg.y + 0.114F * bg.z;
    return luma > 0.627F ? IM_COL32(12, 18, 22, 255) : IM_COL32(255, 255, 255, 255);
}

float snap(float v) noexcept {
    return std::floor(v) + 0.5F;
}

// ImDrawList has no dash pattern, so dashes are walked out by hand, on whole-pixel steps so
// they land on the same grid the gridlines do and never shimmer while panning.
void dashed_h(ImDrawList* dl, float x_from, float x_to, float y, ImU32 c, float thickness,
              float dash, float gap) {
    const float sy = snap(y);
    for (float x = std::floor(x_from); x < x_to; x += dash + gap)
        dl->AddLine(ImVec2(x, sy), ImVec2(std::min(x + dash, x_to), sy), c, thickness);
}

void dashed_v(ImDrawList* dl, float y_from, float y_to, float x, ImU32 c, float thickness,
              float dash, float gap) {
    const float sx = snap(x);
    for (float y = std::floor(y_from); y < y_to; y += dash + gap)
        dl->AddLine(ImVec2(sx, y), ImVec2(sx, std::min(y + dash, y_to)), c, thickness);
}

// Candle body width for a given bar spacing, from TradingView's optimalCandlestickWidth. A
// fixed fraction reads too fat when zoomed in and closes the gap between bars entirely when
// zoomed out, so the coefficient eases from 1.0 towards 0.8 as bars get wider. Rounded to whole
// pixels and forced odd, so the 1px wick sits exactly on the body's centre column -- which is
// what stops a row of candles looking unevenly weighted.
float body_width(float bar_spacing) noexcept {
    if (bar_spacing >= 2.5F && bar_spacing <= 4.0F)
        return 3.0F;
    const float coeff =
        1.0F - 0.2F * std::atan(std::max(4.0F, bar_spacing) - 4.0F) / (3.14159265F * 0.5F);
    float w = std::min(std::floor(bar_spacing * coeff), std::floor(bar_spacing));
    if (w >= 2.0F && std::fmod(w, 2.0F) != 1.0F)
        w -= 1.0F;
    return std::max(1.0F, w);
}

double to_px(pc_px v) noexcept {
    return static_cast<double>(v) / static_cast<double>(kScale);
}

// 1.2K / 34.7M -- volume is scanned, not read to the unit.
const char* format_compact(double v, char* out, size_t cap) noexcept {
    const double a = std::abs(v);
    if (a >= 1e9)
        std::snprintf(out, cap, "%.2fB", v / 1e9);
    else if (a >= 1e6)
        std::snprintf(out, cap, "%.2fM", v / 1e6);
    else if (a >= 1e3)
        std::snprintf(out, cap, "%.2fK", v / 1e3);
    else
        std::snprintf(out, cap, "%.4g", v);
    return out;
}

struct Xform {
    float x0{}, x1{}, y0{}, y1{};
    double left_bar{};
    float bar_spacing{};
    double lo{}, hi{};
    bool log{};

    [[nodiscard]] float x(double bar) const noexcept {
        return x0 + static_cast<float>((bar - left_bar) * static_cast<double>(bar_spacing));
    }
    [[nodiscard]] double bar_at(float px) const noexcept {
        return left_bar + static_cast<double>(px - x0) / static_cast<double>(bar_spacing);
    }
    [[nodiscard]] float y(double price) const noexcept {
        return y1 - static_cast<float>(price_frac(price, lo, hi, log)) * (y1 - y0);
    }
    [[nodiscard]] double price_at(float py) const noexcept {
        return frac_price(static_cast<double>(y1 - py) / static_cast<double>(y1 - y0), lo, hi, log);
    }
};

// Compress or expand a price range about `pivot` (0 = the bottom of the pane, 1 = the top), in
// whatever space the axis is in, so a log axis scales by ratio and a linear one by distance.
void scale_price(double& lo, double& hi, bool log, double factor, double pivot) noexcept {
    const double a = log ? std::log10(std::max(lo, 1e-12)) : lo;
    const double b = log ? std::log10(std::max(hi, 1e-12)) : hi;
    const double p = a + (b - a) * pivot;
    const double span = (b - a) * factor;
    const double na = p - span * pivot;
    const double nb = p + span * (1.0 - pivot);
    lo = log ? std::pow(10.0, na) : na;
    hi = log ? std::pow(10.0, nb) : nb;
}

// A line with a head on its far end. Used for the ruler's two axes, whose whole job is to say
// which way the move went.
void arrow(ImDrawList* dl, ImVec2 from, ImVec2 to, ImU32 col) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1.0F) {
        dl->AddLine(from, to, col, 1.5F);
        return;
    }
    const float ux = dx / len;
    const float uy = dy / len;
    const float head = std::min(9.0F, len * 0.5F);
    const ImVec2 base(to.x - ux * head, to.y - uy * head);
    dl->AddLine(from, base, col, 1.5F);
    dl->AddTriangleFilled(to, ImVec2(base.x - uy * head * 0.45F, base.y + ux * head * 0.45F),
                          ImVec2(base.x + uy * head * 0.45F, base.y - ux * head * 0.45F), col);
}

struct TagSlot {
    float y{};
    float half{};
};

// Tags share one narrow strip of gutter, so two that land on the same pixels are unreadable
// rather than merely untidy. Higher-ranked tags are placed first and later ones simply do not
// draw -- nudging them apart would put a price label at a price it is not at.
bool slot_free(const TagSlot* taken, int count, float y, float half) noexcept {
    for (int i = 0; i < count; ++i) {
        if (std::abs(taken[i].y - y) < taken[i].half + half + 1.0F)
            return false;
    }
    return true;
}

// A price tag in the axis gutter. `arrow` points off-screen for a level the pane has scrolled
// past: a liquidation price you cannot see is the one you most need told about, so the tag
// pins to the edge rather than disappearing with its line.
void gutter_tag(ImDrawList* dl, float gx0, float gx1, float y, const char* text, ImU32 bg, ImU32 fg,
                int arrow = 0) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const float half = size.y * 0.5F + 3.0F;
    const float top = std::round(y - half);
    const float bottom = std::round(y + half);
    dl->AddRectFilled(ImVec2(gx0 + 3.0F, top), ImVec2(gx1, bottom), bg, 2.0F);
    dl->AddText(ImVec2(gx0 + 8.0F, std::round(y - size.y * 0.5F)), fg, text);
    if (arrow != 0) {
        const float tip = arrow < 0 ? top - 4.0F : bottom + 4.0F;
        dl->AddTriangleFilled(ImVec2(gx0 + 3.0F, arrow < 0 ? top : bottom),
                              ImVec2(gx1, arrow < 0 ? top : bottom),
                              ImVec2((gx0 + gx1) * 0.5F, tip), bg);
    }
}

}  // namespace

const char* chart_style_label(ChartStyle style) noexcept {
    switch (style) {
        case ChartStyle::Candles:
            return "candles";
        case ChartStyle::Hollow:
            return "hollow";
        case ChartStyle::Bars:
            return "bars";
        case ChartStyle::Line:
            return "line";
        case ChartStyle::Area:
            return "area";
        default:
            return "candles";
    }
}

void reset_chart_view(ChartState& state) noexcept {
    state.bar_spacing = kDefaultBarSpacing;
    state.right_offset = kDefaultRightOffset;
    state.auto_price = true;
    state.scroll_t0 = -1.0;
    state.framed = false;
}

void draw_chart_widget(ChartState& s, const ChartInput& in) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 120.0F || avail.y < 80.0F || in.candles == nullptr || in.count == 0) {
        if (avail.x > 0.0F && avail.y > 0.0F)
            ImGui::Dummy(avail);
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const float text_h = ImGui::GetTextLineHeight();
    const size_t n = in.count;
    const pc_candle& newest = in.candles[n - 1];

    // --- pane geometry -----------------------------------------------------------------
    // The gutter is sized from a real price at this asset's precision, not from the widest
    // label currently on screen: a width that tracked the live labels would breathe by a few
    // pixels every time the price crossed a digit boundary, and the whole plot would shift
    // with it. Padding follows TradingView's axis metrics, scaled to this font.
    char probe[48];
    format_px(newest.c > 0 ? newest.c : in.candles[0].c, in.sz_decimals, probe, sizeof(probe));
    const float gutter_w = std::max(46.0F, ImGui::CalcTextSize(probe).x + 24.0F);
    const float strip_h = text_h + 9.0F;

    const float x0 = origin.x;
    const float x1 = origin.x + avail.x - gutter_w;
    const float y0 = origin.y;
    const float y1 = origin.y + avail.y - strip_h;
    const float plot_w = x1 - x0;
    const float plot_h = y1 - y0;
    if (plot_w < 60.0F || plot_h < 60.0F) {
        ImGui::Dummy(avail);
        return;
    }

    if (!s.framed || s.framed_asset != in.asset || s.framed_interval != in.interval) {
        s.framed = true;
        s.framed_asset = in.asset;
        s.framed_interval = in.interval;
        s.bar_spacing = kDefaultBarSpacing;
        s.right_offset = kDefaultRightOffset;
        s.auto_price = true;
        s.last_open_ms = 0;
        s.last_bar_visible = true;
        s.scroll_t0 = -1.0;
    }

    // --- what has printed since the last frame ------------------------------------------
    // Counted by looking the previous newest bar back up rather than by differencing `count`:
    // the series is a fixed-capacity ring, so once it is full a new bar leaves the count
    // unchanged and shifts every index down by one instead.
    size_t added = 0;
    if (s.last_open_ms != 0 && newest.open_ms > s.last_open_ms) {
        size_t lo = 0;
        size_t hi = n;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (in.candles[mid].open_ms < s.last_open_ms)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < n && in.candles[lo].open_ms == s.last_open_ms)
            added = (n - 1) - lo;
    }
    s.last_open_ms = newest.open_ms;
    // The offset is measured from the newest bar, so a new bar slides the window forward on its
    // own -- which is exactly what you want while the live edge is on screen. Scrolled back
    // into history it is not: compensate, and the bars under the cursor stay put.
    if (added > 0 && !s.last_bar_visible)
        s.right_offset -= static_cast<double>(added);

    // --- input -------------------------------------------------------------------------
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##chart_plot", ImVec2(plot_w, plot_h));
    const bool plot_hovered = ImGui::IsItemHovered();
    const bool plot_active = ImGui::IsItemActive();
    const bool plot_activated = ImGui::IsItemActivated();
    const bool plot_double = plot_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

    ImGui::SetCursorScreenPos(ImVec2(x1, y0));
    ImGui::InvisibleButton("##chart_price_axis", ImVec2(gutter_w, plot_h));
    const bool price_axis_start = ImGui::IsItemActivated();
    const bool price_axis_active = ImGui::IsItemActive();
    const bool price_axis_hovered = ImGui::IsItemHovered();
    const bool price_axis_double =
        price_axis_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (price_axis_hovered || price_axis_active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

    ImGui::SetCursorScreenPos(ImVec2(x0, y1));
    ImGui::InvisibleButton("##chart_time_axis", ImVec2(plot_w, strip_h));
    const bool time_axis_start = ImGui::IsItemActivated();
    const bool time_axis_active = ImGui::IsItemActive();
    const bool time_axis_hovered = ImGui::IsItemHovered();
    const bool time_axis_double =
        time_axis_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (time_axis_hovered || time_axis_active)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    const double last_bar = static_cast<double>(n - 1);
    auto bar_under = [&](float px) {
        const double visible = static_cast<double>(plot_w) / static_cast<double>(s.bar_spacing);
        const double right = last_bar + s.right_offset;
        return right - visible + static_cast<double>(px - x0) / static_cast<double>(s.bar_spacing);
    };
    const bool any_input = plot_active || price_axis_active || time_axis_active ||
                           io.MouseWheel != 0.0F || io.MouseWheelH != 0.0F;
    if (any_input)
        s.scroll_t0 = -1.0;  // the user took the wheel back

    // The price a pixel sat at when it was clicked. s.price_lo/hi still hold the range this
    // frame started with, which is the one the user was looking at when they pressed -- the
    // committed range further down is a frame too late to answer that question.
    auto price_under = [&](float py) {
        return frac_price(static_cast<double>(y1 - py) / static_cast<double>(plot_h), s.price_lo,
                          s.price_hi, s.log_price);
    };

    // --- the ruler ---------------------------------------------------------------------
    // Shift+click (or the armed toolbar button) starts a measurement; the box then follows the
    // cursor with no button held, and a second click pins it. Dragging and releasing still
    // works too, for hands that expect it.
    ChartMeasure& ruler = s.measure;
    if (plot_activated) {
        if (ruler.dragging) {
            ruler.dragging = false;
            ruler.armed = false;  // one-shot, like every other chart's ruler button
        } else if (ruler.armed || io.KeyShift) {
            ruler.dragging = true;
            ruler.shown = true;
            ruler.from_index = static_cast<long>(std::llround(bar_under(io.MousePos.x)));
            ruler.from_price = static_cast<Px>(
                std::llround(price_under(io.MousePos.y) * static_cast<double>(kScale)));
            ruler.to_index = ruler.from_index;
            ruler.to_price = ruler.from_price;
        } else {
            ruler.shown = false;  // a plain click puts the chart back
        }
    }
    if (ruler.dragging) {
        ruler.to_index = static_cast<long>(std::llround(bar_under(io.MousePos.x)));
        ruler.to_price = static_cast<Px>(
            std::llround(price_under(io.MousePos.y) * static_cast<double>(kScale)));
        // A real drag (past the same 4px threshold panning uses) ends on release; a plain
        // click leaves the box following the cursor until the next click.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] > 16.0F) {
            ruler.dragging = false;
            ruler.armed = false;
        }
    }
    if (ImGui::IsWindowFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ruler.shown = false;
        ruler.armed = false;
        ruler.dragging = false;
    }

    // Wheel zooms the time axis about the bar under the cursor, TradingView's default. It never
    // touches price: the price axis re-fits, or it is locked, and either way the wheel scaling
    // both at once is the thing that makes a naive plot feel unlike a chart.
    if (plot_hovered && io.MouseWheel != 0.0F && !io.KeyShift) {
        const double anchor = bar_under(io.MousePos.x);
        s.bar_spacing = std::clamp(s.bar_spacing * std::pow(kZoomPerNotch, io.MouseWheel),
                                   kMinBarSpacing, kMaxBarSpacing);
        s.right_offset += anchor - bar_under(io.MousePos.x);
    }
    // Horizontal wheel (and shift+wheel) pans -- a trackpad's second axis, and TradingView's
    // shift+wheel shortcut, land on the same behaviour.
    const float pan_wheel = io.MouseWheelH + (io.KeyShift ? io.MouseWheel : 0.0F);
    if (plot_hovered && pan_wheel != 0.0F)
        s.right_offset -= static_cast<double>(pan_wheel) * 3.0;

    if (plot_active && !ruler.dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0F)) {
        if (io.MouseDelta.x != 0.0F)
            s.right_offset -= static_cast<double>(io.MouseDelta.x / s.bar_spacing);
        // Vertical drag pans price only once the axis has been locked. In auto mode the chart
        // deliberately refuses to move vertically -- it is what stops a stray drag leaving a
        // trader looking at empty space where the candles were.
        if (io.MouseDelta.y != 0.0F && !s.auto_price) {
            const double d = static_cast<double>(io.MouseDelta.y) / static_cast<double>(plot_h);
            const double nlo = frac_price(d, s.price_lo, s.price_hi, s.log_price);
            const double nhi = frac_price(1.0 + d, s.price_lo, s.price_hi, s.log_price);
            if (nlo > 0.0 && nhi > nlo) {
                s.price_lo = nlo;
                s.price_hi = nhi;
            }
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    // The axes take the wheel too, each zooming its own scale. Without this the price scale is
    // reachable only by dragging it, which is not where a hand goes first -- and until it has
    // been touched at all, auto-fit means a vertical drag in the pane does nothing either.
    if (price_axis_hovered && io.MouseWheel != 0.0F) {
        s.auto_price = false;
        double lo = s.price_lo;
        double hi = s.price_hi;
        const double pivot = std::clamp(
            static_cast<double>(y1 - io.MousePos.y) / static_cast<double>(plot_h), 0.0, 1.0);
        scale_price(lo, hi, s.log_price, std::pow(1.0 / kZoomPerNotch, io.MouseWheel), pivot);
        if (hi > lo && (!s.log_price || lo > 0.0)) {
            s.price_lo = lo;
            s.price_hi = hi;
        }
    }
    if (time_axis_hovered && io.MouseWheel != 0.0F) {
        s.bar_spacing = std::clamp(s.bar_spacing * std::pow(kZoomPerNotch, io.MouseWheel),
                                   kMinBarSpacing, kMaxBarSpacing);
    }

    // Dragging the price axis scales it about the pane's centre and LOCKS auto-fit off -- that
    // second half is the important one, since without it the fit would undo the drag on the
    // next frame and the axis would feel nailed down. The (h-1)*0.2 term is TradingView's
    // softener: without it the gain runs away as the cursor approaches the pane edge.
    if (price_axis_start) {
        s.drag_price_lo = s.price_lo;
        s.drag_price_hi = s.price_hi;
    }
    if (price_axis_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0F)) {
        s.auto_price = false;
        const double h = static_cast<double>(plot_h);
        const double start = std::clamp(static_cast<double>(y1 - io.MouseClickedPos[0].y), 0.0, h);
        const double now = std::clamp(static_cast<double>(y1 - io.MousePos.y), 0.0, h);
        const double soften = (h - 1.0) * 0.2;
        const double k = std::max(0.1, (start + soften) / (now + soften));
        const double a =
            s.log_price ? std::log10(std::max(s.drag_price_lo, 1e-12)) : s.drag_price_lo;
        const double b =
            s.log_price ? std::log10(std::max(s.drag_price_hi, 1e-12)) : s.drag_price_hi;
        const double centre = (a + b) * 0.5;
        const double half = (b - a) * 0.5 * k;
        const double nlo = s.log_price ? std::pow(10.0, centre - half) : centre - half;
        const double nhi = s.log_price ? std::pow(10.0, centre + half) : centre + half;
        if (nhi > nlo && (!s.log_price || nlo > 0.0)) {
            s.price_lo = nlo;
            s.price_hi = nhi;
        }
    }

    // Dragging the time axis scales bar spacing by how much closer to the right edge the cursor
    // has come, so the right edge stays put and the chart opens out from under the mouse.
    if (time_axis_start)
        s.drag_bar_spacing = s.bar_spacing;
    if (time_axis_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0F)) {
        const float from_right_now = std::clamp(x1 - io.MousePos.x, 0.0F, plot_w);
        const float from_right_start = std::clamp(x1 - io.MouseClickedPos[0].x, 0.0F, plot_w);
        if (from_right_now > 0.0F && from_right_start > 0.0F)
            s.bar_spacing = std::clamp(s.drag_bar_spacing * from_right_now / from_right_start,
                                       kMinBarSpacing, kMaxBarSpacing);
    }

    if (plot_double)
        reset_chart_view(s);
    if (price_axis_double)
        s.auto_price = true;
    if (time_axis_double) {
        s.bar_spacing = kDefaultBarSpacing;
        s.right_offset = kDefaultRightOffset;
    }

    // --- resolve the window ------------------------------------------------------------
    if (s.scroll_t0 >= 0.0) {
        const double t = (ImGui::GetTime() - s.scroll_t0) / kScrollAnimSeconds;
        if (t >= 1.0) {
            s.right_offset = s.scroll_to;
            s.scroll_t0 = -1.0;
        } else {
            const double eased = t * t * (3.0 - 2.0 * t);
            s.right_offset = s.scroll_from + (s.scroll_to - s.scroll_from) * eased;
        }
    }
    const double bars_visible = static_cast<double>(plot_w) / static_cast<double>(s.bar_spacing);
    const double keep = std::min(kMinVisibleBars, static_cast<double>(n));
    s.right_offset = std::clamp(s.right_offset, -last_bar - 1.0 + keep, bars_visible - keep);
    const double right_bar = last_bar + s.right_offset;
    s.last_bar_visible = s.right_offset >= 0.0 && s.right_offset <= bars_visible;

    Xform tx{};
    tx.x0 = x0;
    tx.x1 = x1;
    tx.y0 = y0;
    tx.y1 = y1;
    tx.bar_spacing = s.bar_spacing;
    tx.left_bar = right_bar - bars_visible;
    tx.log = s.log_price;

    const long first_l = static_cast<long>(std::floor(tx.left_bar)) - 1;
    const long last_l = static_cast<long>(std::ceil(right_bar)) + 1;
    const size_t i0 = static_cast<size_t>(std::clamp<long>(first_l, 0, static_cast<long>(n) - 1));
    const size_t i1 = static_cast<size_t>(std::clamp<long>(last_l, 0, static_cast<long>(n) - 1));

    // --- price range -------------------------------------------------------------------
    if (s.auto_price) {
        double lo = 1e300;
        double hi = -1e300;
        for (size_t i = i0; i <= i1; ++i) {
            lo = std::min(lo, to_px(in.candles[i].l));
            hi = std::max(hi, to_px(in.candles[i].h));
        }
        if (!(hi > lo)) {
            const double mid = hi > -1e299 ? hi : to_px(newest.c);
            lo = mid * 0.999;
            hi = mid * 1.001;
        }
        const double bottom = s.show_volume ? kBottomMarginVolume : kBottomMargin;
        const double a = s.log_price ? std::log10(std::max(lo, 1e-12)) : lo;
        const double b = s.log_price ? std::log10(std::max(hi, 1e-12)) : hi;
        const double span = std::max(b - a, 1e-9);
        const double usable = 1.0 - kTopMargin - bottom;
        const double na = a - span / usable * bottom;
        const double nb = b + span / usable * kTopMargin;
        s.price_lo = s.log_price ? std::pow(10.0, na) : na;
        s.price_hi = s.log_price ? std::pow(10.0, nb) : nb;
    }
    // A manual axis is scaled from a snapshot each drag, but nothing stops a trader dragging
    // repeatedly, and the range compounds. Bound it well past any price the venue lists, so the
    // tick generator is never handed a range it cannot put on the fixed-point grid.
    constexpr double kPriceCeiling = 1.0e9;
    s.price_lo = std::clamp(s.price_lo, 1.0e-9, kPriceCeiling);
    s.price_hi = std::clamp(s.price_hi, 1.0e-9, kPriceCeiling);
    if (!(s.price_hi > s.price_lo)) {
        const double mid = std::clamp(to_px(newest.c), 1.0e-9, kPriceCeiling);
        s.price_lo = mid * 0.99;
        s.price_hi = mid * 1.01;
    }
    tx.lo = s.price_lo;
    tx.hi = s.price_hi;

    // --- crosshair state (resolved before drawing so the legend can follow it) ----------
    const bool crosshair = plot_hovered && !price_axis_active && !time_axis_active;
    long hover_index = -1;
    if (crosshair) {
        const long b = static_cast<long>(std::llround(tx.bar_at(io.MousePos.x)));
        if (b >= 0 && b < static_cast<long>(n))
            hover_index = b;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    // --- grid ---------------------------------------------------------------------------
    // Label spacing follows TradingView's own budget, scaled to this font: 2.5 line-heights
    // between price labels, and (fontSize + 4) * 5 between time labels.
    const int max_dp = 6 - static_cast<int>(std::min<uint8_t>(in.sz_decimals, 6));
    const PriceTicks pticks = price_ticks(tx.lo, tx.hi, plot_h, text_h * 2.5F, s.log_price, max_dp);
    const TimeTicks tticks =
        time_ticks(in.candles, i0, i1, in.interval_ms, s.bar_spacing, (text_h + 4.0F) * 5.0F);
    // Only the heaviest boundary actually on screen reads as a heading; on a 5m chart that is
    // the day, and the hours between it stay quiet.
    TimeTickWeight major = TimeTickWeight::Minor;
    for (int i = 0; i < tticks.count; ++i)
        major = std::max(major, tticks.ticks[i].weight);

    dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    for (int i = 0; i < pticks.count; ++i) {
        const float y = snap(tx.y(to_px(pticks.values[i])));
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), kGrid);
    }
    for (int i = 0; i < tticks.count; ++i) {
        const float x = snap(tx.x(static_cast<double>(tticks.ticks[i].index)));
        dl->AddLine(ImVec2(x, y0), ImVec2(x, y1), kGrid);
    }

    // --- volume --------------------------------------------------------------------------
    const float body_w = body_width(s.bar_spacing);
    const float half_w = body_w * 0.5F;
    if (s.show_volume) {
        double max_vol = 0.0;
        for (size_t i = i0; i <= i1; ++i)
            max_vol = std::max(max_vol, to_px(in.candles[i].v));
        if (max_vol > 0.0) {
            const float band = plot_h * kVolumeFrac;
            const ImU32 up = col_of(kCandleUp, 0.42F);
            const ImU32 down = col_of(kCandleDown, 0.42F);
            for (size_t i = i0; i <= i1; ++i) {
                const float cx = tx.x(static_cast<double>(i));
                const float h = band * static_cast<float>(to_px(in.candles[i].v) / max_vol);
                const float left = std::round(cx - half_w);
                dl->AddRectFilled(ImVec2(left, y1 - std::max(h, 1.0F)), ImVec2(left + body_w, y1),
                                  in.candles[i].c >= in.candles[i].o ? up : down);
            }
        }
    }

    // --- series --------------------------------------------------------------------------
    const ImU32 bull = col_of(kCandleUp);
    const ImU32 bear = col_of(kCandleDown);
    if (s.style == ChartStyle::Line || s.style == ChartStyle::Area) {
        const ImU32 line = col_of(kColorAccent);
        const ImU32 fill = col_of(kColorAccent, 0.13F);
        float prev_x = 0.0F;
        float prev_y = 0.0F;
        for (size_t i = i0; i <= i1; ++i) {
            const float cx = tx.x(static_cast<double>(i));
            const float cy = tx.y(to_px(in.candles[i].c));
            if (i > i0) {
                if (s.style == ChartStyle::Area)
                    dl->AddQuadFilled(ImVec2(prev_x, prev_y), ImVec2(cx, cy), ImVec2(cx, y1),
                                      ImVec2(prev_x, y1), fill);
                dl->AddLine(ImVec2(prev_x, prev_y), ImVec2(cx, cy), line, 1.6F);
            }
            prev_x = cx;
            prev_y = cy;
        }
    } else {
        for (size_t i = i0; i <= i1; ++i) {
            const pc_candle& k = in.candles[i];
            const bool up = k.c >= k.o;
            const ImU32 c = up ? bull : bear;
            const float cx = tx.x(static_cast<double>(i));
            const float wick_x = std::floor(cx);
            const float y_hi = tx.y(to_px(k.h));
            const float y_lo = tx.y(to_px(k.l));
            const float y_o = std::round(tx.y(to_px(k.o)));
            const float y_c = std::round(tx.y(to_px(k.c)));
            const float left = std::round(cx - half_w);
            const float right = left + body_w;

            if (s.style == ChartStyle::Bars) {
                dl->AddRectFilled(ImVec2(wick_x, y_hi), ImVec2(wick_x + 1.0F, y_lo), c);
                dl->AddRectFilled(ImVec2(left, y_o), ImVec2(wick_x, y_o + 1.0F), c);
                dl->AddRectFilled(ImVec2(wick_x + 1.0F, y_c), ImVec2(right, y_c + 1.0F), c);
                continue;
            }

            dl->AddRectFilled(ImVec2(wick_x, y_hi), ImVec2(wick_x + 1.0F, y_lo), c);
            const float top = std::min(y_o, y_c);
            const float bottom = std::max(y_o, y_c);
            // A doji has no body to fill; a hairline keeps it on the chart as a bar rather than
            // as a gap in the row.
            if (bottom - top < 1.0F)
                dl->AddRectFilled(ImVec2(left, top), ImVec2(right, top + 1.0F), c);
            else if (s.style == ChartStyle::Hollow && up)
                dl->AddRect(ImVec2(left, top), ImVec2(right, bottom), c);
            else
                dl->AddRectFilled(ImVec2(left, top), ImVec2(right, bottom), c);
        }
    }
    dl->PopClipRect();

    // --- levels ---------------------------------------------------------------------------
    constexpr int kMaxLevels = 32;
    int order[kMaxLevels];
    const int level_count = static_cast<int>(std::min<size_t>(in.level_count, kMaxLevels));
    for (int i = 0; i < level_count; ++i)
        order[i] = i;
    std::stable_sort(order, order + level_count,
                     [&](int a, int b) { return in.levels[a].rank > in.levels[b].rank; });

    TagSlot taken[kMaxLevels + 3];
    int taken_count = 0;
    const float tag_half = text_h * 0.5F + 3.0F;
    // The crosshair's slot is reserved first: it is the label the mouse is actively asking for,
    // so nothing else may paint over it.
    if (crosshair)
        taken[taken_count++] = TagSlot{io.MousePos.y, tag_half};

    for (int k = 0; k < level_count; ++k) {
        const ChartLevel& level = in.levels[order[k]];
        if (level.price <= 0)
            continue;
        const float y = tx.y(to_px(level.price));
        const int arrow = y < y0 ? -1 : (y > y1 ? 1 : 0);

        if (arrow == 0) {
            // The name chip sits at the rule's right-hand end, against the gutter, so a stack
            // of resting orders reads as a column next to the axis instead of as text scattered
            // over the candles.
            const ImVec2 size = ImGui::CalcTextSize(level.label);
            const float chip_right = x1 - 4.0F;
            const float chip_left = chip_right - size.x - 10.0F;
            const float line_from = std::max(x0, x1 - plot_w * std::clamp(level.span, 0.05F, 1.0F));
            dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
            dashed_h(dl, line_from, chip_left - 4.0F, y, col_of(level.color, 0.7F), level.thickness,
                     6.0F, 6.0F);
            if (level.label[0] != '\0') {
                dl->AddRectFilled(ImVec2(chip_left, std::round(y - tag_half)),
                                  ImVec2(chip_right, std::round(y + tag_half)), col_of(level.color),
                                  2.0F);
                dl->AddText(ImVec2(chip_left + 5.0F, std::round(y - size.y * 0.5F)),
                            contrast_text(level.color), level.label);
            }
            dl->PopClipRect();
        }

        const float tag_y = std::clamp(y, y0 + tag_half + 1.0F, y1 - tag_half - 1.0F);
        if (!slot_free(taken, taken_count, tag_y, tag_half))
            continue;
        taken[taken_count++] = TagSlot{tag_y, tag_half};
        char price_text[32];
        format_px(level.price, in.sz_decimals, price_text, sizeof(price_text));
        gutter_tag(dl, x1, x1 + gutter_w, tag_y, price_text, col_of(level.color),
                   contrast_text(level.color), arrow);
    }

    // --- axes -----------------------------------------------------------------------------
    dl->AddLine(ImVec2(snap(x1), y0), ImVec2(snap(x1), y1 + strip_h), kGrid);
    dl->AddLine(ImVec2(x0, snap(y1)), ImVec2(x1 + gutter_w, snap(y1)), kGrid);
    for (int i = 0; i < tticks.count; ++i) {
        const TimeTick& tick = tticks.ticks[i];
        const ImVec2 size = ImGui::CalcTextSize(tick.label);
        const float left = tx.x(static_cast<double>(tick.index)) - size.x * 0.5F;
        if (left < x0 || left + size.x > x1)
            continue;
        dl->AddText(
            ImVec2(std::round(left), y1 + 5.0F),
            tick.weight >= major && major != TimeTickWeight::Minor ? kAxisTextMajor : kAxisText,
            tick.label);
    }

    // --- last price -------------------------------------------------------------------
    if (newest.c > 0) {
        const float y = tx.y(to_px(newest.c));
        const ImVec4 tint = newest.c >= newest.o ? kCandleUp : kCandleDown;
        if (y >= y0 && y <= y1) {
            dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
            dashed_h(dl, x0, x1, y, col_of(tint, 0.8F), 1.0F, 2.0F, 2.0F);
            dl->PopClipRect();
        }
        const float tag_y = std::clamp(y, y0 + tag_half + 1.0F, y1 - tag_half - 1.0F);
        if (slot_free(taken, taken_count, tag_y, tag_half)) {
            taken[taken_count++] = TagSlot{tag_y, tag_half};
            char label[48];
            format_px(newest.c, in.sz_decimals, label, sizeof(label));
            gutter_tag(dl, x1, x1 + gutter_w, tag_y, label, col_of(tint), contrast_text(tint),
                       y < y0 ? -1 : (y > y1 ? 1 : 0));
            // Time left in the bar, under its price, on a neutral chip so it reads as separate
            // from the price rather than as part of it. A bar about to close is the difference
            // between a signal and a wick that has not happened yet.
            const uint64_t left = bar_seconds_left(newest.open_ms, in.interval_ms, in.now_ms);
            if (left > 0) {
                char countdown[16];
                if (left >= 3600)
                    std::snprintf(countdown, sizeof(countdown), "%llu:%02llu:%02llu",
                                  static_cast<unsigned long long>(left / 3600),
                                  static_cast<unsigned long long>((left % 3600) / 60),
                                  static_cast<unsigned long long>(left % 60));
                else
                    std::snprintf(countdown, sizeof(countdown), "%llu:%02llu",
                                  static_cast<unsigned long long>(left / 60),
                                  static_cast<unsigned long long>(left % 60));
                const float cy = tag_y + tag_half * 2.0F + 2.0F;
                if (cy + tag_half < y1) {
                    gutter_tag(dl, x1, x1 + gutter_w, cy, countdown, kChipBg, col_of(tint));
                    taken[taken_count++] = TagSlot{cy, tag_half};
                }
            }
        }
    }

    // --- crosshair --------------------------------------------------------------------
    if (crosshair) {
        const float cx = hover_index >= 0 ? tx.x(static_cast<double>(hover_index)) : io.MousePos.x;
        dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
        dashed_v(dl, y0, y1, cx, kCrosshair, 1.0F, 6.0F, 6.0F);
        dashed_h(dl, x0, x1, io.MousePos.y, kCrosshair, 1.0F, 6.0F, 6.0F);
        dl->PopClipRect();

        char label[48];
        const Px price =
            static_cast<Px>(std::llround(tx.price_at(io.MousePos.y) * static_cast<double>(kScale)));
        format_px(price, in.sz_decimals, label, sizeof(label));
        gutter_tag(dl, x1, x1 + gutter_w, io.MousePos.y, label, kChipBg, kChipText);

        if (hover_index >= 0) {
            char stamp[32];
            format_bar_time(in.candles[hover_index].open_ms, in.interval_ms, stamp, sizeof(stamp));
            const ImVec2 size = ImGui::CalcTextSize(stamp);
            const float bx = std::clamp(cx - size.x * 0.5F - 6.0F, x0, x1 - size.x - 12.0F);
            dl->AddRectFilled(ImVec2(bx, y1 + 1.0F), ImVec2(bx + size.x + 12.0F, y1 + strip_h),
                              kChipBg, 2.0F);
            dl->AddText(ImVec2(bx + 6.0F, y1 + 5.0F), kChipText, stamp);
        }
    }

    // --- the ruler ----------------------------------------------------------------------
    // Drawn after the crosshair so its readout is never crossed by it. The box is translucent,
    // so the candles and the crosshair still show through what is being measured.
    if (ruler.shown && ruler.from_price > 0 && ruler.to_price > 0) {
        const float ax = tx.x(static_cast<double>(ruler.from_index));
        const float bx = tx.x(static_cast<double>(ruler.to_index));
        const float ay = tx.y(to_px(ruler.from_price));
        const float by = tx.y(to_px(ruler.to_price));
        const float lx = std::min(ax, bx);
        const float rx = std::max(ax, bx);
        const float ty = std::min(ay, by);
        const float bottom = std::max(ay, by);
        if (rx - lx >= 2.0F || bottom - ty >= 2.0F) {
            dl->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
            dl->AddRectFilled(ImVec2(lx, ty), ImVec2(rx, bottom), kMeasureFill);
            dl->AddRect(ImVec2(lx, ty), ImVec2(rx, bottom), kMeasureEdge);
            // One arrow per axis, each running from the anchor to the end, so the direction of
            // the move is readable without parsing the signs in the box above.
            arrow(dl, ImVec2((lx + rx) * 0.5F, ay), ImVec2((lx + rx) * 0.5F, by), kMeasureLine);
            arrow(dl, ImVec2(ax, (ty + bottom) * 0.5F), ImVec2(bx, (ty + bottom) * 0.5F),
                  kMeasureLine);

            const Px delta = ruler.to_price - ruler.from_price;
            char delta_buf[32];
            char pct_buf[24];
            char tick_buf[24];
            format_px(delta, in.sz_decimals, delta_buf, sizeof(delta_buf));
            format_pct(ruler.from_price != 0 ? static_cast<int64_t>(static_cast<__int128>(delta) *
                                                                    kScale / ruler.from_price)
                                             : 0,
                       2, pct_buf, sizeof(pct_buf));
            // In ticks as well as in dollars: a move is sized against the instrument's own
            // increment as often as against its price.
            Px tick = kScale;
            for (int i = 0; i < max_dp; ++i)
                tick /= 10;
            format_count(tick > 0 ? delta / tick : 0, tick_buf, sizeof(tick_buf));

            const long bars = ruler.to_index - ruler.from_index;
            char span_buf[24];
            format_duration(static_cast<uint64_t>(bars < 0 ? -bars : bars) * in.interval_ms,
                            span_buf, sizeof(span_buf));

            double volume = 0.0;
            const long lo_i = std::max(0L, std::min(ruler.from_index, ruler.to_index));
            const long hi_i =
                std::min(static_cast<long>(n) - 1, std::max(ruler.from_index, ruler.to_index));
            for (long i = lo_i; i <= hi_i; ++i)
                volume += to_px(in.candles[i].v);
            char vol_buf[24];
            format_compact(volume, vol_buf, sizeof(vol_buf));

            char lines[3][96];
            std::snprintf(lines[0], sizeof(lines[0]), "%s (%s) %s", delta_buf, pct_buf, tick_buf);
            std::snprintf(lines[1], sizeof(lines[1]), "%ld bars, %s%s", bars, bars < 0 ? "-" : "",
                          span_buf);
            std::snprintf(lines[2], sizeof(lines[2]), "Vol %s", vol_buf);

            float text_w = 0.0F;
            for (const auto& line : lines)
                text_w = std::max(text_w, ImGui::CalcTextSize(line).x);
            constexpr float kPadX = 12.0F;
            constexpr float kPadY = 7.0F;
            const float box_w = text_w + kPadX * 2.0F;
            const float box_h = text_h * 3.0F + 4.0F * 2.0F + kPadY * 2.0F;
            const float box_x = std::clamp((lx + rx) * 0.5F - box_w * 0.5F, x0 + 4.0F,
                                           std::max(x0 + 4.0F, x1 - box_w - 4.0F));
            // Above the box by preference, below it when the measurement runs off the top --
            // the readout must not leave the pane, whichever way the drag went.
            float box_y = ty - box_h - 8.0F;
            if (box_y < y0 + 2.0F)
                box_y = std::min(bottom + 8.0F, y1 - box_h - 2.0F);
            box_y = std::clamp(box_y, y0 + 2.0F, std::max(y0 + 2.0F, y1 - box_h - 2.0F));

            dl->AddRectFilled(ImVec2(box_x, box_y), ImVec2(box_x + box_w, box_y + box_h),
                              kMeasureLine, 5.0F);
            for (int i = 0; i < 3; ++i) {
                const float w = ImGui::CalcTextSize(lines[i]).x;
                dl->AddText(ImVec2(std::round(box_x + (box_w - w) * 0.5F),
                                   std::round(box_y + kPadY + (text_h + 4.0F) * i)),
                            IM_COL32_WHITE, lines[i]);
            }
            dl->PopClipRect();
        }
    }

    // --- legend ----------------------------------------------------------------------
    {
        const size_t li = hover_index >= 0 ? static_cast<size_t>(hover_index) : n - 1;
        const pc_candle& k = in.candles[li];
        const ImU32 tint = k.c >= k.o ? bull : bear;
        char o[32], h[32], l[32], c[32];
        format_px(k.o, in.sz_decimals, o, sizeof(o));
        format_px(k.h, in.sz_decimals, h, sizeof(h));
        format_px(k.l, in.sz_decimals, l, sizeof(l));
        format_px(k.c, in.sz_decimals, c, sizeof(c));

        char head[96];
        std::snprintf(head, sizeof(head), "%s  %s", in.symbol ? in.symbol : "",
                      in.interval_label ? in.interval_label : "");
        const float ly = y0 + 7.0F;
        dl->AddText(ImVec2(x0 + 10.0F, ly), col_of(kColorTextMuted), head);

        const float ly2 = ly + text_h + 2.0F;
        const ImU32 muted = col_of(kColorTextMuted);
        const char* keys[4] = {"O", "H", "L", "C"};
        const char* vals[4] = {o, h, l, c};
        float lx = x0 + 10.0F;
        for (int i = 0; i < 4; ++i) {
            dl->AddText(ImVec2(lx, ly2), muted, keys[i]);
            lx += ImGui::CalcTextSize(keys[i]).x + 4.0F;
            dl->AddText(ImVec2(lx, ly2), tint, vals[i]);
            lx += ImGui::CalcTextSize(vals[i]).x + 10.0F;
        }
        // Change against the PREVIOUS close, not this bar's open: that is the number the venue
        // and every other terminal quote, and on a gapped open the two differ.
        const Px prev_close = li > 0 ? in.candles[li - 1].c : k.o;
        if (prev_close > 0) {
            char change[32];
            format_pct(static_cast<int64_t>((static_cast<__int128>(k.c - prev_close) * kScale) /
                                            prev_close),
                       2, change, sizeof(change));
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%s%s", k.c >= prev_close ? "+" : "", change);
            const ImU32 change_tint = k.c >= prev_close ? bull : bear;
            if (lx < x1 - 80.0F) {
                dl->AddText(ImVec2(lx, ly2), change_tint, buf);
                lx += ImGui::CalcTextSize(buf).x + 12.0F;
            }
        }
        char vol[24];
        format_compact(to_px(k.v), vol, sizeof(vol));
        if (lx < x1 - 90.0F) {
            dl->AddText(ImVec2(lx, ly2), muted, "V");
            dl->AddText(ImVec2(lx + ImGui::CalcTextSize("V").x + 4.0F, ly2), tint, vol);
        }
    }

    // --- jump to the live edge --------------------------------------------------------
    if (std::abs(s.right_offset - kDefaultRightOffset) > 1.0) {
        constexpr float kSize = 26.0F;
        const ImVec2 pos(x1 - kSize - 12.0F, y1 - kSize - 12.0F);
        ImGui::SetCursorScreenPos(pos);
        const bool pressed = ImGui::Button("##chart_realtime", ImVec2(kSize, kSize));
        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 centre(pos.x + kSize * 0.5F, pos.y + kSize * 0.5F);
        dl->AddCircleFilled(centre, kSize * 0.5F, IM_COL32(20, 34, 40, 235));
        dl->AddCircle(centre, kSize * 0.5F, hovered ? col_of(kColorAccent) : kGrid);
        const ImU32 glyph = hovered ? col_of(kColorAccent) : col_of(kColorTextMuted);
        dl->AddTriangleFilled(ImVec2(centre.x - 5.0F, centre.y - 5.0F),
                              ImVec2(centre.x - 5.0F, centre.y + 5.0F),
                              ImVec2(centre.x + 2.0F, centre.y), glyph);
        dl->AddRectFilled(ImVec2(centre.x + 3.0F, centre.y - 5.0F),
                          ImVec2(centre.x + 5.0F, centre.y + 5.0F), glyph);
        if (hovered)
            ImGui::SetTooltip("jump to the live edge");
        if (pressed) {
            s.scroll_from = s.right_offset;
            s.scroll_to = kDefaultRightOffset;
            s.scroll_t0 = ImGui::GetTime();
        }
    }

    // --- price axis labels ------------------------------------------------------------
    // Drawn last and only where nothing has claimed the strip. A gridline's own label sitting
    // half-under the live price, a liquidation tag or the crosshair is worse than no label at
    // all: the reader has to work out which of two overlapping numbers is the one they wanted.
    for (int i = 0; i < pticks.count; ++i) {
        const float y = tx.y(to_px(pticks.values[i]));
        if (y < y0 + 2.0F || y > y1 - 2.0F || !slot_free(taken, taken_count, y, tag_half))
            continue;
        char label[48];
        format_px_decimals(pticks.values[i], pticks.decimals, label, sizeof(label));
        dl->AddText(ImVec2(x1 + 8.0F, std::round(y - text_h * 0.5F)), kAxisText, label);
    }

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(avail);
}

}  // namespace pc::ui
