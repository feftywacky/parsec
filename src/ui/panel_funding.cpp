#include <imgui.h>

#include <cstdint>

#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {

void draw_funding(PanelContext& ctx) {
    if (!ImGui::Begin(kWindowFunding)) {
        ImGui::End();
        return;
    }

    // Only the *current* funding rate for the active instrument is available here, from the
    // activeAssetCtx feed (md::AssetCtx::funding_rate_1e8()). Per-fill funding payment history
    // (userFunding) has no path to the UI yet: there is no PC_FETCH_USER_FUNDING result event
    // in app::UiEventKind and no funding-history snapshot in app::PortfolioSnapshot, so the
    // historical table below is necessarily empty until that plumbing exists.
    if (!ctx.instrument.valid()) {
        ImGui::TextDisabled("No instrument selected.");
        ImGui::End();
        return;
    }

    char buf[32];
    // funding_rate_1e8() is already a kScale-scaled fraction, exactly what format_pct wants.
    format_pct(ctx.instrument.ctx.funding_rate_1e8(), 4, buf, sizeof(buf));
    ImGui::Text("Current funding rate:");
    ImGui::SameLine();
    ImGui::TextColored(ctx.instrument.ctx.funding_rate_1e8() >= 0 ? kColorBid : kColorAsk, "%s",
                       buf);

    // Countdown to the next funding settlement (see ms_to_next_funding in panels.hpp). Also
    // shown in the header strip; both read the same helper off the same frame clock.
    const unsigned total_s = static_cast<unsigned>(ms_to_next_funding(ctx.now_ms) / 1000ULL);
    ImGui::Text("Next funding in:");
    ImGui::SameLine();
    ImGui::TextColored(total_s <= 60 ? kColorWarning : kColorTextPrimary, "%02u:%02u:%02u",
                       total_s / 3600U, (total_s / 60U) % 60U, total_s % 60U);

    ImGui::Separator();
    ImGui::TextDisabled("No funding payment history available in this session.");
    ImGui::TextDisabled("(userFunding has no path to the UI yet -- see this pass's report.)");

    ImGui::End();
}

}  // namespace pc::ui
