#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdio>

#include "core/units.hpp"
#include "md/l2_book.hpp"
#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/depth_bar.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// Ladder depth per side. A fixed row count (rather than "however many levels the feed sent")
// is what makes this read as a ladder: the spread sits at a stable screen position and rows
// stay put frame to frame, so the eye tracks size changes instead of re-finding the touch.
constexpr int kDisplayLevels = 12;
static_assert(kDisplayLevels <= PC_MAX_LEVELS);

// --- price-interval aggregation ---------------------------------------------------------
//
// The dropdown offers concrete price steps -- 1, 2, 5, 10, 100, 1000 for BTC at ~$78k --
// because a trader thinks in ticks, not in mantissa width. The steps adapt to the instrument's
// price: the base step is the venue's own 5-significant-figure granularity at the current mid,
// so a $0.42 coin gets 0.00001 / 0.00002 / 0.00005 / ... instead.
//
// Aggregation is done by the VENUE, not here. `l2Book` returns 20 levels whatever the
// granularity, so subscribing at a coarser step is what actually fills a deep ladder --
// bucketing the native-granularity levels client-side could only ever produce as many rows as
// the ~$19 those 20 levels span on BTC allowed, which is where the half-empty ladder came
// from. Measured on mainnet BTC:
//
//     nSigFigs=null           20 levels, $1 step
//     nSigFigs=5 mantissa=2   20 levels, $2 step
//     nSigFigs=5 mantissa=5   20 levels, $5 step
//     nSigFigs=4              20 levels, $10 step
//     nSigFigs=3              20 levels, $100 step
//     nSigFigs=2              20 levels, $1000 step
//
// Changing it is therefore a subscription change, which panels may not make directly (docs/02
// §7) -- it goes out as UiCommand::SetBookAggregation and the engine re-subscribes. The venue
// parameters are significant-figure based, so this one table is correct for every instrument:
// the multiplier is relative to whatever the asset's native step happens to be.
constexpr int kAggCount = 6;
constexpr int64_t kAggMultipliers[kAggCount] = {1, 2, 5, 10, 100, 1000};
constexpr int8_t kAggSigFigs[kAggCount] = {-1, 5, 5, 4, 3, 2};
constexpr uint8_t kAggMantissa[kAggCount] = {0, 2, 5, 0, 0, 0};

// Smallest sensible price step for an instrument trading at `ref_px`: 5 significant figures,
// i.e. 10^(floor(log10(price)) - 4), floored at one unit of the fixed-point scale. All integer
// arithmetic -- no double anywhere, same discipline as widgets/number_fmt. Used only to label
// the dropdown and to space the grid-continuation rows; the levels themselves arrive already
// bucketed from the venue.
Px base_step(Px ref_px) noexcept {
    if (ref_px <= 0)
        return 1;
    int digits = 0;  // digits before the decimal point, 0 if price < 1
    for (Px x = ref_px / kScale; x > 0; x /= 10)
        ++digits;
    // Target exponent relative to 1.0: (digits - 1) - 4 for price >= 1. For sub-1 prices,
    // count the leading zeros after the point instead.
    int exp = digits - 5;
    if (digits == 0) {
        int lead = 0;
        for (Px x = ref_px * 10; x < kScale && lead < 12; x *= 10)
            ++lead;
        exp = -lead - 5;
    }
    Px step = kScale;
    for (int i = 0; i < exp; ++i)
        step *= 10;
    for (int i = 0; i > exp; --i)
        step /= 10;
    return step > 0 ? step : 1;
}

