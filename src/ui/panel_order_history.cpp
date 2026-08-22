#include <imgui.h>

#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

const char* status_text(uint16_t status) noexcept {
    switch (status) {
        case PC_ORD_OPEN:
            return "Open";
        case PC_ORD_FILLED:
            return "Filled";
        case PC_ORD_CANCELED:
            return "Canceled";
        case PC_ORD_TRIGGERED:
            return "Triggered";
        case PC_ORD_REJECTED:
            return "Rejected";
        case PC_ORD_MARGIN_CANCELED:
            return "Margin Canceled";
        default:
            return "Unknown";
    }
}

}  // namespace

void draw_order_history(PanelContext& ctx) {
    event_store().drain(ctx.bridge);

    if (!ImGui::Begin(kWindowOrderHistory)) {
        ImGui::End();
        return;
    }

    const size_t n = event_store().history_count();
    if (n == 0) {
        ImGui::TextDisabled("No terminal order events this session.");
        ImGui::TextDisabled(
            "(This session's live orderUpdates stream only -- no historicalOrders REST "
            "backfill wired here.)");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable(
            "order_history", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("OID");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Detail");
        ImGui::TableHeadersRow();

        char buf[32];
        for (size_t i = 0; i < n; ++i) {
            const OrderRow& r = event_store().history_at(i);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(r.oid));

            ImGui::TableNextColumn();
            const bool bad = r.status == PC_ORD_REJECTED || r.status == PC_ORD_MARGIN_CANCELED;
            ImGui::TextColored(bad ? kColorAsk : kColorTextPrimary, "%s", status_text(r.status));

            ImGui::TableNextColumn();
            ImGui::TextColored(r.is_buy ? kColorBid : kColorAsk, "%s", r.is_buy ? "Buy" : "Sell");

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_px(r.px, ctx.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_qty(r.sz, ctx.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", r.err);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace pc::ui
