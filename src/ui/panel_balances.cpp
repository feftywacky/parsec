#include <imgui.h>

#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {

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

    // Displayed exactly as the venue reports it (docs/02 §6.4 / docs/07 Phase 3 acceptance:
    // "must match the Hyperliquid web UI exactly") -- no recomputation of anything the venue
    // already gave us in pc_account.
    const pc_account& a = ctx.portfolio.account;
    char buf[32];

    auto row = [&](const char* label, pc::Usd v) {
        format_usd(v, buf, sizeof(buf));
        ImGui::Text("%-22s", label);
        ImGui::SameLine();
        ImGui::Text("%s", buf);
    };

    row("Account value", a.account_value);
    row("Total margin used", a.total_margin_used);
    row("Total notional position", a.total_ntl_pos);
    row("Withdrawable", a.withdrawable);
    row("Maintenance margin", a.cross_maintenance_margin);

    ImGui::End();
}

}  // namespace pc::ui
