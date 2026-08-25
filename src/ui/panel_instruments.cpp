#include <imgui.h>

#include <cctype>
#include <cfloat>
#include <cstdint>

#include "core/time.hpp"
#include "core/units.hpp"
#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

struct InstrumentPrefs {
    char coin_filter[64]{};
    bool coin_combo_open{false};
};
InstrumentPrefs g_prefs;

bool contains_coin_name(const char* name, const char* filter) noexcept {
    if (!filter || !*filter)
        return true;
    if (!name)
        return false;

    for (const char* name_start = name; *name_start; ++name_start) {
        const char* name_it = name_start;
        const char* filter_it = filter;
        while (*name_it && *filter_it &&
               std::tolower(static_cast<unsigned char>(*name_it)) ==
                   std::tolower(static_cast<unsigned char>(*filter_it))) {
            ++name_it;
            ++filter_it;
        }
        if (!*filter_it)
            return true;
    }
    return false;
}

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

void draw_menu_bar_status(PanelContext& ctx) {
    // Network first and unabbreviated. This is a safety control (docs/06 §2): the one place
    // that says out loud whether an order placed from this window spends real money, so it
    // never gets shortened to a colour or an icon.
    const bool live = ctx.instrument.valid() && (ctx.instrument.bbo.has_execution_bid() ||
                                                 ctx.instrument.bbo.has_execution_ask());
    ImGui::TextColored(ctx.mainnet ? kColorTextPrimary : kColorWarning, "hyperliquid %s",
                       ctx.mainnet ? "mainnet" : "testnet");
    ImGui::SameLine(0.0F, 8.0F);
    ImGui::TextColored(live ? kColorBid : kColorWarning, live ? "\xe2\x97\x8f live"
                                                              : "\xe2\x97\x8b connecting");

    if (!ctx.instrument.valid())
        return;

    app::SafetySnapshot safety{};
    ctx.bridge.load_safety(safety);

    // Feed latency, one column per order-book subscription. All three feed the same ladder
    // (md/book_merge.hpp), and their cadences differ by two orders of magnitude, so a single
    // averaged number would hide exactly the difference that matters: `deep` is the
    // deep-but-slow default l2Book, `fast` the 5-level l2Book, `bbo` the top-of-book stream
    // that actually sets how quickly the touch moves. Each shows rolling cadence and age since
    // the last message, and turns amber when its staleness signal fires.
    const auto& st = ctx.instrument.staleness;
    ImGui::SameLine(0.0F, 24.0F);
    ImGui::BeginGroup();
    // Round-trip latency first: it is the one number here that is a true network figure, and
    // the cadences beside it are meaningless without it. A quote is already ~rtt/2 old the
    // instant it lands, since the venue's push travels one way.
    const float rtt_ms = static_cast<float>(safety.market_rtt_us) / 1000.0F;
    const float user_rtt_ms = static_cast<float>(safety.user_rtt_us) / 1000.0F;
    if (safety.market_rtt_us == 0)
        ImGui::TextColored(kColorTextMuted, "mkt --");
    else
        ImGui::TextColored(rtt_ms > 250.0F ? kColorWarning : kColorTextPrimary, "mkt %.0fms",
                           static_cast<double>(rtt_ms));
    ImGui::SameLine();
    // The user socket carries fills and order acks, so its round trip -- not the market
    // socket's -- is what an order actually pays on the way back.
    if (safety.user_rtt_us == 0)
        ImGui::TextColored(kColorTextMuted, "usr --");
    else
        ImGui::TextColored(user_rtt_ms > 250.0F ? kColorWarning : kColorTextPrimary, "usr %.0fms",
                           static_cast<double>(user_rtt_ms));
    ImGui::SameLine();
    ImGui::TextColored(st.bbo_signals != 0 ? kColorWarning : kColorTextPrimary, "| bbo %u/%ums",
                       st.bbo_cadence_ms, st.bbo_age_ms);
    ImGui::SameLine();
    ImGui::TextColored(kColorTextMuted, "fast %u/%ums", st.l2_fast_cadence_ms, st.l2_fast_age_ms);
    ImGui::SameLine();
    ImGui::TextColored(st.l2_signals != 0 ? kColorWarning : kColorTextMuted, "deep %u/%ums",
                       st.l2_cadence_ms, st.l2_age_ms);
    ImGui::EndGroup();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "VENUE -- WebSocket ping/pong round trip, the only true network figures here.\n"
            "mkt:  market socket (l2Book/bbo/assetCtx). Market data travels ONE way, so a\n"
            "      quote arrives about mkt/2 old.\n"
            "usr:  user socket (fills, order acks). An order round trip (send -> ack)\n"
            "      costs the full usr rtt.\n\n"
            "The rest are push CADENCES: how often the venue sends, not transit time.\n"
            "bbo:  1 level,   ~86ms on mainnet BTC -- sets the ladder's touch.\n"
            "fast: 5 levels,  ~530ms  (l2Book fast:true).\n"
            "deep: 20 levels, ~5.4s   (default l2Book) -- fills the tail only.\n"
            "The ladder composes all three, so the touch is not gated on the slow feed.");

    // --- Local latency: what this process costs, kept visually separate from the venue
    // figures above so a slow tick is never read as a slow venue. ---
    ImGui::SameLine(0.0F, 24.0F);
    ImGui::BeginGroup();
    const float tick_ms = static_cast<float>(safety.engine_tick_us) / 1000.0F;
    const float tick_max_ms = static_cast<float>(safety.engine_tick_max_us) / 1000.0F;
    ImGui::TextColored(safety.engine_tick_max_us > 5'000 ? kColorWarning : kColorTextPrimary,
                       "tick %.2f/%.2fms", static_cast<double>(tick_ms),
                       static_cast<double>(tick_max_ms));
    ImGui::SameLine();
    if (safety.engine_cmd_us == 0)
        ImGui::TextColored(kColorTextMuted, "| cmd --");
    else
        ImGui::TextColored(safety.engine_cmd_us > 5'000 ? kColorWarning : kColorTextMuted,
                           "| cmd %.2fms", static_cast<double>(safety.engine_cmd_us) / 1000.0);
    ImGui::SameLine();
    // Snapshot age: how stale everything on screen is relative to the engine's own state.
    // Published on the engine's monotonic clock, which is the same clock this reads.
    const uint64_t now_ns = monotonic_ns();
    const double snap_age_ms = safety.publish_mono_ns != 0 && now_ns > safety.publish_mono_ns
                                   ? static_cast<double>(now_ns - safety.publish_mono_ns) / 1e6
                                   : 0.0;
    ImGui::TextColored(snap_age_ms > 100.0 ? kColorWarning : kColorTextMuted, "| age %.1fms",
                       snap_age_ms);
    ImGui::SameLine();
    ImGui::TextColored(kColorTextMuted, "| batch %u", safety.engine_batch_events);
    ImGui::EndGroup();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "LOCAL -- this process, not the network. Nothing here crosses the internet.\n"
            "tick: one engine loop's work (apply events -> UI commands -> timers ->\n"
            "      publish), shown as EWMA / worst case since the last publish. The\n"
            "      pc_poll wait is excluded -- blocking on an idle socket is not work.\n"
            "cmd:  how long a UI command (order, leverage, subscription) waited in the\n"
            "      SPSC ring before the engine picked it up. The local half of an\n"
            "      order's send latency; the venue half is the usr rtt on the left.\n"
            "age:  how old this snapshot is -- engine state -> your screen.\n"
            "batch: events applied in the last non-empty poll; high means busy, not slow.");
}

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
        const bool combo_open = ImGui::BeginCombo("##coin", active_name);
        const bool combo_just_opened = combo_open && !g_prefs.coin_combo_open;
        if (combo_just_opened) {
            // Start each search from a clean field. This also makes reopening the selector
            // predictable after a previous filtered selection.
            g_prefs.coin_filter[0] = '\0';
        }
        g_prefs.coin_combo_open = combo_open;
        if (combo_open) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (combo_just_opened)
                ImGui::SetKeyboardFocusHere();
            ImGui::InputTextWithHint("##coin_search", "Search token...", g_prefs.coin_filter,
                                     sizeof(g_prefs.coin_filter));

            bool found_match = false;
            for (uint32_t i = 0; i < ctx.universe.count; ++i) {
                const auto& option = ctx.universe.assets[i];
                if (!contains_coin_name(option.name, g_prefs.coin_filter))
                    continue;
                found_match = true;
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
            if (!found_match)
                ImGui::TextDisabled("No matching tokens");
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
