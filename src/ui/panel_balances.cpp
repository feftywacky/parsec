#include <algorithm>

#include <imgui.h>

#include "portfolio/order_preview.hpp"
#include "ui/app_window.hpp"
#include "ui/asset_lookup.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// Every figure below is derived from a `clearinghouseState` snapshot that lands about every
// 4s. The Positions tab does NOT show that vintage: it re-marks the row on screen from the
// live activeAssetCtx mark on every frame (panel_positions.cpp, `row_mark`), so PnL there
// ticks with the market. Printing the raw snapshot here meant the two tabs disagreed with each
// other at the same instant -- Positions saying mark 78,869 / unrealized $935 while Balances
// still carried mark 78,911 / unrealized $904, a ~$31 gap in account value that read as "the
// Balances tab is not updating". So this applies the same live re-mark, and the two tabs now
// move together.
//
// Only the instrument on screen has a live mark; for every other position `row_mark` recovers
// the venue's own mark from `position_value / |szi|`, which is the snapshot mark exactly, so
// those contribute nothing and the totals stay the venue's numbers.
struct LiveMarks {
    Usd account_value{};
    Usd total_margin_used{};
    Usd total_ntl_pos{};
    Usd withdrawable{};
    Usd maintenance{};
};

LiveMarks live_marks(const PanelContext& ctx) noexcept {
    const pc_account& a = ctx.portfolio.account;
    LiveMarks out{a.account_value, a.total_margin_used, a.total_ntl_pos, a.withdrawable,
                  a.cross_maintenance_margin};

    const Px live_mark = ctx.instrument.ctx.mark_px();
    if (live_mark <= 0)
        return out;

    for (uint32_t i = 0; i < ctx.portfolio.position_count; ++i) {
        const portfolio::Position& row = ctx.portfolio.positions[i];
        if (row.asset != ctx.instrument.asset)
            continue;
        const pc_position& p = row.value;
        const Qty abs_size = p.szi < 0 ? -p.szi : p.szi;
        if (abs_size <= 0 || p.position_value == 0 || p.leverage == 0)
            continue;

        const Usd snap_value = p.position_value < 0 ? -p.position_value : p.position_value;
        const Usd live_value = notional(live_mark, abs_size);
        const Usd d_value = live_value - snap_value;
        if (d_value == 0)
            continue;

        // A long gains when the position is worth more; a short loses by the same amount.
        const Usd d_unrealized = p.szi > 0 ? d_value : -d_value;
        const Usd d_margin = portfolio::initial_margin(live_value, p.leverage) -
                             portfolio::initial_margin(snap_value, p.leverage);

        out.account_value += d_unrealized;
        out.total_ntl_pos += d_value;
        out.total_margin_used += d_margin;
        // Equity moved by the PnL and the margin requirement moved with the notional; free
        // cash is what is left of the first after the second.
        out.withdrawable += d_unrealized - d_margin;

        if (p.is_cross != 0) {
            const AssetLabel label = lookup_asset(ctx.universe, row.asset, ctx.sz_decimals);
            const Usd rate = portfolio::maintenance_rate_for_max_leverage(label.max_leverage);
            if (rate > 0)
                out.maintenance += portfolio::maintenance_margin(live_value, rate) -
                                   portfolio::maintenance_margin(snap_value, rate);
        }
    }
    return out;
}

}  // namespace

void draw_balances(PanelContext& ctx) {
    if (!ImGui::Begin(kWindowBalances)) {
        ImGui::End();
        return;
    }

    if (!ctx.portfolio.account_valid) {
        ImGui::TextDisabled("No account data yet.");
        ImGui::End();
        return;
    }

    const LiveMarks m = live_marks(ctx);
    char buf[32];

    auto row = [&](const char* label, pc::Usd v) {
        format_usd(v, buf, sizeof(buf));
        ImGui::Text("%-22s", label);
        ImGui::SameLine();
        ImGui::Text("%s", buf);
    };

    row("Account value", m.account_value);
    row("Total margin used", m.total_margin_used);
    row("Total notional position", m.total_ntl_pos);
    row("Withdrawable", m.withdrawable);

    // Free collateral: the USDC sitting in the account that is NOT backing a position and can
    // therefore fund a new one. That is `withdrawable` -- the venue's own answer to "what can
    // leave the account", which is the same question as "what can enter a new position".
    //
    // NOT `account_value - total_margin_used`. That subtraction looks equivalent but is not:
    // `total_margin_used` is marked to market, so it grows as a position moves against you,
    // while the margin behind that position was already posted at entry. The difference goes
    // negative on any ordinary adverse move -- a 10x position needs 10% initial margin against
    // ~1.25% maintenance, so equity sits below the initial requirement long before anything is
    // at risk -- and reported "-$44.97 available", which is not a quantity of anything. The
    // venue never reports `withdrawable` below zero; the clamp guards the live re-mark above,
    // not the venue.
    const Usd available = std::max<Usd>(0, m.withdrawable);
    format_usd(available, buf, sizeof(buf));
    ImGui::Text("%-22s", "Available margin");
    ImGui::SameLine();
    ImGui::Text("%s", buf);

    row("Maintenance margin", m.maintenance);

    // Zero free collateral stops new positions but says nothing about safety, so the cushion
    // over the maintenance requirement -- the number that actually tracks liquidation -- goes
    // next to it rather than leaving "$0.00" to be read as distress.
    if (available <= 0 && ctx.portfolio.position_count > 0) {
        const Usd cushion = m.account_value - m.maintenance;
        char cushion_buf[32];
        format_usd(cushion > 0 ? cushion : -cushion, cushion_buf, sizeof(cushion_buf));
        if (cushion > 0)
            ImGui::TextColored(kColorTextMuted,
                               "No free collateral -- open positions are using it all. %s above "
                               "the maintenance requirement.",
                               cushion_buf);
        else
            ImGui::TextColored(kColorAsk,
                               "Equity is %s below the maintenance requirement -- the position "
                               "is at risk of liquidation.",
                               cushion_buf);
    }

    ImGui::End();
}

}  // namespace pc::ui
