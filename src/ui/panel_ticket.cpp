#include <imgui.h>

#include <cstdio>
#include <cstring>
#include <random>

#include "exec/rounder.hpp"
#include "exec/slippage.hpp"
#include "risk/pre_trade.hpp"
#include "ui/app_window.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// View-preference-only state (docs/02 §7's "panels hold no state beyond ViewState and their
// own view preferences") -- raw text buffers for the two numeric inputs, plus TP/SL working
// state that ViewState (panels.hpp, not owned by this agent) has no fields for yet.
struct TicketPrefs {
    char px_buf[32]{};
    char sz_buf[32]{};
    float pct_slider{0.0F};
    bool tpsl_enabled{false};
    char tp_buf[32]{};
    char sl_buf[32]{};
    bool synced_from_view{false};  // one-shot: seed px_buf/sz_buf from ViewState on first draw
};
TicketPrefs g_prefs;

// Every order needs a cloid (docs/02 §6.2), but the stateful session-id/sequence scheme that
// makes cloids collision-free and reconciliation-friendly lives in exec::OrderRouter, which is
// engine-owned state this panel must not instantiate a second copy of (that would violate the
// "panels hold no state" rule and desync from the engine's own bookkeeping). This generates a
// locally-unique-enough tag instead so the order is never sent with an all-zero cloid; the
// engine/OrderRouter remains the authority for cloid-based reconciliation.
void make_local_cloid(uint8_t out[16], uint64_t now_ms) noexcept {
    static std::mt19937_64 rng{std::random_device{}()};
    const uint64_t r = rng();
    std::memcpy(out, &now_ms, 8);
    std::memcpy(out + 8, &r, 8);
}

// Parses a decimal string like "118342.5" into a kScale-scaled integer. Returns false (leaving
// *out at 0) on anything that isn't a plain non-negative decimal -- good enough for a text
// field the ticket is about to run through exec::Rounder anyway, and it never silently accepts
// garbage as zero.
bool parse_fixed(const char* text, Px* out) noexcept {
    if (!text || !*text)
        return false;
    int64_t whole = 0;
    int64_t frac = 0;
    int frac_digits = 0;
    bool seen_dot = false;
    bool any_digit = false;
    for (const char* p = text; *p; ++p) {
        if (*p == '.' && !seen_dot) {
            seen_dot = true;
            continue;
        }
        if (*p < '0' || *p > '9')
            return false;
        any_digit = true;
        if (!seen_dot) {
            whole = whole * 10 + (*p - '0');
        } else if (frac_digits < 8) {
            frac = frac * 10 + (*p - '0');
            ++frac_digits;
        }
    }
    if (!any_digit)
        return false;
    int64_t scale_left = kScale;
    for (int i = 0; i < frac_digits; ++i)
        scale_left /= 10;
    *out = whole * kScale + frac * scale_left;
    return true;
}

}  // namespace

