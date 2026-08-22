#include <imgui.h>

#include <cstdint>

#include "core/units.hpp"
#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// (mark - prev_day) / prev_day, as a 1e8-scaled fraction, computed in 128 bits to keep the
// multiply from overflowing before the divide. Returns 0 if prev_day is unknown (0), which
// format_pct then renders as a harmless "0.00%" rather than a divide-by-zero.
int64_t day_change_1e8(Px mark, Px prev_day) noexcept {
    if (prev_day == 0)
        return 0;
    const __int128 delta = static_cast<__int128>(mark) - prev_day;
    const __int128 scaled = delta * kScale / prev_day;
    return static_cast<int64_t>(scaled);
}

}  // namespace

void draw_instruments(PanelContext& ctx) {
    if (ImGui::Begin(kWindowInstruments)) {
        if (ctx.universe.count == 0) {
            ImGui::TextDisabled("loading coin list...");
            ImGui::End();
            return;
        }

        const uint32_t selected_asset = ctx.instrument.valid() ? ctx.instrument.asset
                                                                : ctx.view.active_asset;
        const char* active_name = "Select coin";
        for (uint32_t i = 0; i < ctx.universe.count; ++i) {
            if (ctx.universe.assets[i].asset == selected_asset) {
                active_name = ctx.universe.assets[i].name;
                break;
            }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::SetNextItemWidth(150.0F);
        if (ImGui::BeginCombo("##coin", active_name)) {
            for (uint32_t i = 0; i < ctx.universe.count; ++i) {
                const auto& option = ctx.universe.assets[i];
                const bool selected = option.asset == selected_asset;
                if (ImGui::Selectable(option.name, selected)) {
                    ctx.view.active_asset = option.asset;
                    app::UiCommand command{};
                    command.kind = app::UiCommandKind::SetActiveAsset;
                    command.asset = option.asset;
                    command.interval = ctx.view.interval;
                    (void)ctx.bridge.push_command(command);
                }
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (!ctx.instrument.valid()) {
            ImGui::SameLine(0.0F, 24.0F);
            ImGui::TextDisabled("waiting for market data...");
            ImGui::End();
            return;
        }

        // Single header strip rather than a table: label above value, metrics laid out left
        // to right on one line. A table drew a separate header row, which cost a whole line of
        // vertical space and read as a grid of one row -- this is a status strip, not tabular
        // data, and the chart underneath is what wants the pixels.
        char buf[32];
        const auto& asset = ctx.instrument.ctx;

        auto metric = [&](const char* label, const char* value, const ImVec4& color,
                          float gap = 28.0F) {
            ImGui::SameLine(0.0F, gap);
            ImGui::BeginGroup();
            ImGui::TextColored(kColorTextMuted, "%s", label);
            ImGui::TextColored(color, "%s", value);
            ImGui::EndGroup();
        };

        format_px(asset.mark_px(), ctx.sz_decimals, buf, sizeof(buf));
        metric("Mark", buf, kColorTextPrimary, 24.0F);
        format_px(asset.oracle_px(), ctx.sz_decimals, buf, sizeof(buf));
        metric("Oracle", buf, kColorTextMuted);

        const int64_t chg = day_change_1e8(asset.mark_px(), asset.prev_day_px());
        format_pct(chg, 2, buf, sizeof(buf));
        metric("24h Change", buf, chg < 0 ? kColorAsk : kColorBid);

        format_usd(asset.day_notional_volume(), buf, sizeof(buf));
        metric("24h Volume", buf, kColorTextPrimary);

        // openInterest arrives in COIN units (e.g. 34,421 BTC), not USD -- the strip wants the
        // notional a trader recognises, so convert at the mark here. Display conversion only;
        // AssetCtx keeps the venue's own units.
        format_usd(notional(asset.mark_px(), asset.open_interest()), buf, sizeof(buf));
        metric("Open Interest", buf, kColorTextPrimary);

        // Funding rate and the countdown to its next settlement share one cell, the way the
        // venue's own header presents them.
        const unsigned funding_s = static_cast<unsigned>(ms_to_next_funding(ctx.now_ms) / 1000ULL);
        ImGui::SameLine(0.0F, 28.0F);
        ImGui::BeginGroup();
        ImGui::TextColored(kColorTextMuted, "Funding / Countdown");
        format_pct(asset.funding_rate_1e8(), 4, buf, sizeof(buf));
        ImGui::TextColored(asset.funding_rate_1e8() < 0 ? kColorAsk : kColorBid, "%s", buf);
        ImGui::SameLine();
        ImGui::TextColored(funding_s <= 60 ? kColorWarning : kColorTextPrimary, "%02u:%02u:%02u",
                           funding_s / 3600U, (funding_s / 60U) % 60U, funding_s % 60U);
        ImGui::EndGroup();

        // Feed latency, one column per order-book subscription. All three feed the same
        // ladder (md/book_merge.hpp), and their cadences differ by two orders of magnitude, so
        // a single averaged number would hide exactly the difference that matters: `book` is
        // the deep-but-slow default l2Book, `fast` the 5-level l2Book, `bbo` the top-of-book
        // stream that actually sets how quickly the touch moves. Each shows rolling cadence
        // and age since the last message, and turns amber when its staleness signal fires.
        const auto& st = ctx.instrument.staleness;
        ImGui::SameLine(0.0F, 28.0F);
        ImGui::BeginGroup();
        // Round-trip latency first: it is the one number here that is a true network figure,
        // and the cadences beside it are meaningless without it. A quote is already ~rtt/2 old
        // the instant it lands, since the venue's push travels one way.
        app::SafetySnapshot safety{};
        ctx.bridge.load_safety(safety);
        const float rtt_ms = static_cast<float>(safety.market_rtt_us) / 1000.0F;
        ImGui::TextColored(kColorTextMuted, "RTT / feeds (cadence / age)");
        if (safety.market_rtt_us == 0)
            ImGui::TextColored(kColorTextMuted, "rtt --");
        else
            ImGui::TextColored(rtt_ms > 250.0F ? kColorWarning : kColorTextPrimary, "rtt %.0fms",
                               static_cast<double>(rtt_ms));
        ImGui::SameLine();
        ImGui::TextColored(st.bbo_signals != 0 ? kColorWarning : kColorTextPrimary,
                           "| bbo %u/%ums", st.bbo_cadence_ms, st.bbo_age_ms);
        ImGui::SameLine();
        ImGui::TextColored(kColorTextMuted, "fast %u/%ums", st.l2_fast_cadence_ms,
                           st.l2_fast_age_ms);
        ImGui::SameLine();
        ImGui::TextColored(st.l2_signals != 0 ? kColorWarning : kColorTextMuted, "deep %u/%ums",
                           st.l2_cadence_ms, st.l2_age_ms);
        ImGui::EndGroup();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "rtt:  WebSocket ping/pong round trip -- the only true latency figure here.\n"
                "      Market data travels ONE way, so a quote arrives about rtt/2 old.\n"
                "      An order round trip (send -> ack) costs the full rtt.\n\n"
                "The rest are push cadences: how often the venue sends, not transit time.\n"
                "bbo:  1 level,   ~86ms on mainnet BTC -- sets the ladder's touch.\n"
                "fast: 5 levels,  ~530ms  (l2Book fast:true).\n"
                "deep: 20 levels, ~5.4s   (default l2Book) -- fills the tail only.\n"
                "The ladder composes all three, so the touch is not gated on the slow feed.");

        // docs/09-measurements.md §2.1-2.2: funding/OI/volume/mark/oracle were observed
        // parsing as zero on testnet earlier in development, due to a decimal-precision bug
        // and a nesting bug in the Rust edge parser (both since fixed, per §2.2's
        // `verify_phase1.rs` sample output). If these columns still read 0 at runtime in this
        // build, that is an upstream regression to report against rust/src/codec, not a bug in
        // this panel -- it renders exactly what AssetCtx is given.
    }
    ImGui::End();
}

}  // namespace pc::ui
