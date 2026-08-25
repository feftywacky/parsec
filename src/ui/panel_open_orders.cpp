#include <imgui.h>

#include "ui/app_window.hpp"
#include "ui/asset_lookup.hpp"
#include "ui/order_label.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {

void draw_open_orders(PanelContext& ctx) {
    event_store().drain(ctx.bridge);  // see ui_event_store.hpp: safe to call from any panel

    if (!ImGui::Begin(kWindowOpenOrders)) {
        ImGui::End();
        return;
    }

    // Cancel-all needs no per-order data, so it is always available -- and per docs/07 Phase 5
    // it takes no confirmation (unlike kill-switch flatten).
    if (ImGui::Button("Cancel All")) {
        app::UiCommand cmd{};
        cmd.kind = app::UiCommandKind::CancelAll;
        if (!ctx.bridge.push_command(cmd))
            event_store().note_local("command queue full -- cancel-all not sent, try again", 2);
    }
    ImGui::Separator();

    const size_t n = event_store().open_order_count();
    if (n == 0) {
        ImGui::TextDisabled("No resting orders this session.");
        ImGui::TextDisabled(
            "(Built from the orderUpdates event stream only -- no REST openOrders backfill is "
            "wired to this panel yet, so orders resting before this session started will not "
            "appear here until they next update.)");
        ImGui::End();
        return;
    }

    // Column set deliberately mirrors Order History's, in the same order: a resting stop and
    // the same stop once it has fired are read one above the other, and the two panels
    // describing it differently is worse than either description being imperfect.
    if (ImGui::BeginTable(
            "open_orders", 10,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("Market");
        ImGui::TableSetupColumn("OID");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Direction");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Trigger Condition");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Orig Size");
        ImGui::TableSetupColumn("Reduce Only");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        char buf[64];
        for (size_t i = 0; i < n; ++i) {
            const OrderRow& r = event_store().open_order_at(i);
            const AssetLabel asset = lookup_asset(ctx.universe, r.asset, ctx.sz_decimals);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(asset.coin);

            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(r.oid));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(type_text(r));

            ImGui::TableNextColumn();
            ImGui::TextColored(r.is_buy ? kColorBid : kColorAsk, "%s", direction_text(r));

            ImGui::TableNextColumn();
            if (r.is_trigger && r.is_market_trigger) {
                // The limit price on a market trigger is a slippage bound ~5% from the market,
                // not a price the order aims for -- this column used to print it, so a stop
                // resting at 50,000 read as an order at 47,500. Where the order actually sits
                // is the trigger, which the next column states in full.
                ImGui::TextDisabled("Market");
            } else if (r.px > 0) {
                ImGui::TextUnformatted(format_px(r.px, asset.sz_decimals, buf, sizeof(buf)));
            } else {
                ImGui::TextDisabled("--");
            }

            ImGui::TableNextColumn();
            trigger_condition(r, asset.sz_decimals, buf, sizeof(buf));
            if (r.is_trigger)
                ImGui::TextColored(r.tpsl == PC_TPSL_TP ? kColorBid : kColorWarning, "%s", buf);
            else
                ImGui::TextDisabled("%s", buf);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_qty(r.sz, asset.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_qty(r.orig_sz, asset.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            if (r.reduce_only)
                ImGui::TextUnformatted("Yes");
            else
                ImGui::TextDisabled("--");

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Cancel")) {
                // The row carries the asset the order is actually on (UiEvent::asset, threaded
                // through OrderRow), so cancelling a resting order on a coin other than the
                // one currently displayed targets the right instrument.
                app::UiCommand cmd{};
                cmd.kind = app::UiCommandKind::CancelOrder;
                cmd.asset = r.asset != PC_ASSET_NONE ? r.asset : ctx.instrument.asset;
                cmd.oid = r.oid;
                if (!ctx.bridge.push_command(cmd))
                    event_store().note_local("command queue full -- cancel not sent, try again", 2);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace pc::ui
