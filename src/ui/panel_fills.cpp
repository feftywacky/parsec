#include <imgui.h>

#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {

void draw_fills(PanelContext& ctx) {
    event_store().drain(ctx.bridge);

    if (!ImGui::Begin(kWindowFills)) {
        ImGui::End();
        return;
    }

    const size_t n = event_store().fill_count();
    if (n == 0) {
        ImGui::TextDisabled("No fills this session.");
        ImGui::TextDisabled(
            "(This session's live fill stream only -- no userFills REST backfill wired here.)");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable(
            "fills", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("OID");
        ImGui::TableSetupColumn("TID");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Size");
        // `fee` already includes builderFee (docs/03 §W2.9) -- displayed as-is, never summed
        // with anything else. `crossed` (taker) is shown directly, not derived.
        ImGui::TableSetupColumn("Fee");
        ImGui::TableSetupColumn("Closed PnL");
        ImGui::TableHeadersRow();

        char buf[32];
        for (size_t i = 0; i < n; ++i) {
            const FillRow& r = event_store().fill_at(i);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(r.oid));

            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(r.tid));

            ImGui::TableNextColumn();
            ImGui::TextColored(r.is_buy ? kColorBid : kColorAsk, "%s",
                               r.is_buy ? (r.is_taker ? "Buy (taker)" : "Buy (maker)")
                                        : (r.is_taker ? "Sell (taker)" : "Sell (maker)"));

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_px(r.px, ctx.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_qty(r.qty, ctx.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_usd(r.fee, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            format_usd(r.closed_pnl, buf, sizeof(buf));
            ImGui::TextColored(r.closed_pnl >= 0 ? kColorBid : kColorAsk, "%s", buf);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace pc::ui
