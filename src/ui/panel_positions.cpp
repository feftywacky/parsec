#include <imgui.h>

#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {

void draw_positions(PanelContext& ctx) {
    if (!ImGui::Begin(kWindowPositions)) {
        ImGui::End();
        return;
    }

    if (ctx.portfolio.position_count == 0) {
        ImGui::TextDisabled("No open positions.");
        ImGui::End();
        return;
    }

    // KNOWN GAP: app::PortfolioSnapshot::positions (src/app/ui_bridge.hpp) is built by
    // app::make_portfolio_snapshot() compacting portfolio::PositionBook's per-asset slots into
    // a dense array, but neither portfolio::Position nor pc_position carries the asset id each
    // row came from -- so this panel has no reliable way to print which coin a row is for. The
    // "Asset" column below shows the row index as a placeholder rather than a real symbol; see
    // this pass's report for the fix (thread an asset id through PortfolioSnapshot).
    if (ImGui::BeginTable(
            "positions", 9,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Asset");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Entry Px");
        ImGui::TableSetupColumn("Liq Px");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Unrealized PnL");
        ImGui::TableSetupColumn("ROE");
        ImGui::TableSetupColumn("Margin Used");
        ImGui::TableSetupColumn("Leverage / Mode");
        ImGui::TableHeadersRow();

        char buf[32];
        for (uint32_t i = 0; i < ctx.portfolio.position_count; ++i) {
            const pc_position& p = ctx.portfolio.positions[i].value;
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("#%u", i);

            ImGui::TableNextColumn();
            format_qty(p.szi, ctx.sz_decimals, buf, sizeof(buf));
            ImGui::TextColored(p.szi >= 0 ? kColorBid : kColorAsk, "%s", buf);

            ImGui::TableNextColumn();
            format_px(p.entry_px, ctx.sz_decimals, buf, sizeof(buf));
            ImGui::Text("%s", buf);

            ImGui::TableNextColumn();
            format_px(p.liq_px, ctx.sz_decimals, buf, sizeof(buf));
            ImGui::TextColored(kColorWarning, "%s", buf);

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_usd(p.position_value, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            format_usd(p.unrealized_pnl, buf, sizeof(buf));
            ImGui::TextColored(p.unrealized_pnl >= 0 ? kColorBid : kColorAsk, "%s", buf);

            ImGui::TableNextColumn();
            // roe_bps is basis points; format_pct wants a kScale-scaled fraction, and
            // 1 bps == 1/10000, so value_1e8 = roe_bps * (kScale / 10000).
            format_pct(static_cast<int64_t>(p.roe_bps) * (kScale / 10'000), 2, buf, sizeof(buf));
            ImGui::TextColored(p.roe_bps >= 0 ? kColorBid : kColorAsk, "%s", buf);

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_usd(p.margin_used, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::Text("%ux %s", p.leverage, p.is_cross ? "Cross" : "Isolated");
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace pc::ui