// Human label for a price step, e.g. "1", "0.00005", "1000". Integer digit manipulation only
// (no double), same discipline as widgets/number_fmt: a label that disagreed with the step it
// names would be worse than no label at all.
void format_step(Px step, char* out, size_t cap) noexcept {
    const int64_t whole = step / kScale;
    int64_t frac = step % kScale;
    if (frac == 0) {
        std::snprintf(out, cap, "%lld", static_cast<long long>(whole));
        return;
    }
    char frac_buf[9];
    for (int i = 7; i >= 0; --i) {
        frac_buf[i] = static_cast<char>('0' + frac % 10);
        frac /= 10;
    }
    frac_buf[8] = '\0';
    int last = 7;
    while (last > 0 && frac_buf[last] == '0')
        --last;
    frac_buf[last + 1] = '\0';
    std::snprintf(out, cap, "%lld.%s", static_cast<long long>(whole), frac_buf);
}

struct Row {
    Px px{};
    Qty sz{};
    Qty cum{};  // cumulative size from the spread outward
};

// Copies the venue's already-aggregated levels into display rows, capped at kDisplayLevels.
int take_levels(const md::Level* levels, uint8_t count, Row* out) noexcept {
    const int n = std::min(static_cast<int>(count), kDisplayLevels);
    for (int i = 0; i < n; ++i) {
        out[i].px = levels[i].px;
        out[i].sz = levels[i].sz;
    }
    return n;
}

}  // namespace

