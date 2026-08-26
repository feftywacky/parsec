#include <imgui.h>

#include <cctype>
#include <cfloat>
#include <cstdint>
#include <cstdio>

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

// --- Local latency readout: peak-hold ------------------------------------------------------
//
// Two problems make the raw engine figures unreadable, and they pull in opposite directions.
// Shown in milliseconds they are all "0.00" -- the engine loop runs three orders of magnitude
// below the venue's scale, so every digit rounds away. Shown in microseconds they carry real
// digits but change every frame, and a number that redraws 60 times a second cannot be read at
// all; the eye only gets an impression of "some digits".
//
// Fixing one without the other is impossible, so both are fixed at once: the unit is ALWAYS
// microseconds (no per-frame scale switching -- these fields are compared against each other,
// and a column that silently changes unit cannot be), and the displayed value is latched.
// Each figure accumulates its PEAK over a 500ms window; at the end of the window the peak
// becomes the displayed value and the accumulator resets. So the strip updates twice a second,
// slowly enough to actually read, and what it shows is the worst case in the window rather than
// whichever sample happened to land on the frame that drew -- which is the number that matters
// anyway. A spike can no longer flash past between two redraws.
constexpr uint64_t kLatchWindowNs = 500'000'000;

struct LatchedUs {
    uint64_t shown{};  // held for the whole window; what gets drawn
    uint64_t peak{};   // accumulating for the window in progress
};

struct LocalLatencyLatch {
    LatchedUs age;
    LatchedUs tick;
    LatchedUs tick_max;
    LatchedUs cmd;
    uint32_t batch_shown{};
    uint32_t batch_peak{};
    uint64_t window_start_ns{};
};
LocalLatencyLatch g_latency;

// Peaks are folded on every frame; the swap to `shown` happens only when the window closes.
void latch_observe(LatchedUs& v, uint64_t sample_us) noexcept {
    if (sample_us > v.peak)
        v.peak = sample_us;
}
void latch_roll(LatchedUs& v) noexcept {
    v.shown = v.peak;
    v.peak = 0;
}

