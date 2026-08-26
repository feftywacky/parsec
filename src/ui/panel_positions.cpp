#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "exec/rounder.hpp"
#include "portfolio/order_preview.hpp"
#include "portfolio/pnl.hpp"
#include "ui/app_window.hpp"
#include "ui/asset_lookup.hpp"
#include "ui/cloid.hpp"
#include "ui/panels.hpp"
#include "ui/position_levels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// Everything one row needs that is not already in pc_position, resolved once per row so the
// table cells and the row's actions (close, TP/SL) can never disagree about the price, the
// precision or the coin they are talking about.
struct RowView {
    const char* coin{"--"};      // venue symbol; never a bare asset index (see resolve_row)
    uint8_t sz_decimals{};       // this row's asset, not the panel's active instrument
    uint32_t max_leverage{};     // for the maintenance-rate estimate
    Qty abs_size{};
    Px mark{};                   // live for the active instrument, else derived from the venue
    bool mark_is_live{};
    Usd unrealized{};
    int32_t roe_bps{};
    Px liq_px{};
    bool liq_is_estimate{};
};

Usd abs_usd(Usd value) noexcept {
    return value < 0 ? -value : value;
}

// A position's mark price. For the instrument on screen the live activeAssetCtx mark is a
// second or two fresher than the ~4s clearinghouseState snapshot the row itself came from, so
// it is preferred; every other row derives the mark the venue actually used from that row's
// own numbers (position_value = mark * |szi|), which keeps Mark, Value and Unrealized PnL
// internally consistent instead of mixing two vintages of the same number.
Px row_mark(const pc_position& p, Qty abs_size) noexcept {
    if (abs_size <= 0 || p.position_value == 0)
        return 0;
    return static_cast<Px>(static_cast<__int128>(abs_usd(p.position_value)) *
                           static_cast<__int128>(kScale) / abs_size);
}

RowView resolve_row(const PanelContext& ctx, const portfolio::Position& row) noexcept {
    const pc_position& p = row.value;
    RowView view{};
    view.abs_size = p.szi < 0 ? -p.szi : p.szi;

    // The asset id travels on portfolio::Position (the snapshot compacts PositionBook's sparse
    // table into a dense list, so the row has to carry its own id), and the venue's `meta`
    // universe is what turns it into a symbol and that coin's own precision.
    const AssetLabel label = lookup_asset(ctx.universe, row.asset, ctx.sz_decimals);
    view.coin = label.coin;
    view.sz_decimals = label.sz_decimals;
    view.max_leverage = label.max_leverage;

    view.mark = row_mark(p, view.abs_size);
    if (row.asset == ctx.instrument.asset && ctx.instrument.ctx.mark_px() > 0) {
        view.mark = ctx.instrument.ctx.mark_px();
        view.mark_is_live = true;
    }

    // Recomputed from the live mark rather than taken from the snapshot, so PnL ticks with the
    // market instead of stepping every ~4s (docs/02 §6.4: "recomputed from AssetCtx::mark on
    // every tick ... keeps the UI's unrealized P&L identical to the venue's definition").
    if (view.mark_is_live && p.entry_px > 0) {
        view.unrealized = portfolio::unrealized_pnl(p.szi, p.entry_px, view.mark);
        view.roe_bps = portfolio::roe_bps(view.unrealized, p.margin_used);
    } else {
        view.unrealized = p.unrealized_pnl;
        view.roe_bps = p.roe_bps;
    }

    view.liq_px = liquidation_px(p, ctx.portfolio, view.max_leverage, &view.liq_is_estimate);
    return view;
}

// How a TP/SL field's text is read: an absolute trigger price, or a percentage. Same contract
// as the ticket's own selector -- the percentage is a move of the POSITION, leverage included,
// so "SL 10%" on a 10x position fires after a 1% move in the coin. That is the number being
// risk-managed against; a percentage that quietly meant the coin's move would understate a
// levered position by exactly the leverage factor.
enum class TpSlUnit : uint8_t { Price, Pct };

const char* tpsl_unit_label(TpSlUnit unit) noexcept {
    return unit == TpSlUnit::Pct ? "%" : "price";
}

// A leg's size field, which is denominated either as a fraction of the position or in the coin
// itself. Percent is the default because that is how a scaled exit is actually thought about
// ("take half off at 2.10"), and it stays correct as the position changes size.
enum class SizeUnit : uint8_t { Pct, Coin };

const char* size_unit_label(SizeUnit unit, const char* coin) noexcept {
    return unit == SizeUnit::Pct ? "%" : coin;
}

// Resolves a size field against the position. An empty field means the whole position, so a
// leg that is only ever a full exit needs nothing typed into it.
Qty resolve_leg_size(const char* text, SizeUnit unit, Qty abs_size,
                     exec::AssetPrecision precision, const char** error) noexcept {
    *error = nullptr;
    if (!text || !*text)
        return abs_size;
    Qty value = 0;
    if (!parse_fixed(text, &value) || value <= 0) {
        *error = unit == SizeUnit::Pct ? "size percent must be positive"
                                       : "size must be positive";
        return 0;
    }
    Qty sz = value;
    if (unit == SizeUnit::Pct) {
        if (value > 100 * kScale) {
            *error = "size cannot exceed 100% of the position";
            return 0;
        }
        sz = static_cast<Qty>(static_cast<__int128>(abs_size) * value / (100 * kScale));
    } else if (sz > abs_size) {
        *error = "size is larger than the position";
        return 0;
    }
    sz = exec::Rounder::round_sz(sz, precision);
    if (sz <= 0) {
        *error = "size rounds to zero at this coin's precision";
        return 0;
    }
    return sz;
}

