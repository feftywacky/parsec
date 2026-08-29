#include <algorithm>

#include <imgui.h>

#include "portfolio/collateral.hpp"
#include "portfolio/live_marks.hpp"
#include "portfolio/order_preview.hpp"
#include "portfolio/pnl.hpp"
#include "ui/app_window.hpp"
#include "ui/asset_lookup.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// Every figure below is derived from a `clearinghouseState` snapshot that lands about every
// 4s, re-marked to the live mark by portfolio::apply_live_mark -- see that header for why the
// raw snapshot cannot be printed beside the Positions tab, and for the cross/isolated split.
//
// Only the instrument on screen has a live mark; for every other position `row_mark` recovers
// the venue's own mark from `position_value / |szi|`, which is the snapshot mark exactly, so
// those contribute nothing and the totals stay the venue's numbers.
portfolio::AccountMarks live_marks(const PanelContext& ctx) noexcept {
    portfolio::AccountMarks out = portfolio::account_marks(ctx.portfolio.account);
    const Px live_mark = ctx.instrument.ctx.mark_px();
    for (uint32_t i = 0; i < ctx.portfolio.position_count; ++i) {
        const portfolio::Position& row = ctx.portfolio.positions[i];
        if (row.asset != ctx.instrument.asset)
            continue;
        const AssetLabel label = lookup_asset(ctx.universe, row.asset, ctx.sz_decimals);
        portfolio::apply_live_mark(
            out, row.value, live_mark,
            portfolio::maintenance_rate_for_max_leverage(label.max_leverage));
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

    const portfolio::AccountMarks m = live_marks(ctx);
    char buf[32];

    // Wide enough for the longest label ("Cross maintenance margin", 24). At 22 the two labels
    // already past it pushed their own value a column right of every other row.
    auto row = [&](const char* label, pc::Usd v) {
        format_usd(v, buf, sizeof(buf));
        ImGui::Text("%-26s", label);
        ImGui::SameLine();
        ImGui::Text("%s", buf);
    };

    // USDC first, because it is the account's actual balance and everything below is derived
    // from it. Hyperliquid collateralises perps from one USDC pool, so `spot.total` is not a
    // separate wallet sitting beside the perp account -- it CONTAINS the perp equity, which is
    // why "Total balance" reads larger than "Account value" by exactly the idle balance. These
    // two lines are the venue's own Balances tab.
    //
    // USDC only: it is the sole perp collateral. A spot portfolio would need spotMeta and per
    // token marks to price the other rows, which is a different feature.
    //
    // "Available balance" is NOT "Available margin" further down and the two do not have to
    // agree: this one is the idle USDC the venue reports as unheld, while free margin also
    // counts the perp side's own free cash, so it sits about `withdrawable` higher.
    if (ctx.portfolio.spot_valid) {
        const pc_spot& spot = ctx.portfolio.spot;
        row("Total balance (USDC)", spot.total);
        row("Available balance", std::max<Usd>(0, spot.total - spot.hold));
        ImGui::Spacing();
    }

    row("Account value", m.account_value);
    row("Total margin used", m.total_margin_used);
    row("Total notional position", m.total_ntl_pos);
    row("Withdrawable", m.withdrawable);

    // Free collateral: the USDC that can back a NEW position.
    //
    // NOT `account_value - total_margin_used`. That subtraction looks equivalent but is not:
    // `total_margin_used` is marked to market, so it grows as a position moves against you,
    // while the margin behind that position was already posted at entry. The difference goes
    // negative on any ordinary adverse move -- a 10x position needs 10% initial margin against
    // ~1.25% maintenance, so equity sits below the initial requirement long before anything is
    // at risk -- and reported "-$44.97 available", which is not a quantity of anything.
    //
    // NOT `withdrawable` alone either, which is what this line used to be. That is the PERP
    // side's free cash and cannot see USDC that has never been deployed into perps: measured
    // against mainnet it read $37 while the venue's own `availableToTrade` for the opening
    // side said $1,471, because $1,434 of the balance was simply idle. free_collateral() adds
    // the idle part back; the clamp inside it guards the live re-mark above, not the venue.
    const Usd available = portfolio::free_collateral(m.account_value, m.withdrawable,
                                                     ctx.portfolio.spot, ctx.portfolio.spot_valid);
    format_usd(available, buf, sizeof(buf));
    ImGui::Text("%-26s", "Available margin");
    ImGui::SameLine();
    ImGui::Text("%s", buf);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "What can back a new position, marked to the current price -- so unrealized\n"
            "P&L on open positions is spendable here and this number moves every tick.\n"
            "The line below is the same pool with open P&L stripped out.");

    // The same pool valued at cost. "Available margin" is the number the venue will actually
    // let an order through on, but it counts unrealized gains as buying power, so a winning
    // position makes it climb without a dollar being banked -- and it falls straight back the
    // moment the mark turns. Sizing off it means sizing off a number that can evaporate, so
    // the cash figure sits underneath it rather than in a tooltip.
    Usd total_unrealized = 0;
    Usd total_cost_basis = 0;
    for (uint32_t i = 0; i < ctx.portfolio.position_count; ++i) {
        const portfolio::Position& row = ctx.portfolio.positions[i];
        total_unrealized += row.asset == ctx.instrument.asset && ctx.instrument.ctx.mark_px() > 0
                                ? portfolio::unrealized_pnl(row.value.szi, row.value.entry_px,
                                                            ctx.instrument.ctx.mark_px())
                                : row.value.unrealized_pnl;
        total_cost_basis += portfolio::position_cost_basis(row.value);
    }
    if (ctx.portfolio.position_count > 0) {
        const Usd cash = portfolio::cash_collateral(available, m.account_value, total_unrealized,
                                                    total_cost_basis, ctx.portfolio.spot,
                                                    ctx.portfolio.spot_valid);
        format_usd(cash, buf, sizeof(buf));
        ImGui::Text("%-26s", "USDC available (cash)");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", buf);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Available margin with open P&L removed: idle USDC, plus account equity\n"
                "valued at entry, minus the margin already posted. Sizing off this rather\n"
                "than the line above means never sizing off a paper gain.");
    }

    // Cross-only, like the venue's `crossMaintenanceMarginUsed` it comes from: an isolated
    // position's maintenance requirement is charged against its own walled-off collateral and
    // is not part of this number. Labelled so, because pairing it with the cross+isolated
    // account value below would otherwise read as a whole-account safety margin.
    row("Cross maintenance margin", m.cross_maintenance);

    // Zero free collateral stops new positions but says nothing about safety, so the cushion
    // over the maintenance requirement -- the number that actually tracks liquidation -- goes
    // next to it rather than leaving "$0.00" to be read as distress. Cross equity against
    // cross maintenance: isolated equity cannot be pulled in to defend a cross position, so
    // including it here would overstate the cushion by the whole isolated book.
    if (available <= 0 && ctx.portfolio.position_count > 0) {
        const Usd cushion = m.cross_account_value - m.cross_maintenance;
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
