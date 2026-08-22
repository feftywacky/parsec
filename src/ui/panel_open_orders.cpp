#include <imgui.h>

#include "ui/app_window.hpp"
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

    if (ImGui::BeginTable(
            "open_orders", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("OID");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Price");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Orig Size");
        ImGui::TableSetupColumn("Reduce Only");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        char buf[32];
        for (size_t i = 0; i < n; ++i) {
            const OrderRow& r = event_store().open_order_at(i);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(r.oid));

            ImGui::TableNextColumn();
            ImGui::TextColored(r.is_buy ? kColorBid : kColorAsk, "%s", r.is_buy ? "Buy" : "Sell");

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_px(r.px, ctx.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_qty(r.sz, ctx.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::Text("%s", format_qty(r.orig_sz, ctx.sz_decimals, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::Text("%s", r.reduce_only ? "yes" : "");

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Cancel")) {
                // KNOWN GAP: pc_order_update carries no asset id (see ui_event_store.hpp), so
                // this row's actual asset is unknown; the active instrument is the closest
                // available approximation and will be wrong for a resting order on a different
                // coin than the one currently displayed.
                app::UiCommand cmd{};
                cmd.kind = app::UiCommandKind::CancelOrder;
                cmd.asset = ctx.instrument.asset;
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