// Panel-local view state. Only the two modal workflows need to remember anything between
// frames, and both are keyed by the asset they were opened for so a snapshot arriving mid-
// interaction (positions are re-published every few seconds, and the row order can change)
// can never re-point an open dialog at a different coin.
struct PositionsPrefs {
    uint32_t tpsl_asset{PC_ASSET_NONE};
    char tp_buf[32]{};
    char sl_buf[32]{};
    // Per-field, because the two legs are naturally thought about differently ("take profit at
    // 2.10" but "never lose more than 20% of the margin").
    TpSlUnit tp_unit{TpSlUnit::Price};
    TpSlUnit sl_unit{TpSlUnit::Price};
    // How much of the position each leg closes. Blank means all of it, so the common
    // full-exit case needs nothing typed.
    char tp_sz_buf[32]{};
    char sl_sz_buf[32]{};
    SizeUnit tp_sz_unit{SizeUnit::Pct};
    SizeUnit sl_sz_unit{SizeUnit::Pct};

    char note[160]{};
    uint64_t note_ms{};
    bool note_error{};
};
PositionsPrefs g_prefs;

void set_note(const char* text, bool error, uint64_t now_ms) noexcept {
    std::snprintf(g_prefs.note, sizeof(g_prefs.note), "%s", text);
    g_prefs.note_error = error;
    g_prefs.note_ms = now_ms;
    event_store().note_local(text, error ? 2u : 0u);
}

// Sends a reduce-only IOC for the position's full size: the venue's own "close at market".
// Priced off the mark with the standard 5% slippage cap and rounded Aggressive, so it is
// guaranteed marketable but still cannot fill arbitrarily far from where the trader looked
// (exec::Rounder::marketable_px, docs/03 §6). reduce_only means it can only ever shrink the
// position -- a stale size cannot accidentally open the other way.
void submit_close(PanelContext& ctx, const portfolio::Position& row, const RowView& view) {
    if (row.value.szi == 0 || view.mark <= 0) {
        set_note("Close not sent: no mark price for this coin yet", true, ctx.now_ms);
        return;
    }
    const exec::AssetPrecision precision{view.sz_decimals};
    const Side side = row.value.szi > 0 ? Side::Sell : Side::Buy;
    const Qty sz = exec::Rounder::round_sz(view.abs_size, precision);
    if (sz <= 0) {
        set_note("Close not sent: position rounds to zero size", true, ctx.now_ms);
        return;
    }

    pc_order order{};
    order.asset = row.asset;
    order.is_buy = side == Side::Buy ? 1 : 0;
    order.reduce_only = 1;
    order.tif = PC_TIF_IOC;
    order.tpsl = PC_TPSL_NONE;
    order.limit_px = exec::Rounder::marketable_px(view.mark, side, precision);
    order.sz = sz;
    make_local_cloid(order.cloid, ctx.now_ms);

    app::UiCommand cmd{};
    cmd.kind = app::UiCommandKind::PlaceOrder;
    cmd.asset = row.asset;
    cmd.order.grouping = PC_GROUP_NA;
    cmd.order.n_orders = 1;
    cmd.order.orders[0] = order;

    char qty_buf[32], note[160];
    format_qty(sz, view.sz_decimals, qty_buf, sizeof(qty_buf));
    if (ctx.bridge.push_command(cmd)) {
        std::snprintf(note, sizeof(note), "Sent: close %s %s at market",
                      qty_buf, view.coin);
        set_note(note, false, ctx.now_ms);
    } else {
        set_note("Close not sent: command queue full -- try again", true, ctx.now_ms);
    }
}

// One resolved TP or SL leg for an existing position.
struct PositionLeg {
    bool present{};
    bool valid{};
    Px trigger{};
    Qty sz{};              // how much of the position this leg closes, in the base coin
    bool full_size{};      // true when `sz` covers the whole position
    Usd pnl{};             // realized P&L if the leg fills at its trigger, for `sz`; signed
    int64_t roe_1e8{};     // the same move as the POSITION sees it, kScale-scaled fraction
    const char* error{};
};

