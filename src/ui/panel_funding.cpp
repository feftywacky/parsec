#include <imgui.h>

#include <cstdint>

#include "ui/app_window.hpp"
#include "ui/asset_lookup.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {

void draw_funding(PanelContext& ctx) {
    event_store().drain(ctx.bridge);

    if (!ImGui::Begin(kWindowFunding)) {
        ImGui::End();
        return;
    }

    char buf[48];

    // --- The current rate for the instrument on screen, and when it next settles ----------
    if (ctx.instrument.valid()) {
        // funding_rate_1e8() is already a kScale-scaled fraction, exactly what format_pct wants.
        format_pct(ctx.instrument.ctx.funding_rate_1e8(), 4, buf, sizeof(buf));
        ImGui::TextDisabled("Current rate");
        ImGui::SameLine();
        ImGui::TextColored(ctx.instrument.ctx.funding_rate_1e8() >= 0 ? kColorBid : kColorAsk,
                           "%s", buf);
        ImGui::SameLine();

        // Countdown to the next settlement (see ms_to_next_funding in panels.hpp). Also shown
        // in the header strip; both read the same helper off the same frame clock.
        const unsigned total_s = static_cast<unsigned>(ms_to_next_funding(ctx.now_ms) / 1000ULL);
        ImGui::TextDisabled("|  Next in");
        ImGui::SameLine();
        ImGui::TextColored(total_s <= 60 ? kColorWarning : kColorTextPrimary, "%02u:%02u:%02u",
                           total_s / 3600U, (total_s / 60U) % 60U, total_s % 60U);
        ImGui::SameLine();
    }

    // Net over the payments held. Signed the way the venue signs them: positive is received.
    const Usd net = event_store().funding_total();
    ImGui::TextDisabled("|  Net paid/received");
    ImGui::SameLine();
    format_usd_fine(net, buf, sizeof(buf));
    ImGui::TextColored(net >= 0 ? kColorBid : kColorAsk, "%s", buf);
    ImGui::Separator();

    const size_t n = event_store().funding_count();
    if (n == 0) {
        ImGui::TextDisabled("No funding payments.");
        ImGui::TextDisabled(
            "(The last 30 days, pulled once the account snapshot proves the session is\n"
            "signed in, plus anything settled since. A position held for less than an hour\n"
            "never pays funding, so an empty table here is a normal state, not a fault.)");
        ImGui::End();
        return;
    }

    if (ImGui::BeginTable(
            "funding", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
                ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Time");
        ImGui::TableSetupColumn("Market");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Side");
        ImGui::TableSetupColumn("Payment");
        ImGui::TableSetupColumn("Rate");
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < n; ++i) {
            const FundingRow& r = event_store().funding_at(i);
            const AssetLabel asset = lookup_asset(ctx.universe, r.asset, ctx.sz_decimals);
            const bool is_long = r.szi >= 0;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_time_ms(r.time_ms, buf, sizeof(buf)));

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(asset.coin);

            ImGui::TableNextColumn();
            // Magnitude: the direction is the Side column's job, and a signed size beside an
            // explicit "Short" reads as a double negative.
            ImGui::Text("%s %s",
                        format_qty(r.szi < 0 ? -r.szi : r.szi, asset.sz_decimals, buf,
                                   sizeof(buf)),
                        asset.coin);

            ImGui::TableNextColumn();
            ImGui::TextColored(is_long ? kColorBid : kColorAsk, "%s", is_long ? "Long" : "Short");

            ImGui::TableNextColumn();
            // Signed as the venue reports it (pc_funding: negative is paid, positive is
            // received), so green is money in and red is money out with no reinterpretation.
            format_usd_fine(r.usdc, buf, sizeof(buf));
            ImGui::TextColored(r.usdc >= 0 ? kColorBid : kColorAsk, "%s", buf);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(format_pct(r.rate_1e8, 4, buf, sizeof(buf)));
            if (r.n_samples > 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Averaged over %u samples in the hour.", r.n_samples);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

}  // namespace pc::ui