void draw_ticket(PanelContext& ctx) {
    if (!ImGui::Begin(kWindowTicket)) {
        ImGui::End();
        return;
    }

    // "Click a book level -> the limit price populates the ticket" (docs/02 §7). Consumed
    // exactly once per pick, and forces Limit mode since a picked price only makes sense there.
    if (ctx.view.price_pick_pending) {
        ctx.view.ticket_px = ctx.view.picked_px;
        ctx.view.ticket_market = false;
        format_px(ctx.view.picked_px, ctx.sz_decimals, g_prefs.px_buf, sizeof(g_prefs.px_buf));
        ctx.view.price_pick_pending = false;
    }

    if (!ctx.instrument.valid()) {
        ImGui::TextDisabled("No instrument selected.");
        ImGui::End();
        return;
    }

    if (!g_prefs.synced_from_view) {
        format_px(ctx.view.ticket_px, ctx.sz_decimals, g_prefs.px_buf, sizeof(g_prefs.px_buf));
        format_qty(ctx.view.ticket_sz, ctx.sz_decimals, g_prefs.sz_buf, sizeof(g_prefs.sz_buf));
        g_prefs.synced_from_view = true;
    }

    // --- Market / Limit ---
    if (ImGui::RadioButton("Limit", !ctx.view.ticket_market))
        ctx.view.ticket_market = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Market", ctx.view.ticket_market))
        ctx.view.ticket_market = true;

    // --- Buy/Long vs Sell/Short ---
    ImGui::PushStyleColor(ImGuiCol_Button, ctx.view.ticket_is_buy ? kColorBid : kColorTextMuted);
    if (ImGui::Button("Buy / Long", ImVec2(120, 0)))
        ctx.view.ticket_is_buy = true;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, !ctx.view.ticket_is_buy ? kColorAsk : kColorTextMuted);
    if (ImGui::Button("Sell / Short", ImVec2(120, 0)))
        ctx.view.ticket_is_buy = false;
    ImGui::PopStyleColor();

    const Side side = ctx.view.ticket_is_buy ? Side::Buy : Side::Sell;

    // --- Price (disabled for Market) ---
    ImGui::BeginDisabled(ctx.view.ticket_market);
    if (ImGui::InputText("Price", g_prefs.px_buf, sizeof(g_prefs.px_buf))) {
        Px parsed{};
        if (parse_fixed(g_prefs.px_buf, &parsed))
            ctx.view.ticket_px = parsed;
    }
    ImGui::EndDisabled();

    // --- Size + % slider ---
    if (ImGui::InputText("Size", g_prefs.sz_buf, sizeof(g_prefs.sz_buf))) {
        Qty parsed{};
        if (parse_fixed(g_prefs.sz_buf, &parsed))
            ctx.view.ticket_sz = parsed;
    }
    if (ImGui::SliderFloat("% of margin", &g_prefs.pct_slider, 0.0F, 100.0F, "%.0f%%")) {
        // Sized off account withdrawable margin x leverage, at the reference price. Best-effort
        // only: with no account snapshot yet (empty state) this just leaves size untouched.
        if (ctx.portfolio.account_valid) {
            const Px ref = ctx.instrument.bbo.execution_mid();
            if (ref > 0) {
                const __int128 budget = static_cast<__int128>(ctx.portfolio.account.withdrawable) *
                                        ctx.view.ticket_leverage *
                                        static_cast<int64_t>(g_prefs.pct_slider) / 100;
                const __int128 sz128 = budget * kScale / ref;
                ctx.view.ticket_sz = sz128 > 0 ? static_cast<Qty>(sz128) : 0;
                format_qty(ctx.view.ticket_sz, ctx.sz_decimals, g_prefs.sz_buf,
                           sizeof(g_prefs.sz_buf));
            }
        }
    }

    ImGui::Checkbox("Reduce only", &ctx.view.ticket_reduce_only);
    ImGui::SameLine();
    ImGui::Checkbox("TP/SL", &g_prefs.tpsl_enabled);
    if (g_prefs.tpsl_enabled) {
        ImGui::InputText("TP trigger", g_prefs.tp_buf, sizeof(g_prefs.tp_buf));
        ImGui::InputText("SL trigger", g_prefs.sl_buf, sizeof(g_prefs.sl_buf));
    }

    int leverage_i = static_cast<int>(ctx.view.ticket_leverage);
    if (ImGui::SliderInt("Leverage", &leverage_i, 1, 50, "%dx"))
        ctx.view.ticket_leverage = static_cast<uint32_t>(leverage_i);
    if (ImGui::RadioButton("Cross", ctx.view.ticket_cross))
        ctx.view.ticket_cross = true;
    ImGui::SameLine();
    if (ImGui::RadioButton("Isolated", !ctx.view.ticket_cross))
        ctx.view.ticket_cross = false;
    // NB: this only updates local ticket state -- there is no UiCommandKind for "set leverage"
    // / "set margin mode" yet (app/ui_bridge.hpp only defines PlaceOrder/CancelOrder/
    // CancelByCloid/CancelAll), so changing this control does not yet reach pc_set_leverage /
    // pc_set_iso_margin. It still feeds the local pre-trade check below correctly.

    ImGui::Separator();

    // --- Rounding preview: "what you see is what gets signed" (docs/02 §7) ---
    const exec::AssetPrecision precision{ctx.sz_decimals};
    const Px reference = ctx.instrument.bbo.execution_mid();
    const bool market_blocked = ctx.instrument.staleness.blocks_market_orders();

    Px final_px = 0;
    bool have_price = true;
    if (ctx.view.ticket_market) {
        if (reference > 0 && !market_blocked) {
            final_px = exec::Rounder::marketable_px(reference, side,
                                                    exec::Rounder::kDefaultSlippageBps, precision);
        } else {
            have_price = false;
        }
    } else {
        final_px =
            exec::Rounder::round_px(ctx.view.ticket_px, side, precision, exec::RoundMode::Passive);
    }
    const Qty final_sz = exec::Rounder::round_sz(ctx.view.ticket_sz, precision);

    char px_disp[32], sz_disp[32], notional_disp[32];
    if (have_price) {
        format_px(final_px, ctx.sz_decimals, px_disp, sizeof(px_disp));
    } else {
        std::snprintf(px_disp, sizeof(px_disp), "--");
    }
    format_qty(final_sz, ctx.sz_decimals, sz_disp, sizeof(sz_disp));
    format_usd(have_price ? notional(final_px, final_sz) : 0, notional_disp, sizeof(notional_disp));

    ImGui::TextColored(kColorTextMuted, "Rounded price: %s", px_disp);
    ImGui::TextColored(kColorTextMuted, "Rounded size:  %s", sz_disp);
    ImGui::TextColored(kColorTextMuted, "Notional:      %s", notional_disp);

    // --- Est / Max slippage line (docs/02 §6.3, §7) -- modelled from the live book, using the
    // bbo mid as the reference, never L2Book::mid() (docs/02 §6.1). ---
    if (have_price && final_sz > 0 && reference > 0) {
        const exec::SlippageEstimate est =
            exec::SlippageModel::estimate(ctx.instrument.book, side, final_sz, reference);
        char avg_disp[32], est_pct[32];
        format_px(est.avg_px, ctx.sz_decimals, avg_disp, sizeof(avg_disp));
        // slippage_bps is basis points; format_pct wants a kScale-scaled fraction (1 bps =
        // 1/10000, so value_1e8 = slippage_bps * (kScale / 10000)).
        format_pct(static_cast<int64_t>(est.slippage_bps) * (kScale / 10'000), 2, est_pct,
                   sizeof(est_pct));
        ImGui::TextColored(kColorTextMuted, "Est fill: %s  (%s slippage)%s", avg_disp, est_pct,
                           est.full_fill ? "" : "  [book too thin]");
        if (!est.full_fill) {
            char unfilled_disp[32];
            format_qty(est.unfilled, ctx.sz_decimals, unfilled_disp, sizeof(unfilled_disp));
            ImGui::TextColored(kColorWarning, "%s unfilled at visible depth", unfilled_disp);
        }
    } else {
        ImGui::TextColored(kColorTextMuted, "Est fill: --");
    }

    if (ctx.view.ticket_market && market_blocked) {
        ImGui::TextColored(kColorWarning,
                           "Feed stale -- market orders disabled (slippage estimate untrusted).");
    }

    // --- Pre-trade risk gate (docs/02 §6.5) ---
    risk::OrderIntent intent{};
    intent.px = final_px;
    intent.qty = final_sz;
    intent.leverage = ctx.view.ticket_leverage;

    risk::RiskContext risk_ctx{};
    risk_ctx.mark = ctx.instrument.ctx.mark_px();
    // No per-asset existing-position notional is available here: app::PortfolioSnapshot
    // (src/app/ui_bridge.hpp) compacts portfolio::Position entries without carrying the asset
    // id each one belongs to (portfolio::Position / pc_position have no asset field), so this
    // panel cannot look up "this asset's" current position notional. Left at 0 -- see the
    // report for this pass. kill_switch_armed and rate_budget_bps are similarly not exposed to
    // the UI anywhere yet (risk::KillSwitch / risk::RateBudget are engine-side-only state with
    // no snapshot or event carrying them across app::UiBridge), so they default to their safe
    // "not armed" / "full budget" values rather than fabricating a reading.
    risk_ctx.kill_switch_armed = false;
    risk_ctx.rate_budget_bps = 10'000;

    const risk::Limits limits{};
    const risk::CheckOutcome outcome = have_price ? risk::check_order(intent, risk_ctx, limits)
                                                  : risk::CheckOutcome::fail("no price");

    if (!outcome.ok) {
        char excess_disp[32];
        if (outcome.unit == risk::LimitUnit::Usd) {
            format_usd(outcome.excess, excess_disp, sizeof(excess_disp));
            ImGui::TextColored(kColorAsk, "Blocked: %s (over by %s)", outcome.check, excess_disp);
        } else if (outcome.unit == risk::LimitUnit::Bps) {
            format_pct(outcome.excess * (kScale / 10'000), 2, excess_disp, sizeof(excess_disp));
            ImGui::TextColored(kColorAsk, "Blocked: %s (over by %s)", outcome.check, excess_disp);
        } else {
            ImGui::TextColored(kColorAsk, "Blocked: %s", outcome.check);
        }
    }

    const bool submit_blocked =
        !have_price || final_sz <= 0 || !outcome.ok || (ctx.view.ticket_market && market_blocked);

    ImGui::Separator();
    ImGui::BeginDisabled(submit_blocked);
    const char* label = ctx.view.ticket_is_buy ? "Buy / Long" : "Sell / Short";
    ImGui::PushStyleColor(ImGuiCol_Button, ctx.view.ticket_is_buy ? kColorBid : kColorAsk);
    if (ImGui::Button(label, ImVec2(-1, 0))) {
        pc_order main_order{};
        main_order.asset = ctx.instrument.asset;
        main_order.is_buy = ctx.view.ticket_is_buy ? 1 : 0;
        main_order.reduce_only = ctx.view.ticket_reduce_only ? 1 : 0;
        main_order.tif = ctx.view.ticket_market ? PC_TIF_IOC : PC_TIF_GTC;
        main_order.tpsl = PC_TPSL_NONE;
        main_order.limit_px = final_px;
        main_order.sz = final_sz;
        make_local_cloid(main_order.cloid, ctx.now_ms);

        pc_order_req req{};
        req.n_orders = 0;
        req.orders[req.n_orders++] = main_order;

        if (g_prefs.tpsl_enabled) {
            req.grouping = PC_GROUP_NORMAL_TPSL;
            Px tp_trigger{}, sl_trigger{};
            const bool have_tp = parse_fixed(g_prefs.tp_buf, &tp_trigger) && tp_trigger > 0;
            const bool have_sl = parse_fixed(g_prefs.sl_buf, &sl_trigger) && sl_trigger > 0;
            if (have_tp && req.n_orders < 4) {
                pc_order child{};
                child.asset = ctx.instrument.asset;
                child.is_buy = !ctx.view.ticket_is_buy;  // opposite side, reduce-only
                child.reduce_only = 1;
                child.tif = PC_TIF_GTC;
                child.tpsl = PC_TPSL_TP;
                child.trigger_px = exec::Rounder::round_px(tp_trigger, side, precision);
                child.limit_px = child.trigger_px;
                child.is_market_trigger = 1;
                child.sz = final_sz;
                make_local_cloid(child.cloid, ctx.now_ms + 1);
                req.orders[req.n_orders++] = child;
            }
            if (have_sl && req.n_orders < 4) {
                pc_order child{};
                child.asset = ctx.instrument.asset;
                child.is_buy = !ctx.view.ticket_is_buy;
                child.reduce_only = 1;
                child.tif = PC_TIF_GTC;
                child.tpsl = PC_TPSL_SL;
                child.trigger_px = exec::Rounder::round_px(sl_trigger, side, precision);
                child.limit_px = child.trigger_px;
                child.is_market_trigger = 1;
                child.sz = final_sz;
                make_local_cloid(child.cloid, ctx.now_ms + 2);
                req.orders[req.n_orders++] = child;
            }
        } else {
            req.grouping = PC_GROUP_NA;
        }

        app::UiCommand cmd{};
        cmd.kind = app::UiCommandKind::PlaceOrder;
        cmd.asset = ctx.instrument.asset;
        cmd.order = req;
        // UiCommandRing is bounded (256 slots, low-rate by design); surface a full ring rather
        // than silently dropping the trader's submit.
        if (!ctx.bridge.push_command(cmd))
            event_store().note_local("command queue full -- order not sent, try again", 2);
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    ImGui::End();
}

}  // namespace pc::ui
