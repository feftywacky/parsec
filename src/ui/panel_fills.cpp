#include <imgui.h>

#include "ui/app_window.hpp"
#include "ui/asset_lookup.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// What the fill did to the position, from the venue's own `dir` string (pc_fill::dir). Not
// derived from the side: a sell is an opening short or a closing long depending on what was
// held, and only the venue knows which.
const char* direction_text(uint8_t dir, bool is_buy) noexcept {
    switch (dir) {
        case PC_DIR_OPEN_LONG:
            return "Open Long";
        case PC_DIR_CLOSE_LONG:
            return "Close Long";
        case PC_DIR_OPEN_SHORT:
            return "Open Short";
        case PC_DIR_CLOSE_SHORT:
            return "Close Short";
        case PC_DIR_LONG_TO_SHORT:
            return "Long > Short";
        case PC_DIR_SHORT_TO_LONG:
            return "Short > Long";
        case PC_DIR_LIQUIDATION:
            return "Liquidated";
        case PC_DIR_BUY:
            return "Buy";
        case PC_DIR_SELL:
            return "Sell";
        default:
            // Pre-`dir` events, and any wording the venue adds later. The side is the one
            // thing that is always known, so say that rather than nothing.
            return is_buy ? "Buy" : "Sell";
    }
}

// Green for anything that grows a long or realises a gain, red for the mirror. Closing
// directions take their colour from the P&L rather than the side, since "Close Short" is a
// good outcome or a bad one depending only on the number beside it.
ImVec4 direction_color(uint8_t dir, bool is_buy) noexcept {
    switch (dir) {
        case PC_DIR_OPEN_LONG:
        case PC_DIR_SHORT_TO_LONG:
        case PC_DIR_BUY:
            return kColorBid;
        case PC_DIR_OPEN_SHORT:
        case PC_DIR_LONG_TO_SHORT:
        case PC_DIR_SELL:
            return kColorAsk;
        case PC_DIR_CLOSE_LONG:
        case PC_DIR_CLOSE_SHORT:
            return kColorTextPrimary;
        case PC_DIR_LIQUIDATION:
            return kColorWarning;
        default:
            return is_buy ? kColorBid : kColorAsk;
    }
}

}  // namespace

void draw_fills(PanelContext& ctx) {
    event_store().drain(ctx.bridge);

    if (!ImGui::Begin(kWindowFills)) {
        ImGui::End();
        return;
    }

    const size_t n = event_store().fill_count();
    if (n == 0) {
        ImGui::TextDisabled("No trades yet.");
        ImGui::TextDisabled(
            "(The live fill stream plus the venue's userFills backfill, pulled once the\n"
            "account snapshot proves the session is signed in. Duplicates across the two\n"
            "are dropped by trade id.)");
        ImGui::End();
        return;
    }

    // Session totals, so the table answers "how did today go" without a spreadsheet. Both are
    // over the fills held (kMaxFills), not the account's lifetime -- see RealizedRow.
    const RealizedRow& realized = event_store().realized_total();
    char buf[48];
    ImGui::TextDisabled("Realized");
    ImGui::SameLine();
    format_usd_fine(realized.pnl, buf, sizeof(buf));
    ImGui::TextColored(realized.pnl >= 0 ? kColorBid : kColorAsk, "%s", buf);
    ImGui::SameLine();
    ImGui::TextDisabled("|  Fees");
    ImGui::SameLine();
    ImGui::TextUnformatted(format_usd_fine(realized.fees, buf, sizeof(buf)));
    ImGui::SameLine();
    ImGui::TextDisabled("|  %u fills", realized.fills);
    ImGui::Separator();

    if (ImGui::BeginTable(
            "fills", 8,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Market");
        ImGui::TableSetupColumn("Direction");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Trade Value");
        // `fee` already includes builderFee (docs/03 §W2.9) -- displayed as-is, never summed
        // with anything else. `crossed` (taker) rides along as a tooltip on the direction.
        ImGui::TableSetupColumn("Fee");
        ImGui::TableSetupColumn("Closed PnL");
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < n; ++i) {
            const FillRow& r = event_store().fill_at(i);
            // Per-row precision, not the active instrument's: a fill on a coin with different
            // szDecimals would otherwise print its price and size at the wrong number of
            // decimals, which is a wrong number, not a cosmetic one.
            const AssetLabel asset = lookup_asset(ctx.universe, r.asset, ctx.sz_decimals);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_time_ms(r.time_ms, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(asset.coin);

            ImGui::TableNextColumn();
            ImGui::TextColored(direction_color(r.dir, r.is_buy), "%s",
                               direction_text(r.dir, r.is_buy));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s  ·  order %llu  ·  trade %llu",
                                  r.is_taker ? "Taker" : "Maker",
                                  static_cast<unsigned long long>(r.oid),
                                  static_cast<unsigned long long>(r.tid));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_px(r.px, asset.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::Text("%s %s", format_qty(r.qty, asset.sz_decimals, buf, sizeof(buf)),
                        asset.coin);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_usd(notional(r.px, r.qty), buf, sizeof(buf)));

            ImGui::TableNextColumn();
            // Fine precision: a taker fee on a small order is a fraction of a cent, and at two
            // decimals it renders as "$0.00" -- indistinguishable from a fee-free fill.
            ImGui::TextUnformatted(format_usd_fine(r.fee, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            if (r.closed_pnl != 0 || r.dir == PC_DIR_CLOSE_LONG ||
                r.dir == PC_DIR_CLOSE_SHORT) {
                format_usd_fine(r.closed_pnl, buf, sizeof(buf));
                ImGui::TextColored(r.closed_pnl >= 0 ? kColorBid : kColorAsk, "%s", buf);
                if (ImGui::IsItemHovered()) {
                    char net[32];
                    format_usd_fine(r.closed_pnl - r.fee, net, sizeof(net));
                    ImGui::SetTooltip("Gross, before this fill's fee. Net: %s", net);
                }
            } else {
                // An opening fill closes nothing; a "$0.00" there reads as a break-even trade
                // rather than as a trade that has not been closed.
                ImGui::TextDisabled("--");
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace pc::ui