// Resolves a typed trigger against the position's entry price, in whichever unit the field is
// set to. Unlike the ticket, the entry here is a fact rather than a preview, so a percentage
// is measured against the price actually paid.
PositionLeg resolve_leg(const char* text, TpSlUnit unit, bool is_tp, const pc_position& p,
                        Qty abs_size, Qty leg_size, const char* size_error,
                        exec::AssetPrecision precision) noexcept {
    PositionLeg leg{};
    if (!text || !*text)
        return leg;
    leg.present = true;
    if (p.entry_px <= 0) {
        leg.error = "no entry price on this position yet";
        return leg;
    }

    const bool is_long = p.szi > 0;
    // A long takes profit above entry and stops out below; a short is the mirror. One
    // predicate drives both the sign applied to a percentage and the validity check on an
    // absolute price, so the two can never disagree about which side is which.
    const bool above_entry = (is_long == is_tp);
    const uint32_t lev = p.leverage > 0 ? p.leverage : 1;

    Px raw = 0;
    if (unit == TpSlUnit::Pct) {
        Px pct = 0;
        if (!parse_fixed(text, &pct) || pct <= 0) {
            leg.error = "percent must be a positive number";
            return leg;
        }
        // pct is a kScale-scaled percent; /100 makes it a fraction. The position moves
        // `leverage` times as far as the coin, so a 10% position move at 10x needs only a 1%
        // price move -- hence the extra division. One 128-bit expression so the leverage
        // division never rounds through an intermediate.
        const __int128 denominator = static_cast<__int128>(100) * kScale * lev;
        const __int128 delta = static_cast<__int128>(p.entry_px) * pct / denominator;
        raw = above_entry ? p.entry_px + static_cast<Px>(delta)
                          : p.entry_px - static_cast<Px>(delta);
        if (raw <= 0) {
            leg.error = "percent is larger than the whole position";
            return leg;
        }
    } else if (!parse_fixed(text, &raw) || raw <= 0) {
        leg.error = "trigger must be a positive price";
        return leg;
    }
    // The closing order sits on the opposite side of the position, so it is that side's
    // rounding that decides which tick the trigger lands on.
    const Side close_side = is_long ? Side::Sell : Side::Buy;
    leg.trigger = exec::Rounder::round_px(raw, close_side, precision);
    if (leg.trigger <= 0) {
        leg.error = "trigger rounds to zero at this tick size";
        return leg;
    }
    // Checked after rounding: rounding is what can push a trigger one tick onto the wrong side
    // of entry, and the venue rejects the whole group when it is.
    if (above_entry && leg.trigger <= p.entry_px) {
        leg.error = is_tp ? "take profit must be above the entry price"
                          : "stop loss must be above the entry price";
        return leg;
    }
    if (!above_entry && leg.trigger >= p.entry_px) {
        leg.error = is_tp ? "take profit must be below the entry price"
                          : "stop loss must be below the entry price";
        return leg;
    }

    if (size_error) {
        leg.error = size_error;
        return leg;
    }
    leg.sz = leg_size;
    leg.full_size = leg_size >= abs_size;
    // P&L for the slice this leg actually closes, not for the whole position -- a half-size
    // take-profit that quoted the full position's gain would overstate it by 2x.
    leg.pnl = is_long ? notional(leg.trigger - p.entry_px, leg.sz)
                      : notional(p.entry_px - leg.trigger, leg.sz);
    const int64_t px_move = static_cast<int64_t>(
        (static_cast<__int128>(leg.trigger - p.entry_px) * kScale) / p.entry_px);
    // What the move does to the POSITION, not to the coin: the margin behind it is only
    // 1/leverage of the notional, so a levered position moves that many times as far.
    leg.roe_1e8 = portfolio::roe_from_price_move(px_move, lev);
    if (!is_long)
        leg.roe_1e8 = -leg.roe_1e8;
    leg.valid = true;
    return leg;
}

// Attaches TP/SL to an existing position: reduce-only market triggers on the closing side.
//
// The grouping depends on the sizes. `positionTpsl` tells the venue these protect the POSITION
// -- it keeps their size in step as the position changes -- but that only makes sense when a
// leg actually covers the whole position. A partial leg ("take a third off at 2.10") is an
// ordinary standalone reduce-only trigger, so it goes ungrouped; sending it as positionTpsl
// would ask the venue to resize it back up to the full position behind the trader's back.
void submit_tpsl(PanelContext& ctx, const portfolio::Position& row, const RowView& view,
                 const PositionLeg& tp, const PositionLeg& sl) {
    const exec::AssetPrecision precision{view.sz_decimals};
    const bool is_long = row.value.szi > 0;
    const Side close_side = is_long ? Side::Sell : Side::Buy;

    const bool all_full = (!tp.valid || tp.full_size) && (!sl.valid || sl.full_size);

    pc_order_req req{};
    req.grouping = all_full ? PC_GROUP_POSITION_TPSL : PC_GROUP_NA;
    auto attach = [&](const PositionLeg& leg, uint8_t kind, uint64_t cloid_salt) {
        if (!leg.valid || leg.sz <= 0 || req.n_orders >= 4)
            return;
        pc_order child{};
        child.asset = row.asset;
        child.is_buy = close_side == Side::Buy ? 1 : 0;
        child.reduce_only = 1;
        child.tif = PC_TIF_GTC;
        child.tpsl = kind;
        child.trigger_px = leg.trigger;
        child.is_market_trigger = 1;
        // A market trigger still carries a limit price, and the venue uses it as the slippage
        // bound once the trigger fires. Setting it to the trigger itself (which is what a
        // naive reading suggests) makes the stop unfillable in exactly the fast market it
        // exists for, so it gets the same marketable padding a close order does.
        child.limit_px = exec::Rounder::marketable_px(leg.trigger, close_side, precision);
        child.sz = leg.sz;
        make_local_cloid(child.cloid, ctx.now_ms + cloid_salt);
        req.orders[req.n_orders++] = child;
    };
    attach(tp, PC_TPSL_TP, 1);
    attach(sl, PC_TPSL_SL, 2);
    if (req.n_orders == 0) {
        set_note("TP/SL not sent: nothing to place", true, ctx.now_ms);
        return;
    }

    app::UiCommand cmd{};
    cmd.kind = app::UiCommandKind::PlaceOrder;
    cmd.asset = row.asset;
    cmd.order = req;

    if (!ctx.bridge.push_command(cmd)) {
        set_note("TP/SL not sent: command queue full -- try again", true, ctx.now_ms);
        return;
    }

    char tp_part[48] = "";
    char sl_part[48] = "";
    char sz_buf[32];
    if (tp.valid) {
        format_qty(tp.sz, view.sz_decimals, sz_buf, sizeof(sz_buf));
        std::snprintf(tp_part, sizeof(tp_part), "take profit %s", sz_buf);
    }
    if (sl.valid) {
        format_qty(sl.sz, view.sz_decimals, sz_buf, sizeof(sz_buf));
        std::snprintf(sl_part, sizeof(sl_part), "%sstop loss %s", tp.valid ? " + " : "",
                      sz_buf);
    }
    char note[160];
    std::snprintf(note, sizeof(note), "Sent: %s%s on %s", tp_part, sl_part, view.coin);
    set_note(note, false, ctx.now_ms);
}