// Fixed unit, thousands-separated so a six-digit microsecond figure stays scannable. Never
// switches to ms: see the comment above.
struct LatencyText {
    char buf[24]{};
};
LatencyText format_us(uint64_t us) noexcept {
    LatencyText out;
    if (us < 1000) {
        std::snprintf(out.buf, sizeof(out.buf), "%lluus", static_cast<unsigned long long>(us));
    } else {
        std::snprintf(out.buf, sizeof(out.buf), "%llu,%03lluus",
                      static_cast<unsigned long long>(us / 1000),
                      static_cast<unsigned long long>(us % 1000));
    }
    return out;
}

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
            "VENUE -- WebSocket ping/pong ROUND TRIP, the only true network figures here.\n"
            "Both are measured the same way (we send a ping, the venue echoes a pong, we\n"
            "time the loop) because that is the only latency a client can measure at all:\n"
            "one-way timing would need our clock and the venue's to agree, and they do not.\n"
            "The two sockets are separate TCP connections, so they are timed separately.\n\n"
            "mkt:  market socket (l2Book/bbo/assetCtx). Nothing is SENT on this socket --\n"
            "      we subscribe once and the venue pushes. So a quote only travels the\n"
            "      venue->us half, and arrives roughly mkt/2 old. The ping is sent purely\n"
            "      to measure; halving the round trip is how we price the one-way push.\n"
            "usr:  user socket (fills, order acks). Here the round trip is real work, not\n"
            "      an estimate: an order goes out and its ack comes back, so placing one\n"
            "      costs the FULL usr rtt before you know it landed.\n\n"
            "The rest are push CADENCES: how often the venue sends, not transit time.\n"
            "bbo:  1 level,   ~86ms on mainnet BTC -- sets the ladder's touch.\n"
            "fast: 5 levels,  ~530ms  (l2Book fast:true).\n"
            "deep: 20 levels, ~5.4s   (default l2Book) -- fills the tail only.\n"
            "The ladder composes all three, so the touch is not gated on the slow feed.");

    // --- Local latency: what this process costs, kept visually separate from the venue
    // figures above so a slow tick is never read as a slow venue. ---
    // Snapshot age: how stale everything on screen is relative to the engine's own state.
    // Published on the engine's monotonic clock, which is the same clock this reads.
    const uint64_t now_ns = monotonic_ns();
    const uint64_t snap_age_us = safety.publish_mono_ns != 0 && now_ns > safety.publish_mono_ns
                                     ? (now_ns - safety.publish_mono_ns) / 1000
                                     : 0;

    // Fold this frame's samples into the window's peaks, then roll the window if it has closed.
    latch_observe(g_latency.age, snap_age_us);
    latch_observe(g_latency.tick, safety.engine_tick_us);
    latch_observe(g_latency.tick_max, safety.engine_tick_max_us);
    latch_observe(g_latency.cmd, safety.engine_cmd_us);
    if (safety.engine_batch_events > g_latency.batch_peak)
        g_latency.batch_peak = safety.engine_batch_events;
    if (g_latency.window_start_ns == 0)
        g_latency.window_start_ns = now_ns;
    if (now_ns - g_latency.window_start_ns >= kLatchWindowNs) {
        latch_roll(g_latency.age);
        latch_roll(g_latency.tick);
        latch_roll(g_latency.tick_max);
        latch_roll(g_latency.cmd);
        g_latency.batch_shown = g_latency.batch_peak;
        g_latency.batch_peak = 0;
        g_latency.window_start_ns = now_ns;
    }

    ImGui::SameLine(0.0F, 24.0F);
    ImGui::BeginGroup();

    // Ordered by what a trader can actually act on. Snapshot age comes first: it is the only
    // figure here that describes what is on the screen right now rather than what the engine
    // did some time ago, and it is the one that goes bad first when anything upstream stalls.
    ImGui::TextColored(g_latency.age.shown > 100'000 ? kColorWarning : kColorTextPrimary,
                       "age %s", format_us(g_latency.age.shown).buf);
    ImGui::SameLine();

    // EWMA / worst case. The EWMA alone says the loop is idle-fast, which is never in doubt;
    // the max is the one that costs a quote, so it drives the warning colour.
    ImGui::TextColored(g_latency.tick_max.shown > 5'000 ? kColorWarning : kColorTextMuted,
                       "| tick %s/%s", format_us(g_latency.tick.shown).buf,
                       format_us(g_latency.tick_max.shown).buf);
    ImGui::SameLine();

    if (g_latency.cmd.shown == 0)
        ImGui::TextColored(kColorTextMuted, "| cmd --");
    else
        ImGui::TextColored(g_latency.cmd.shown > 5'000 ? kColorWarning : kColorTextMuted,
                           "| cmd %s", format_us(g_latency.cmd.shown).buf);
    ImGui::SameLine();

    // Not a latency figure, and kept only because it is the one that makes `tick` readable: a
    // high tick next to a high batch is a busy engine, next to a low batch it is a slow one.
    ImGui::TextColored(kColorTextMuted, "| batch %u", g_latency.batch_shown);
    ImGui::SameLine();

    // The one ring-buffer figure that is a fault rather than a measurement. The engine never
    // blocks on a full event ring -- it drops and counts (UiBridge::push_event) -- so a nonzero
    // count is the only evidence that the UI thread fell behind and lost engine events outright.
    // Sticky for the session by construction, and never muted: this is not a perf curiosity.
    const uint64_t drops = ctx.bridge.dropped_events();
    if (drops != 0) {
        ImGui::TextColored(kColorWarning, "| drop %llu", static_cast<unsigned long long>(drops));
        ImGui::SameLine();
    }
    ImGui::TextColored(kColorTextMuted, "| ring %s", drops == 0 ? "ok" : "LOSS");
    ImGui::EndGroup();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "LOCAL -- this process, not the network. Nothing here crosses the internet.\n\n"
            "All figures are MICROSECONDS, and each is the PEAK over a 500ms window: the\n"
            "strip updates twice a second so it can be read, and holds the worst sample in\n"
            "the window rather than whichever one the drawing frame happened to catch.\n\n"
            "age:  how old this snapshot is -- engine state -> your screen. The figure that\n"
            "      describes what you are looking at; warns above 100,000us (100ms).\n"
            "tick: one engine loop's work (apply events -> UI commands -> timers ->\n"
            "      publish), shown as EWMA / worst case. The pc_poll wait is excluded --\n"
            "      blocking on an idle socket is not work.\n"
            "cmd:  how long a UI command (order, leverage, subscription) waited in the\n"
            "      SPSC ring before the engine picked it up. The local half of an\n"
            "      order's send latency; the venue half is the usr rtt on the left.\n"
            "batch: events applied in one poll; high means busy, not slow.\n"
            "ring: engine -> UI event ring. 'ok' until the engine has to drop an event\n"
            "      because the UI thread fell behind, which is a fault, not a slow frame.");
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