void draw_book(PanelContext& ctx) {
    if (ImGui::Begin(kWindowOrderBook)) {
        if (ctx.market == nullptr || !ctx.instrument.valid()) {
            ImGui::TextDisabled("waiting for market data...");
            ImGui::End();
            return;
        }

        // ctx.instrument.book is a plain-value copy already pulled across the seqlock this
        // frame (docs/05 §5.1) -- reading it here can never tear or flicker regardless of how
        // fast the engine thread republishes.
        const md::L2Book& book = ctx.instrument.book;

        // Reference price for the step ladder: the mid if we have two sides, else whatever
        // touch exists, else the mark. Steps must not flicker as the touch moves, so this only
        // ever changes the *magnitude*, which is stable within an instrument.
        Px ref = 0;
        if (book.bid_count() > 0 && book.ask_count() > 0)
            ref = book.mid();
        else if (book.bid_count() > 0)
            ref = book.bids()[0].px;
        else if (book.ask_count() > 0)
            ref = book.asks()[0].px;
        if (ref <= 0)
            ref = ctx.instrument.ctx.mark_px();

        const Px base = base_step(ref);
        const int agg = std::clamp(ctx.view.book_aggregation, 0, kAggCount - 1);
        const Px step = base * kAggMultipliers[agg];

        char step_buf[32];
        format_step(step, step_buf, sizeof(step_buf));
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##agg", step_buf)) {
            char item_buf[32];
            for (int i = 0; i < kAggCount; ++i) {
                format_step(base * kAggMultipliers[i], item_buf, sizeof(item_buf));
                const bool selected = ctx.view.book_aggregation == i;
                ImGui::PushID(i);
                if (ImGui::Selectable(item_buf, selected) && i != ctx.view.book_aggregation) {
                    ctx.view.book_aggregation = i;
                    // The venue does the aggregating: re-subscribe both l2Book streams at the
                    // matching granularity (see the kAggSigFigs table above).
                    app::UiCommand command{};
                    command.kind = app::UiCommandKind::SetBookAggregation;
                    command.asset = ctx.instrument.asset;
                    command.n_sig_figs = kAggSigFigs[i];
                    command.mantissa = kAggMantissa[i];
                    (void)ctx.bridge.push_command(command);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }

        // Size/Total denomination. The coin name comes from the universe rather than being
        // hardcoded, so the label always matches whatever instrument is selected.
        const char* coin = "COIN";
        for (uint32_t i = 0; i < ctx.universe.count; ++i) {
            if (ctx.universe.assets[i].asset == ctx.instrument.asset) {
                coin = ctx.universe.assets[i].name;
                break;
            }
        }
        const bool in_usd = ctx.view.book_size_in_usd;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80);
        if (ImGui::BeginCombo("##denom", in_usd ? "USDC" : coin)) {
            if (ImGui::Selectable("USDC", in_usd))
                ctx.view.book_size_in_usd = true;
            if (ImGui::Selectable(coin, !in_usd))
                ctx.view.book_size_in_usd = false;
            ImGui::EndCombo();
        }

        // Column header, so the ladder's three columns are named once instead of implied.
        const float w = ImGui::GetContentRegionAvail().x;
        // USDC notionals are wider strings than coin sizes ("$1,234,567.89" vs "1.23456"), so
        // the two value columns start earlier in that mode to keep them from colliding.
        const float col_sz = w * (in_usd ? 0.34F : 0.42F);
        const float col_total = w * (in_usd ? 0.66F : 0.72F);
        ImGui::TextColored(kColorTextMuted, "Price");
        ImGui::SameLine(col_sz);
        ImGui::TextColored(kColorTextMuted, "Size (%s)", in_usd ? "USDC" : coin);
        ImGui::SameLine(col_total);
        ImGui::TextColored(kColorTextMuted, "Total (%s)", in_usd ? "USDC" : coin);
        ImGui::Separator();

        Row asks[kDisplayLevels]{};
        Row bids[kDisplayLevels]{};
        const int n_asks = take_levels(book.asks().data(), book.ask_count(), asks);
        const int n_bids = take_levels(book.bids().data(), book.bid_count(), bids);

        // Cumulative depth from the spread outward, and the normalisation max shared across
        // BOTH sides' *visible* rows only -- not the whole book -- so a thin side still fills
        // legibly instead of being dwarfed by a deep level that isn't even on screen
        // (docs/02 §7).
        Qty max_cum = 1;  // avoid div-by-zero when a side is empty
        Qty running = 0;
        for (int i = 0; i < n_asks; ++i) {
            running += asks[i].sz;
            asks[i].cum = running;
            max_cum = std::max(max_cum, running);
        }
        running = 0;
        for (int i = 0; i < n_bids; ++i) {
            running += bids[i].sz;
            bids[i].cum = running;
            max_cum = std::max(max_cum, running);
        }

        char px_buf[32];
        char sz_buf[32];
        char cum_buf[32];
        const float row_h = ImGui::GetTextLineHeightWithSpacing();

        // Draws one ladder row: a depth bar (bucket cumulative / max_cum) behind
        // price/size/total text, and an invisible full-row button on top so a click anywhere
        // on the row picks the price -- docs/02 §7's "single most-used interaction in any
        // trading UI". Consumed by panel_ticket via ViewState::price_pick_pending/picked_px.
        // `empty` rows keep the ladder at a fixed height when the feed is thin.
        auto draw_row = [&](const Row& row, bool is_ask, bool empty) {
            ImGui::PushID(is_ask ? "ask" : "bid");
            ImGui::PushID(static_cast<int>(row.px % 1000000007));

            const ImVec2 row_min = ImGui::GetCursorScreenPos();
            const ImVec2 row_max(row_min.x + ImGui::GetContentRegionAvail().x, row_min.y + row_h);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (!empty) {
                const float frac =
                    static_cast<float>(static_cast<double>(row.cum) / static_cast<double>(max_cum));
                // Depth grows away from the spread on both sides: asks stack downward toward
                // it from above, bids stack upward toward it from below, so both bars anchor
                // to the same edge (right) in this single-column layout.
                draw_depth_bar(dl, row_min, row_max, frac,
                               ImGui::GetColorU32(is_ask ? kColorAskMuted : kColorBidMuted),
                               DepthBarAnchor::Right);
            }

            if (empty) {
                // Grid continuation row: a real, clickable price on the step ladder that the
                // feed simply has no resting size at. Rendering it muted (rather than blank)
                // keeps the ladder a fixed 12 rows per side and still lets a click pick the
                // price, which is the whole point of a ladder.
                format_px(row.px, ctx.sz_decimals, px_buf, sizeof(px_buf));
                ImGui::TextColored(kColorTextMuted, "%s", px_buf);
            } else {
                format_px(row.px, ctx.sz_decimals, px_buf, sizeof(px_buf));
                if (in_usd) {
                    // Notional at this row's own price -- the depth's dollar value, which is
                    // what the USDC view is actually asking for. Cumulative rows are only an
                    // approximation of true notional depth (each level's size priced at *its*
                    // own price would need a running sum), so `cum` is priced at this row too;
                    // it is a display aid, not a fill estimate.
                    format_usd(notional(row.px, row.sz), sz_buf, sizeof(sz_buf));
                    format_usd(notional(row.px, row.cum), cum_buf, sizeof(cum_buf));
                } else {
                    format_qty(row.sz, ctx.sz_decimals, sz_buf, sizeof(sz_buf));
                    format_qty(row.cum, ctx.sz_decimals, cum_buf, sizeof(cum_buf));
                }
                ImGui::TextColored(is_ask ? kColorAsk : kColorBid, "%s", px_buf);
                ImGui::SameLine(col_sz);
                ImGui::TextColored(kColorTextPrimary, "%s", sz_buf);
                ImGui::SameLine(col_total);
                ImGui::TextColored(kColorTextMuted, "%s", cum_buf);
            }

            ImGui::SetCursorScreenPos(row_min);
            if (ImGui::InvisibleButton("row", ImVec2(row_max.x - row_min.x, row_h))) {
                ctx.view.price_pick_pending = true;
                ctx.view.picked_px = row.px;
            }
            ImGui::PopID();
            ImGui::PopID();
        };

        // The venue returns 20 levels per side at the selected granularity, so kDisplayLevels
        // rows are normally all real. These grid-continuation rows only appear on a genuinely
        // thin book, or in the moment right after a granularity change while the new
        // subscription's first snapshot is still in flight -- the ladder keeps a fixed height
        // and every row stays a pickable price.
        const Px ask_edge = n_asks > 0 ? asks[n_asks - 1].px
                                       : ((ref / step) * step + step);
        const Px bid_edge = n_bids > 0 ? bids[n_bids - 1].px : (ref / step) * step;

        // Asks descending: farthest (worst) ask at the top, best ask directly above the
        // spread. Grid rows go above the deepest ask so the spread stays anchored.
        for (int i = kDisplayLevels - 1; i >= n_asks; --i)
            draw_row(Row{ask_edge + step * (i - n_asks + 1), 0, 0}, /*is_ask=*/true,
                     /*empty=*/true);
        for (int i = n_asks - 1; i >= 0; --i)
            draw_row(asks[i], /*is_ask=*/true, /*empty=*/false);

        // Spread row.
        ImGui::Separator();
        if (book.bid_count() > 0 && book.ask_count() > 0) {
            format_px(book.spread(), ctx.sz_decimals, px_buf, sizeof(px_buf));
            char mid_buf[32];
            format_px(book.mid(), ctx.sz_decimals, mid_buf, sizeof(mid_buf));
            ImGui::TextColored(kColorTextPrimary, "%s", mid_buf);
            ImGui::SameLine(col_sz);
            ImGui::TextColored(kColorTextMuted, "spread %s", px_buf);
        } else {
            ImGui::TextDisabled("spread unavailable");
        }
        ImGui::Separator();

        // Bids descending: best bid directly below the spread, worst bid at the bottom.
        for (int i = 0; i < n_bids; ++i)
            draw_row(bids[i], /*is_ask=*/false, /*empty=*/false);
        for (int i = n_bids; i < kDisplayLevels; ++i)
            draw_row(Row{bid_edge - step * (i - n_bids + 1), 0, 0}, /*is_ask=*/false,
                     /*empty=*/true);
    }
    ImGui::End();
}

}  // namespace pc::ui