// Cancels one resting trigger order. Same command the Open Orders panel sends, issued from
// here so a scaled exit can be managed where it was built rather than in another tab.
void cancel_order(PanelContext& ctx, uint32_t asset, uint64_t oid) {
    app::UiCommand cmd{};
    cmd.kind = app::UiCommandKind::CancelOrder;
    cmd.asset = asset;
    cmd.oid = oid;
    if (ctx.bridge.push_command(cmd))
        set_note("Sent: cancel trigger", false, ctx.now_ms);
    else
        set_note("Cancel not sent: command queue full -- try again", true, ctx.now_ms);
}

// Header cell with an explanation attached. Several of these columns mean something narrower
// than their name suggests (realized P&L is session-scoped, funding is all-time), and a
// hovered tooltip is where that belongs -- putting it in the header text itself would make the
// table unreadable at the widths it actually gets docked at.
void header_with_tooltip(const char* label, const char* tooltip) {
    ImGui::TableHeader(label);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
}

}  // namespace

void draw_positions(PanelContext& ctx) {
    event_store().drain(ctx.bridge);  // see ui_event_store.hpp: safe to call from any panel

    if (!ImGui::Begin(kWindowPositions)) {
        ImGui::End();
        return;
    }

    // --- Account summary strip ---------------------------------------------------------
    Usd total_unrealized = 0;
    Usd total_margin = 0;
    for (uint32_t i = 0; i < ctx.portfolio.position_count; ++i) {
        const portfolio::Position& row = ctx.portfolio.positions[i];
        total_unrealized += resolve_row(ctx, row).unrealized;
        total_margin += row.value.margin_used;
    }
    const RealizedRow& realized_total = event_store().realized_total();

    // Funding belongs in the session realized total, not beside it. It is settled cash: the
    // venue moves it hourly against the margin balance, closing the position does not give it
    // back, and no mark-to-market can undo it -- which is the definition of realized. Left out,
    // the summary answers "what did my entries and exits earn" while appearing to answer "what
    // have I actually made", and cannot reconcile against account value, where
    //     account = deposits + trading P&L + funding + unrealized.
    //
    // The two components do NOT cover identical windows, and pretending otherwise would make
    // this number quietly wrong: `userFills` backfills the venue's most recent fills with no
    // time bound, while `userFunding` backfills a fixed 30 days (rust/src/ffi/fetch.rs). Both
    // then run forward on the live stream, and both deduplicate replays (by trade id / by
    // (asset, hour)), so neither double-counts -- but the funding leg can reach back further
    // than the trading leg on a quiet account, or less far on a busy one. That is a real limit
    // of what the venue serves, not something this panel can reconcile, so the tooltip states
    // both windows rather than the strip implying a single clean one.
    //
    // The per-position Funding COLUMN is a third figure again -- the venue's
    // cumFunding.allTime for that one position -- and must not be substituted here.
    const Usd funding_total = event_store().funding_total();
    const Usd session_realized = realized_total.pnl + funding_total;

    char buf[48];
    // The strip ends in a full-height "Close All" button, so every text run before it is
    // aligned to frame padding -- otherwise the numbers ride above the button's baseline.
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Unrealized");
    ImGui::SameLine();
    format_usd(total_unrealized, buf, sizeof(buf));
    ImGui::TextColored(total_unrealized >= 0 ? kColorBid : kColorAsk, "%s", buf);
    ImGui::SameLine();
    ImGui::TextDisabled("|  Realized");
    ImGui::SameLine();
    format_usd_fine(session_realized, buf, sizeof(buf));
    ImGui::TextColored(session_realized >= 0 ? kColorBid : kColorAsk, "%s", buf);
    if (ImGui::IsItemHovered()) {
        // Broken out rather than shown as one number: the total says how much money moved,
        // the split says whether it moved because of trade selection or because of carry,
        // and those call for completely different responses.
        char closed[32], fees[32], funding[32];
        format_usd_fine(realized_total.pnl + realized_total.fees, closed, sizeof(closed));
        format_usd_fine(-realized_total.fees, fees, sizeof(fees));
        format_usd_fine(funding_total, funding, sizeof(funding));
        ImGui::SetTooltip(
            "Cash actually settled -- everything except open marks.\n\n"
            "  closed P&L   %s   (over %u fills)\n"
            "  fees         %s\n"
            "  funding      %s\n"
            "  ---------------------------\n"
            "  total        %s\n\n"
            "Funding is included because it is settled cash: the venue moves it hourly\n"
            "against your margin, and closing the position does not give it back.\n\n"
            "The two legs cover different windows, which is a limit of what the venue\n"
            "serves: fills backfill the most recent ones with no time bound, funding\n"
            "backfills a fixed 30 days. Both run forward live from there.\n\n"
            "The per-position Funding column is a THIRD figure -- cumFunding.allTime for\n"
            "that one position -- so it will not tie out against the funding line here.",
            closed, realized_total.fills, fees, funding, buf);
    }
    // The funding component, spelled out on the strip rather than left to a hover. Fusing it
    // into the total is right (it is settled cash) but it made the total unverifiable at a
    // glance: there is no way to tell a total that includes funding from one that forgot to,
    // and "is my funding in there?" is exactly the question this number has to answer.
    if (funding_total != 0) {
        ImGui::SameLine(0.0F, 6.0F);
        char funding_buf[32];
        format_usd_fine(funding_total, funding_buf, sizeof(funding_buf));
        ImGui::TextDisabled("(incl. funding %s)", funding_buf);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|  Margin used");
    ImGui::SameLine();
    ImGui::TextUnformatted(format_usd(total_margin, buf, sizeof(buf)));

    if (ctx.portfolio.position_count > 0) {
        ImGui::SameLine();
        if (ImGui::Button("Close All"))
            ImGui::OpenPopup("confirm_close_all");
    }

    // Flattening every position at once is the one action here that cannot be walked back a
    // row at a time, so it confirms -- the same rule the kill switch's flatten follows.
    if (ImGui::BeginPopupModal("confirm_close_all", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Close all %u positions at market?", ctx.portfolio.position_count);
        ImGui::TextDisabled("Each is sent as a reduce-only IOC at up to %.1f%% slippage.",
                            static_cast<double>(exec::Rounder::kDefaultSlippageBps) / 100.0);
        ImGui::Separator();
        if (ImGui::Button("Close all")) {
            for (uint32_t i = 0; i < ctx.portfolio.position_count; ++i) {
                const portfolio::Position& row = ctx.portfolio.positions[i];
                submit_close(ctx, row, resolve_row(ctx, row));
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::Separator();

    if (ctx.portfolio.position_count == 0) {
        ImGui::TextDisabled("No open positions.");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable(
            "positions", 11,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("Market");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Position Value");
        ImGui::TableSetupColumn("Entry Price");
        ImGui::TableSetupColumn("Mark Price");
        ImGui::TableSetupColumn("PnL (ROE %)");
        ImGui::TableSetupColumn("Realized PnL");
        ImGui::TableSetupColumn("Liq. Price");
        ImGui::TableSetupColumn("Margin");
        ImGui::TableSetupColumn("Funding");
        ImGui::TableSetupColumn("");

        // Hand-rolled header row (rather than TableHeadersRow) so the columns whose meaning is
        // narrower than their label can carry a tooltip.
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        for (int column = 0; column < 11; ++column) {
            if (!ImGui::TableSetColumnIndex(column))
                continue;
            const char* name = ImGui::TableGetColumnName(column);
            ImGui::PushID(column);
            switch (column) {
                case 5:
                    header_with_tooltip(name,
                                        "Unrealized, marked to the venue's mark price.\n"
                                        "ROE is that P&L over the margin behind the position.");
                    break;
                case 6:
                    header_with_tooltip(name,
                                        "Closed P&L net of fees, accumulated from the fills\n"
                                        "this session has seen for this coin. Not lifetime.");
                    break;
                case 7:
                    header_with_tooltip(name,
                                        "The venue's liquidation price. Shown as 'est' when\n"
                                        "the venue has not published one yet, in which case\n"
                                        "it is this client's own estimate.");
                    break;
                case 9:
                    header_with_tooltip(name,
                                        "The venue's cumFunding.allTime for this position, in\n"
                                        "its own convention: a positive number is funding\n"
                                        "PAID, i.e. a cost, not a credit.");
                    break;
                default:
                    ImGui::TableHeader(name);
                    break;
            }
            ImGui::PopID();
        }

        for (uint32_t i = 0; i < ctx.portfolio.position_count; ++i) {
            const portfolio::Position& row = ctx.portfolio.positions[i];
            const pc_position& p = row.value;
            const RowView view = resolve_row(ctx, row);
            const RealizedRow& realized = event_store().realized(row.asset);

            // Keyed by asset, not by row index: the snapshot is rebuilt every few seconds and
            // a position closing elsewhere in the list shifts every index after it, which
            // would otherwise re-point an open popup at a different coin mid-interaction.
            ImGui::PushID(static_cast<int>(row.asset));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(view.coin);
            ImGui::SameLine();
            ImGui::TextDisabled("%ux %s", p.leverage, p.is_cross ? "cross" : "isolated");

            ImGui::TableNextColumn();
            format_qty(p.szi, view.sz_decimals, buf, sizeof(buf));
            ImGui::TextColored(p.szi >= 0 ? kColorBid : kColorAsk, "%s %s", buf, view.coin);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_usd(p.position_value, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_px(p.entry_px, view.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            if (view.mark > 0) {
                format_px(view.mark, view.sz_decimals, buf, sizeof(buf));
                ImGui::TextColored(view.mark_is_live ? kColorTextPrimary : kColorTextMuted, "%s",
                                   buf);
                if (!view.mark_is_live && ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Derived from the venue's own position value -- this coin is not the\n"
                        "selected instrument, so there is no live mark feed for it.");
            } else {
                ImGui::TextDisabled("--");
            }

            ImGui::TableNextColumn();
            char roe_buf[32];
            format_usd(view.unrealized, buf, sizeof(buf));
            // roe_bps is basis points; format_pct wants a kScale-scaled fraction, and
            // 1 bps == 1/10000, so value_1e8 = roe_bps * (kScale / 10000).
            format_pct(static_cast<int64_t>(view.roe_bps) * (kScale / 10'000), 2, roe_buf,
                       sizeof(roe_buf));
            ImGui::TextColored(view.unrealized >= 0 ? kColorBid : kColorAsk, "%s (%s)", buf,
                               roe_buf);

            ImGui::TableNextColumn();
            if (realized.fills > 0) {
                format_usd_fine(realized.pnl, buf, sizeof(buf));
                ImGui::TextColored(realized.pnl >= 0 ? kColorBid : kColorAsk, "%s", buf);
                if (ImGui::IsItemHovered()) {
                    char fees[32];
                    format_usd_fine(realized.fees, fees, sizeof(fees));
                    // Deliberately trading-only, unlike the session total in the strip above,
                    // which folds funding in. At row level the split is the informative part:
                    // it separates whether the entry and exit were good from whether the
                    // position is bleeding on carry, and the Funding column sits right beside
                    // this one to be read against it. Those two facts call for different
                    // responses, so the row keeps them apart.
                    ImGui::SetTooltip(
                        "Closed P&L net of fees: %s in fees over %u fills this session.\n"
                        "Only counts size actually closed -- while the position is open this\n"
                        "is just the fees paid to open it.\n\n"
                        "Trading only. Funding is the column to the right; the two are added\n"
                        "together in the Realized (session) total above.",
                        fees, realized.fills);
                }
            } else {
                ImGui::TextDisabled("--");
            }

            ImGui::TableNextColumn();
            if (view.liq_px > 0) {
                format_px(view.liq_px, view.sz_decimals, buf, sizeof(buf));
                ImGui::TextColored(kColorWarning, "%s", buf);
                if (view.liq_is_estimate) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("est");
                }
            } else {
                ImGui::TextDisabled("--");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "The venue has not published a liquidation price for this position,\n"
                        "and there is not enough account state yet to estimate one.");
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_usd(p.margin_used, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            // Left uncoloured on purpose. Every other signed column here is a P&L whose sign
            // means better/worse for the trader; this one is the venue's `cumFunding.allTime`
            // in its own convention, where a positive number is funding PAID (a cost). Giving
            // it the green/red treatment would read as the opposite of what it means.
            ImGui::TextUnformatted(format_usd_fine(p.cum_funding, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Close"))
                ImGui::OpenPopup("confirm_close");
            ImGui::SameLine();
            if (ImGui::SmallButton("TP/SL")) {
                // Cleared on open rather than carried over, so a trigger typed for one coin
                // can never be submitted against another.
                if (g_prefs.tpsl_asset != row.asset) {
                    g_prefs.tp_buf[0] = '\0';
                    g_prefs.sl_buf[0] = '\0';
                    g_prefs.tp_sz_buf[0] = '\0';
                    g_prefs.sl_sz_buf[0] = '\0';
                }
                g_prefs.tpsl_asset = row.asset;
                ImGui::OpenPopup("position_tpsl");
            }

            if (ImGui::BeginPopupModal("confirm_close", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                char qty_buf[32], mark_buf[32];
                format_qty(view.abs_size, view.sz_decimals, qty_buf, sizeof(qty_buf));
                format_px(view.mark, view.sz_decimals, mark_buf, sizeof(mark_buf));
                ImGui::Text("Close %s %s (%s) at market?", qty_buf, view.coin,
                            p.szi > 0 ? "long" : "short");
                ImGui::TextDisabled("Mark %s -- reduce-only IOC at up to %.1f%% slippage.",
                                    mark_buf,
                                    static_cast<double>(exec::Rounder::kDefaultSlippageBps) /
                                        100.0);
                format_usd(view.unrealized, buf, sizeof(buf));
                ImGui::TextColored(view.unrealized >= 0 ? kColorBid : kColorAsk,
                                   "Realizes %s before fees", buf);
                ImGui::Separator();
                if (ImGui::Button("Close position")) {
                    submit_close(ctx, row, view);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }

            if (ImGui::BeginPopupModal("position_tpsl", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                const exec::AssetPrecision precision{view.sz_decimals};
                char entry_buf[32], size_buf[32];
                format_px(p.entry_px, view.sz_decimals, entry_buf, sizeof(entry_buf));
                format_qty(view.abs_size, view.sz_decimals, size_buf, sizeof(size_buf));
                ImGui::Text("%s %s -- %s %s, entry %s", view.coin,
                            p.szi > 0 ? "long" : "short", size_buf, view.coin, entry_buf);
                ImGui::TextDisabled(
                    "Reduce-only market triggers. Leave a price blank to skip that leg,\n"
                    "a size blank to use the whole position. Place as many as you like --\n"
                    "this stays open so a scaled exit can be built one tranche at a time.\n"
                    "Trigger '%%' is a move of the position (leverage included); size '%%'\n"
                    "is a fraction of the position.");

                // --- What is already protecting this position -------------------------------
                // Built from the resting orders this session knows about, filtered to this
                // asset's reduce-only triggers. Shown here rather than only in Open Orders
                // because "how much of this position is already covered" is the question being
                // answered while adding another tranche -- and because a leg cannot be
                // meaningfully sized without it.
                Qty covered_tp = 0;
                Qty covered_sl = 0;
                ImGui::Separator();
                ImGui::TextDisabled("Resting triggers");
                bool any_resting = false;
                uint64_t cancel_oid = 0;
                for (size_t k = 0; k < event_store().open_order_count(); ++k) {
                    const OrderRow& order = event_store().open_order_at(k);
                    if (order.asset != row.asset || !order.is_trigger || order.trigger_px <= 0)
                        continue;
                    any_resting = true;
                    if (order.tpsl == PC_TPSL_TP)
                        covered_tp += order.sz;
                    else if (order.tpsl == PC_TPSL_SL)
                        covered_sl += order.sz;

                    char trig_buf[32], sz_buf[32];
                    format_px(order.trigger_px, view.sz_decimals, trig_buf, sizeof(trig_buf));
                    format_qty(order.sz, view.sz_decimals, sz_buf, sizeof(sz_buf));
                    ImGui::PushID(static_cast<int>(k));
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(order.tpsl == PC_TPSL_TP ? kColorBid : kColorWarning,
                                       "%s", order.tpsl == PC_TPSL_TP ? "TP" : "SL");
                    ImGui::SameLine();
                    ImGui::Text("%s   %s %s", trig_buf, sz_buf, view.coin);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Cancel"))
                        cancel_oid = order.oid;  // acted on after the loop, see below
                    ImGui::PopID();
                }
                if (!any_resting)
                    ImGui::TextDisabled("  none -- this position is unprotected");
                // Issued outside the loop so the command is not sent while iterating the list
                // it is about to change.
                if (cancel_oid != 0)
                    cancel_order(ctx, row.asset, cancel_oid);

                if (any_resting) {
                    char tp_buf[32], sl_buf[32];
                    format_qty(view.abs_size - std::min(covered_tp, view.abs_size),
                               view.sz_decimals, tp_buf, sizeof(tp_buf));
                    format_qty(view.abs_size - std::min(covered_sl, view.abs_size),
                               view.sz_decimals, sl_buf, sizeof(sl_buf));
                    ImGui::TextDisabled("Uncovered: %s %s by take profit, %s %s by stop loss",
                                        tp_buf, view.coin, sl_buf, view.coin);
                }

                ImGui::Separator();

                auto leg_row = [&](const char* id, const char* combo_id, const char* sz_id,
                                   const char* sz_combo_id, const char* label, char* text_buf,
                                   size_t cap, TpSlUnit* unit, char* sz_text, size_t sz_cap,
                                   SizeUnit* sz_unit, bool is_tp) -> PositionLeg {
                    ImGui::SetNextItemWidth(120.0F);
                    ImGui::InputText(id, text_buf, cap);
                    ImGui::SameLine();
                    // Switching a field's unit rewrites its buffer to the equivalent value in
                    // the new unit, so toggling never silently reinterprets "5" as a $5 trigger
                    // (or 5%).
                    ImGui::SetNextItemWidth(70.0F);
                    const char* size_error = nullptr;
                    const Qty leg_size = resolve_leg_size(sz_text, *sz_unit, view.abs_size,
                                                          precision, &size_error);
                    // Resolved after the fields are drawn so the preview reflects this frame's
                    // keystroke, and from the same call whose result is submitted below --
                    // what is displayed is what gets signed.
                    const PositionLeg leg = resolve_leg(text_buf, *unit, is_tp, p,
                                                        view.abs_size, leg_size, size_error,
                                                        precision);
                    if (ImGui::BeginCombo(combo_id, tpsl_unit_label(*unit))) {
                        auto option = [&](TpSlUnit candidate) {
                            if (!ImGui::Selectable(tpsl_unit_label(candidate),
                                                   *unit == candidate) ||
                                *unit == candidate)
                                return;
                            *unit = candidate;
                            if (!leg.valid)
                                return;
                            if (candidate == TpSlUnit::Price) {
                                format_px(leg.trigger, view.sz_decimals, text_buf, cap);
                                return;
                            }
                            // Magnitude only: the sign is implied by the leg and the side.
                            const int64_t mag = leg.roe_1e8 < 0 ? -leg.roe_1e8 : leg.roe_1e8;
                            std::snprintf(text_buf, cap, "%lld.%02lld",
                                          static_cast<long long>(mag * 100 / kScale),
                                          static_cast<long long>((mag * 100 % kScale) * 100 /
                                                                 kScale));
                        };
                        option(TpSlUnit::Price);
                        option(TpSlUnit::Pct);
                        ImGui::EndCombo();
                    }

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(90.0F);
                    ImGui::InputTextWithHint(sz_id, "all", sz_text, sz_cap);
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(70.0F);
                    if (ImGui::BeginCombo(sz_combo_id, size_unit_label(*sz_unit, view.coin))) {
                        auto size_option = [&](SizeUnit candidate) {
                            if (!ImGui::Selectable(size_unit_label(candidate, view.coin),
                                                   *sz_unit == candidate) ||
                                *sz_unit == candidate)
                                return;
                            *sz_unit = candidate;
                            if (sz_text[0] == '\0' || leg_size <= 0)
                                return;  // blank still means "all", in either unit
                            if (candidate == SizeUnit::Coin) {
                                format_qty(leg_size, view.sz_decimals, sz_text, sz_cap);
                                return;
                            }
                            const int64_t pct = view.abs_size > 0
                                                    ? static_cast<int64_t>(
                                                          static_cast<__int128>(leg_size) * 100 /
                                                          view.abs_size)
                                                    : 0;
                            std::snprintf(sz_text, sz_cap, "%lld", static_cast<long long>(pct));
                        };
                        size_option(SizeUnit::Pct);
                        size_option(SizeUnit::Coin);
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::TextUnformatted(label);

                    if (leg.present && !leg.valid) {
                        ImGui::TextColored(kColorAsk, "  %s",
                                           leg.error ? leg.error : "invalid trigger");
                    } else if (leg.valid) {
                        char pnl_buf[32], pct_buf[32], trigger_buf[32], sz_disp[32];
                        format_usd_fine(leg.pnl, pnl_buf, sizeof(pnl_buf));
                        format_pct(leg.roe_1e8, 2, pct_buf, sizeof(pct_buf));
                        format_px(leg.trigger, view.sz_decimals, trigger_buf,
                                  sizeof(trigger_buf));
                        format_qty(leg.sz, view.sz_decimals, sz_disp, sizeof(sz_disp));
                        ImGui::TextColored(leg.pnl >= 0 ? kColorBid : kColorAsk,
                                           "  closes %s %s at %s -- %s (%s on margin)", sz_disp,
                                           view.coin, trigger_buf, pnl_buf, pct_buf);
                    }
                    return leg;
                };

                const PositionLeg tp =
                    leg_row("##tp", "##tp_unit", "##tp_sz", "##tp_sz_unit", "Take profit",
                            g_prefs.tp_buf, sizeof(g_prefs.tp_buf), &g_prefs.tp_unit,
                            g_prefs.tp_sz_buf, sizeof(g_prefs.tp_sz_buf), &g_prefs.tp_sz_unit,
                            true);
                const PositionLeg sl =
                    leg_row("##sl", "##sl_unit", "##sl_sz", "##sl_sz_unit", "Stop loss",
                            g_prefs.sl_buf, sizeof(g_prefs.sl_buf), &g_prefs.sl_unit,
                            g_prefs.sl_sz_buf, sizeof(g_prefs.sl_sz_buf), &g_prefs.sl_sz_unit,
                            false);

                // A stop at or past liquidation never fires: the venue closes the position
                // first, at the liquidation price and with the liquidation fee, so the loss
                // the stop promises is not the loss that happens.
                if (sl.valid &&
                    portfolio::stop_beyond_liquidation(sl.trigger, view.liq_px, p.szi)) {
                    char liq_buf[32];
                    format_px(view.liq_px, view.sz_decimals, liq_buf, sizeof(liq_buf));
                    ImGui::TextColored(kColorWarning,
                                       "Stop is past the %sliquidation price %s -- you are\n"
                                       "liquidated before it can trigger",
                                       view.liq_is_estimate ? "estimated " : "", liq_buf);
                }

                // Over-committing is not blocked -- the venue rejects a reduce-only order that
                // would increase the position, and legs can legitimately overlap while a
                // scaled exit is being rebuilt -- but it is worth saying out loud.
                if (tp.valid && covered_tp + tp.sz > view.abs_size)
                    ImGui::TextColored(kColorWarning,
                                       "Take profits would total more than the position.");
                if (sl.valid && covered_sl + sl.sz > view.abs_size)
                    ImGui::TextColored(kColorWarning,
                                       "Stops would total more than the position.");

                ImGui::Separator();

                const bool blocked = (!tp.valid && !sl.valid) ||
                                     (tp.present && !tp.valid) || (sl.present && !sl.valid);
                ImGui::BeginDisabled(blocked);
                if (ImGui::Button("Place")) {
                    submit_tpsl(ctx, row, view, tp, sl);
                    // Only the prices are cleared. The popup stays open and the size fields
                    // keep their value, which is what building a scaled exit actually looks
                    // like: same 25% slice, three different prices.
                    g_prefs.tp_buf[0] = '\0';
                    g_prefs.sl_buf[0] = '\0';
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Done")) {
                    g_prefs.tpsl_asset = PC_ASSET_NONE;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Same contract as the ticket's note line: this says the intent left the panel, not that
    // the venue accepted it. Acks, fills and rejections belong to Open Orders and Status.
    constexpr uint64_t kNoteMs = 6'000;
    if (g_prefs.note[0] != '\0') {
        if (ctx.now_ms - g_prefs.note_ms > kNoteMs)
            g_prefs.note[0] = '\0';
        else
            ImGui::TextColored(g_prefs.note_error ? kColorAsk : kColorBid, "%s", g_prefs.note);
    }

    ImGui::End();
}

}  // namespace pc::ui
