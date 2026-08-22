#include <imgui.h>

#include <cstddef>
#include <cstdio>
#include <ctime>

#include "md/trade_tape.hpp"
#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// Scratch buffer for TradeTape::copy_recent()'s output -- function-local static rather than a
// per-frame allocation (docs/05 §5.3) or a stack array reconstructed every call.
pc_trade g_trade_buf[md::TradeTape::kCapacity];

}  // namespace

void draw_trades(PanelContext& ctx) {
    if (ImGui::Begin(kWindowTrades)) {
        if (ctx.market == nullptr || !ctx.instrument.valid()) {
            ImGui::TextDisabled("waiting for market data...");
            ImGui::End();
            return;
        }

        const size_t n = ctx.market->trades.copy_recent(g_trade_buf, md::TradeTape::kCapacity);
        if (n == 0) {
            ImGui::TextDisabled("no trades yet");
            ImGui::End();
            return;
        }

        if (ImGui::BeginTable("##trades", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Price");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            char buf[32];
            for (size_t i = 0; i < n; ++i) {
                const pc_trade& t = g_trade_buf[i];  // already newest-first (TradeTape contract)
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                format_px(t.px, ctx.sz_decimals, buf, sizeof(buf));
                ImGui::TextColored(t.is_buy ? kColorBid : kColorAsk, "%s", buf);

                ImGui::TableSetColumnIndex(1);
                format_qty(t.sz, ctx.sz_decimals, buf, sizeof(buf));
                ImGui::TextColored(kColorTextMuted, "%s", buf);

                ImGui::TableSetColumnIndex(2);
                // pc_trade::time_ms is the venue block clock for this print, in unix ms.
                // Rendered as local wall-clock HH:MM:SS, which is what a tape is read against.
                const std::time_t secs = static_cast<std::time_t>(t.time_ms / 1000);
                std::tm tm_buf{};
                if (t.time_ms != 0 && localtime_r(&secs, &tm_buf) != nullptr) {
                    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min,
                                  tm_buf.tm_sec);
                } else {
                    std::snprintf(buf, sizeof(buf), "--:--:--");
                }
                ImGui::TextColored(kColorTextMuted, "%s", buf);
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

}  // namespace pc::ui
