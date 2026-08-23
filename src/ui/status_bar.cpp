#include <imgui.h>

#include "md/staleness.hpp"
#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"

namespace pc::ui {
namespace {

const char* conn_state_text(uint8_t state) noexcept {
    switch (state) {
        case PC_CONN_CONNECTED:
            return "connected";
        case PC_CONN_RECONNECTING:
            return "reconnecting";
        case PC_CONN_DISCONNECTED:
        default:
            return "disconnected";
    }
}

ImVec4 conn_state_color(uint8_t state) noexcept {
    switch (state) {
        case PC_CONN_CONNECTED:
            return kColorBid;
        case PC_CONN_RECONNECTING:
            return kColorWarning;
        case PC_CONN_DISCONNECTED:
        default:
            return kColorAsk;
    }
}

void draw_conn_badge(const char* label, bool has_data, const pc_conn& c) noexcept {
    ImGui::Text("%s:", label);
    ImGui::SameLine();
    if (!has_data) {
        ImGui::TextColored(kColorTextMuted, "no data");
        return;
    }
    ImGui::TextColored(conn_state_color(c.state), "%s", conn_state_text(c.state));
    if (c.reconnects > 0) {
        ImGui::SameLine();
        ImGui::TextColored(kColorTextMuted, "(%u reconnects)", c.reconnects);
    }
}

}  // namespace

void draw_status_bar(PanelContext& ctx) {
    // This panel drains app::UiEventRing (see ui_event_store.hpp) because AppWindow itself
    // (src/ui/app_window.cpp, not owned by this agent) does not. That is also where the
    // per-frame toast rendering below comes from: nothing else in the current panel set claims
    // toasts, so they surface here as the natural home for "system status".
    event_store().drain(ctx.bridge);

    if (!ImGui::Begin(kWindowStatus)) {
        ImGui::End();
        return;
    }

    // --- Network badge: always visible, distinct colour per network (docs/07 Phase 7 #5) ---
    // Read from the engine's resolved network rather than assumed. The badge is a safety
    // control, so it must reflect what the engine actually connected to.
    ImGui::TextColored(ctx.mainnet ? kColorAsk : kColorWarning, "%s",
                       ctx.mainnet ? "MAINNET" : "TESTNET");
    ImGui::SameLine();
    ImGui::TextColored(kColorTextMuted, "|");
    ImGui::SameLine();

    // --- Connection state, per socket ---
    draw_conn_badge("market", event_store().has_market_conn(), event_store().market_conn());
    ImGui::SameLine();
    draw_conn_badge("user", event_store().has_user_conn(), event_store().user_conn());

    ImGui::Separator();

    // --- Per-feed staleness, all signals from md::AssetStalenessState (docs/03 §W6, docs/07
    // Phase 5: "surface it in the status bar as a real indicator, not a hidden log line") ---
    if (!ctx.instrument.valid()) {
        ImGui::TextDisabled("Staleness: no instrument selected.");
    } else {
        const md::AssetStalenessState& st = ctx.instrument.staleness;
        auto signal = [](const char* label, uint32_t bits) {
            char desc[64];
            md::describe_staleness(bits, desc, sizeof(desc));
            ImGui::TextColored(bits == md::kStaleNone ? kColorTextMuted : kColorWarning, "%s: %s",
                               label, desc);
            ImGui::SameLine();
        };
        signal("l2Book", st.l2_signals);
        signal("bbo", st.bbo_signals);
        signal("assetCtx", st.asset_ctx_signals);
        signal("cross-feed", st.cross_feed_signals);
        signal("L1 clock", st.l1_signals);
        ImGui::NewLine();

        if (st.blocks_market_orders()) {
            ImGui::TextColored(kColorAsk,
                               "Market orders disabled: execution feed staleness detected.");
        }
    }

    ImGui::Separator();

    // --- Rate budget / dead-man's-switch / kill switch / reconciler (docs/07 Phase 5) ---
    // Read straight off app::UiBridge rather than through PanelContext: PanelContext is shared
    // by every panel and adding a field to it is out of scope for this pass (see the report),
    // but the bridge itself is already reachable here as ctx.bridge, and load_safety() is just
    // another seqlock load like the instrument/universe/portfolio snapshots.
    app::SafetySnapshot safety{};
    ctx.bridge.load_safety(safety);

    if (safety.rate_budget_remaining < 0) {
        ImGui::TextColored(kColorTextMuted, "Rate budget: no data yet");
    } else {
        const ImVec4 color = safety.rate_budget_bps < 1'000   ? kColorAsk
                             : safety.rate_budget_bps < 3'000 ? kColorWarning
                                                              : kColorTextMuted;
        ImGui::TextColored(color, "Rate budget: %lld remaining (%d.%d%%)",
                           static_cast<long long>(safety.rate_budget_remaining),
                           safety.rate_budget_bps / 100, safety.rate_budget_bps % 100);
    }

    if (safety.dms_unavailable) {
        ImGui::TextColored(kColorWarning,
                           "Dead man's switch: unavailable until $1M account volume");
    } else if (!safety.dms_active) {
        ImGui::TextColored(kColorTextMuted,
                           "Dead man's switch: inactive (no authenticated session)");
    } else {
        const int64_t remaining_ms =
            static_cast<int64_t>(safety.dms_deadline_ms) - static_cast<int64_t>(ctx.now_ms);
        ImGui::TextColored(remaining_ms < 10'000 ? kColorWarning : kColorTextMuted,
                           "Dead man's switch: cancel-all deadline in %lldms (%u/10 triggers "
                           "left today)",
                           static_cast<long long>(remaining_ms),
                           safety.dms_triggers_remaining_today);
    }

    if (safety.kill_switch_armed)
        ImGui::TextColored(kColorAsk, "KILL SWITCH ARMED -- new orders are blocked");

    if (safety.reconcile_divergence_count > 0) {
        ImGui::TextColored(safety.reconcile_alarm ? kColorAsk : kColorWarning,
                           "Reconciler: %u divergence(s) this session%s",
                           safety.reconcile_divergence_count,
                           safety.reconcile_alarm ? " -- ALARM (repeated divergence)" : "");
    }

    ImGui::Separator();

    // --- Recent toasts ---
    const size_t toast_n = event_store().toast_count();
    if (toast_n == 0) {
        ImGui::TextDisabled("No recent activity.");
    } else {
        ImGui::Text("Recent activity:");
        const size_t show = toast_n < 8 ? toast_n : 8;
        for (size_t i = toast_n - show; i < toast_n; ++i) {
            const ToastRow& t = event_store().toast_at(i);
            const ImVec4 color =
                t.severity >= 2 ? kColorAsk : (t.severity == 1 ? kColorWarning : kColorTextMuted);
            ImGui::TextColored(color, "%s", t.text);
        }
    }
    if (event_store().dropped_events() > 0) {
        ImGui::TextColored(kColorWarning, "%llu UI events dropped (ring full)",
                           static_cast<unsigned long long>(event_store().dropped_events()));
    }

    ImGui::End();
}

}  // namespace pc::ui
