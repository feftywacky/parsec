#include <imgui.h>

#include <cstdio>

#include "ui/app_window.hpp"
#include "ui/asset_lookup.hpp"
#include "ui/order_label.hpp"
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
        ImGui::TextDisabled("No terminal order events yet.");
        ImGui::TextDisabled(
            "(The live orderUpdates stream plus the venue's historicalOrders backfill,\n"
            "pulled once the account snapshot proves the session is signed in.)");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable(
            "order_history", 11,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Market");
        ImGui::TableSetupColumn("Direction");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Filled");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Reduce Only");
        ImGui::TableSetupColumn("Trigger Condition");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Order ID");
        ImGui::TableHeadersRow();

        char buf[64];
        for (size_t i = 0; i < n; ++i) {
            const OrderRow& r = event_store().history_at(i);
            const AssetLabel asset = lookup_asset(ctx.universe, r.asset, ctx.sz_decimals);
            // `sz` on a terminal update is what was left UNFILLED, so the original size is the
            // order's size and the difference is what filled. Reporting `sz` as "Size" made a
            // fully filled order read as an order for nothing.
            const Qty size = r.orig_sz > 0 ? r.orig_sz : r.sz;
            const Qty filled = r.orig_sz > 0 && r.orig_sz >= r.sz ? r.orig_sz - r.sz : 0;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_time_ms(r.time_ms, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(type_text(r));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(asset.coin);

            ImGui::TableNextColumn();
            ImGui::TextColored(r.is_buy ? kColorBid : kColorAsk, "%s", direction_text(r));

            ImGui::TableNextColumn();
            if (size > 0)
                ImGui::TextUnformatted(format_qty(size, asset.sz_decimals, buf, sizeof(buf)));
            else
                ImGui::TextDisabled("--");

            ImGui::TableNextColumn();
            if (filled > 0)
                ImGui::TextUnformatted(format_qty(filled, asset.sz_decimals, buf, sizeof(buf)));
            else
                ImGui::TextDisabled("--");

            ImGui::TableNextColumn();
            if (r.is_trigger && r.is_market_trigger) {
                // The limit price on a market trigger is a slippage bound ~5% from the market,
                // not a price the order aims for. Printing it in a Price column invites it to
                // be read as one.
                ImGui::TextDisabled("Market");
            } else if (r.px > 0) {
                ImGui::TextUnformatted(format_px(r.px, asset.sz_decimals, buf, sizeof(buf)));
            } else {
                ImGui::TextDisabled("--");
            }

            ImGui::TableNextColumn();
            if (r.reduce_only)
                ImGui::TextUnformatted("Yes");
            else
                ImGui::TextDisabled("--");

            ImGui::TableNextColumn();
            trigger_condition(r, asset.sz_decimals, buf, sizeof(buf));
            if (r.is_trigger)
                ImGui::TextColored(r.tpsl == PC_TPSL_TP ? kColorBid : kColorWarning, "%s", buf);
            else
                ImGui::TextDisabled("%s", buf);

            ImGui::TableNextColumn();
            const bool bad = r.status == PC_ORD_REJECTED || r.status == PC_ORD_MARGIN_CANCELED;
            ImGui::TextColored(bad ? kColorAsk : kColorTextPrimary, "%s", status_text(r.status));
            // The rejection reason has nowhere else to go now that Detail is gone, and it is
            // the whole point of a rejected row.
            if (r.err[0] != '\0' && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", r.err);

            ImGui::TableNextColumn();
            if (r.oid != 0)
                ImGui::Text("%llu", static_cast<unsigned long long>(r.oid));
            else
                ImGui::TextDisabled("--");  // rejected before the venue assigned one

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace pc::ui
