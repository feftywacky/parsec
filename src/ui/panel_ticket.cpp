#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "exec/rounder.hpp"
#include "exec/slippage.hpp"
#include "portfolio/collateral.hpp"
#include "portfolio/order_preview.hpp"
#include "ui/app_window.hpp"
#include "ui/cloid.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"
#include "ui/ui_event_store.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {
namespace {

// How a TP/SL field's text is interpreted: an absolute trigger price, or a percentage.
//
// The percentage is a move of the POSITION, leverage included -- "SL 10%" at 10x means the
// stop fires once the position is down 10%, which only takes a 1% move in the coin. That is
// the number a trader is actually risk-managing against; a percentage that quietly meant the
// coin's own price move understated a levered trade by exactly the leverage factor.
enum class TpSlUnit : uint8_t { Price, Pct };

const char* tpsl_unit_label(TpSlUnit unit) noexcept {
    return unit == TpSlUnit::Pct ? "%" : "price";
}

// View-preference-only state (docs/02 §7's "panels hold no state beyond ViewState and their
// own view preferences") -- raw text buffers for the two numeric inputs, plus TP/SL working
// state that ViewState (panels.hpp, not owned by this agent) has no fields for yet.
struct TicketPrefs {
    char px_buf[32]{};
    char sz_buf[32]{};
    // Which unit `sz_buf` is denominated in: the base coin (BTC, ETH, ...) or USDC notional.
    // The venue only ever takes a base quantity, so USD entry is a display/entry convenience
    // that is converted at `sizing_ref` below; ViewState::ticket_sz stays the base quantity.
    bool size_in_usd{false};
    float pct_slider{0.0F};
    bool tpsl_enabled{false};
    char tp_buf[32]{};
    char sl_buf[32]{};
    // What unit tp_buf/sl_buf are written in. Per-field, because the two legs are naturally
    // thought about differently ("take profit at 120k" but "never lose more than 2%").
    TpSlUnit tp_unit{TpSlUnit::Price};
    TpSlUnit sl_unit{TpSlUnit::Price};
    bool synced_from_view{false};  // one-shot: seed px_buf/sz_buf from ViewState on first draw
    // Transient "the click registered" line under the submit button. The engine's own toasts
    // land in the Status tab, which is docked behind Positions and so is not where a trader is
    // looking the instant they press Buy -- and a submit that produces no visible change at all
    // is indistinguishable from a submit that never fired (which is exactly how the duplicate
    // ImGui id on this button hid itself). Cleared on a timer, not on the next click, so two
    // orders in a row still each say so.
    char submit_note[128]{};
    uint64_t submit_note_ms{};
    bool submit_note_error{};
    // High-water mark of toasts already mirrored into `submit_note`. The venue's rejections
    // ("Insufficient margin", "Price too far from oracle", ...) arrive as toasts and otherwise
    // only render in the Status tab, which is docked behind Positions -- nowhere near where a
    // trader is looking when an order fails. Counting rather than timestamping means a repeated
    // identical rejection still re-announces itself.
    uint64_t seen_toasts{};
    uint32_t settings_asset{PC_ASSET_NONE};
    bool settings_touched{false};
};
TicketPrefs g_prefs;

// Width of the price/size entry fields. They are laid out with a hidden ImGui label so a unit
// selector and the "Mid" shortcut can sit on the same row, which means the width has to be set
// explicitly rather than inherited from the default full-width item.
constexpr float kFieldWidth = 150.0F;

// One take-profit or stop-loss leg, resolved from whatever unit the trader typed it into.
// The panel computes this once per frame and uses the same struct for the preview line and
// for the child order it attaches on submit, so what is displayed is what gets signed.
struct TpSlLeg {
    bool present{};      // the field is non-empty: the trader is asking for this leg
    bool valid{};        // parsed cleanly AND lands on the correct side of the entry price
    Px trigger{};        // absolute trigger, already rounded to the asset's tick
    int64_t px_pct_1e8{};  // signed move of the coin's PRICE from entry, kScale-scaled
    int64_t pct_1e8{};     // the same move as the POSITION sees it: px_pct_1e8 * leverage.
                           // This is the one the `%` field reads and writes.
    Usd pnl{};           // realised PnL if this leg fills at its trigger; signed
    const char* error{};  // why `valid` is false, for the blocked line
};

// Resolves one leg's text field against the order's entry price. `unit` selects how the text
// is read: an absolute price, or a percentage move of the position -- the latter as a magnitude
// whose direction is implied by the leg and the side (a long's take-profit is above entry, its
// stop is below; a short is the mirror).
//
// Percent is measured against the *entry* price -- final_px, which is the limit price for a
// limit order and the touch for a market order -- rather than the mark, so it is a move away
// from the price this ticket is about to pay: the number the trader typed and the only one
// they control. `leverage` scales it, since a position at 10x moves ten times as far as the
// coin does; an absolute trigger is unaffected.
TpSlLeg resolve_tpsl_leg(const char* text, TpSlUnit unit, bool is_tp, bool is_buy, Px entry,
                         Qty sz, uint32_t leverage, exec::AssetPrecision precision) noexcept {
    TpSlLeg leg{};
    if (!text || !*text)
        return leg;
    leg.present = true;
    if (entry <= 0) {
        leg.error = "no entry price yet";
        return leg;
    }
    // A long takes profit above entry and stops out below; a short is the mirror. This single
    // predicate drives both the sign applied to a percentage and the validity check on an
    // absolute price, so the two can never disagree about which side is which.
    const bool above_entry = (is_buy == is_tp);

    const uint32_t lev = leverage > 0 ? leverage : 1;

    Px raw = 0;
    if (unit == TpSlUnit::Pct) {
        Px pct{};
        if (!parse_fixed(text, &pct) || pct <= 0) {
            leg.error = "percent must be a positive number";
            return leg;
        }
        // pct is kScale-scaled percent; /100 turns it into a fraction. The position moves
        // `leverage` times as far as the coin, so a 10% position move at 10x needs only a 1%
        // price move -- hence the extra division. Kept as one 128-bit expression so the
        // leverage division never rounds through an intermediate.
        const __int128 denominator = static_cast<__int128>(100) * kScale * lev;
        const __int128 delta = static_cast<__int128>(entry) * pct / denominator;
        raw = above_entry ? entry + static_cast<Px>(delta) : entry - static_cast<Px>(delta);
        if (raw <= 0) {
            leg.error = "percent is larger than the whole position";
            return leg;
        }
    } else {
        if (!parse_fixed(text, &raw) || raw <= 0) {
            leg.error = "trigger must be a positive price";
            return leg;
        }
    }

    // The child order is reduce-only on the opposite side of the parent, so it is that side's
    // rounding that decides which tick the trigger lands on.
    const Side child_side = is_buy ? Side::Sell : Side::Buy;
    leg.trigger = exec::Rounder::round_px(raw, child_side, precision);
    if (leg.trigger <= 0) {
        leg.error = "trigger rounds to zero at this tick size";
        return leg;
    }
    // Checked after rounding, because rounding is what can push a trigger one tick onto the
    // wrong side of entry -- and the venue rejects the whole grouped order when it is.
    if (above_entry && leg.trigger <= entry) {
        leg.error = is_tp ? "take profit must be above the entry price"
                          : "stop loss must be above the entry price";
        return leg;
    }
    if (!above_entry && leg.trigger >= entry) {
        leg.error = is_tp ? "take profit must be below the entry price"
                          : "stop loss must be below the entry price";
        return leg;
    }

    leg.px_pct_1e8 = static_cast<int64_t>(
        (static_cast<__int128>(leg.trigger - entry) * kScale) / entry);
    // Signed by what the move does to the POSITION, not to the coin: a short's take-profit is a
    // fall in price but a gain on the position, and the `%` field must read as the latter.
    leg.pct_1e8 = portfolio::roe_from_price_move(leg.px_pct_1e8, lev);
    if (!is_buy)
        leg.pct_1e8 = -leg.pct_1e8;
    // Long: gain is trigger - entry. Short: entry - trigger. Both signed, so a stop reads as
    // the loss it is.
    leg.pnl = is_buy ? notional(leg.trigger - entry, sz) : notional(entry - leg.trigger, sz);
    leg.valid = true;
    return leg;
}

// format_usd() is for display -- it emits "$1,234.56", which is not something parse_fixed()
// can read back. The size field round-trips through its own buffer every frame, so USD entry
// needs a plain "1234.56" rendering instead.
void format_usd_plain(Usd value, char* out, size_t cap) noexcept {
    if (value < 0)
        value = 0;
    const int64_t whole = value / kScale;
    const int64_t cents = (value % kScale) / (kScale / 100);
    std::snprintf(out, cap, "%lld.%02lld", static_cast<long long>(whole),
                  static_cast<long long>(cents));
}

Px blended_entry(Px old_entry, Qty old_size, Px new_entry, Qty new_size) noexcept {
    const __int128 old_abs = old_size < 0 ? -static_cast<__int128>(old_size) : old_size;
    const __int128 new_abs = new_size < 0 ? -static_cast<__int128>(new_size) : new_size;
    const __int128 total = old_abs + new_abs;
    if (total <= 0)
        return new_entry;
    return static_cast<Px>((static_cast<__int128>(old_entry) * old_abs +
                             static_cast<__int128>(new_entry) * new_abs) /
                            total);
}

}  // namespace

void draw_ticket(PanelContext& ctx) {
    // Idempotent within a frame (see ui_event_store.hpp): whichever panel draws first pops the
    // ring, the rest are no-ops. Called here so the ticket never renders a frame behind the
    // venue rejection it is supposed to be showing.
    event_store().drain(ctx.bridge);

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

    const app::AssetOption* asset_option = nullptr;
    for (uint32_t i = 0; i < ctx.universe.count; ++i) {
        if (ctx.universe.assets[i].asset == ctx.instrument.asset) {
            asset_option = &ctx.universe.assets[i];
            break;
        }
    }
    const uint32_t max_leverage = asset_option && asset_option->max_leverage > 0
                                      ? asset_option->max_leverage
                                      : 50;
    const bool only_isolated = asset_option && asset_option->only_isolated != 0;

    // Resolved this early because the % slider's ceiling depends on it: the local
    // max-position-notional limit leaves less room for a new order when one is already open.
    const portfolio::Position* current_position = nullptr;
    for (uint32_t i = 0; i < ctx.portfolio.position_count; ++i) {
        if (ctx.portfolio.positions[i].asset == ctx.instrument.asset) {
            current_position = &ctx.portfolio.positions[i];
            break;
        }
    }

    // Seed the ticket's account settings once per asset from the venue's authoritative
    // activeAssetData response. After the user touches a control, the local choice stays put
    // until the next asset switch instead of being overwritten by a late REST refresh.
    if (g_prefs.settings_asset != ctx.instrument.asset) {
        g_prefs.settings_asset = ctx.instrument.asset;
        g_prefs.settings_touched = false;
    }
    if (!g_prefs.settings_touched && ctx.portfolio.asset_data_valid &&
        ctx.portfolio.asset_data_asset == ctx.instrument.asset) {
        if (ctx.portfolio.asset_data.leverage > 0)
            ctx.view.ticket_leverage =
                std::min(ctx.portfolio.asset_data.leverage, max_leverage);
        ctx.view.ticket_cross = ctx.portfolio.asset_data.is_cross != 0 && !only_isolated;
    }
    if (only_isolated)
        ctx.view.ticket_cross = false;

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

    // --- Sizing reference price ---------------------------------------------------------
    // What one unit of the base coin is worth for the size field's USD mode, the % slider and
    // the availability lines. activeAssetData's mark is preferred (it is the price the venue
    // itself sizes against); the asset-context mark and the execution mid are the fallbacks
    // while that response is still in flight.
    const bool sizing_data_ready = ctx.portfolio.account_valid && ctx.portfolio.asset_data_valid &&
                                   ctx.portfolio.asset_data_asset == ctx.instrument.asset;
    Px sizing_ref = sizing_data_ready ? ctx.portfolio.asset_data.mark : 0;
    if (sizing_ref <= 0)
        sizing_ref = ctx.instrument.ctx.mark_px();
    if (sizing_ref <= 0)
        sizing_ref = ctx.instrument.bbo.execution_mid();

    const char* coin_name = asset_option && asset_option->name[0] ? asset_option->name : "coin";

    // The price THIS order transacts at, as opposed to `sizing_ref` (the mark), which is what
    // the venue's own max/availability numbers are denominated in. The size field's USD lens
    // has to use this one: sizing a limit order off the mark printed a dollar figure that
    // disagreed with the "Order value" row further down by the whole distance between the
    // limit price and the mark -- two different answers to "how big is this order" in one panel.
    const Px entry_touch = ctx.view.ticket_is_buy ? ctx.instrument.bbo.execution_ask_px()
                                                  : ctx.instrument.bbo.execution_bid_px();
    Px entry_ref = ctx.view.ticket_market ? entry_touch : ctx.view.ticket_px;
    if (entry_ref <= 0)
        entry_ref = sizing_ref;

    // Rewrites sz_buf from ViewState::ticket_sz in whichever unit is currently selected. The
    // base quantity in ViewState stays the single source of truth -- USD is only ever a lens.
    auto refresh_size_buf = [&]() {
        if (g_prefs.size_in_usd)
            format_usd_plain(entry_ref > 0 ? notional(entry_ref, ctx.view.ticket_sz) : 0,
                             g_prefs.sz_buf, sizeof(g_prefs.sz_buf));
        else
            format_qty(ctx.view.ticket_sz, ctx.sz_decimals, g_prefs.sz_buf,
                       sizeof(g_prefs.sz_buf));
    };

    // --- Price (disabled for Market) ---
    // In Market mode the field is read-only and mirrors the live touch we would cross into,
    // so the number on screen is the price the order is actually expected to fill at.
    if (ctx.view.ticket_market) {
        const Px live_touch = ctx.view.ticket_is_buy ? ctx.instrument.bbo.execution_ask_px()
                                                     : ctx.instrument.bbo.execution_bid_px();
        if (live_touch > 0) {
            // Keep the model in step with the buffer: switching back to Limit then inherits
            // the last live touch instead of leaving a displayed price the ticket never parsed.
            ctx.view.ticket_px = live_touch;
            format_px(live_touch, ctx.sz_decimals, g_prefs.px_buf, sizeof(g_prefs.px_buf));
        }
    }
    const Px live_mid = ctx.instrument.bbo.execution_mid();
    ImGui::BeginDisabled(ctx.view.ticket_market);
    ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::InputText("##ticket_px", g_prefs.px_buf, sizeof(g_prefs.px_buf))) {
        Px parsed{};
        if (parse_fixed(g_prefs.px_buf, &parsed))
            ctx.view.ticket_px = parsed;
    }
    const bool px_field_active = ImGui::IsItemActive();
    ImGui::SameLine();
    // One-click mid: the most common limit price a trader wants and the one that is most
    // annoying to type, since it moves. Writes both the model and the buffer so the rest of
    // the frame (rounding, slippage, risk gate) sees the new price immediately.
    ImGui::BeginDisabled(live_mid <= 0);
    if (ImGui::Button("Mid")) {
        ctx.view.ticket_px = live_mid;
        format_px(live_mid, ctx.sz_decimals, g_prefs.px_buf, sizeof(g_prefs.px_buf));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextUnformatted("Price");

    // Re-read after the price field has been drawn: a keystroke in the price box changes what
    // a USD-denominated size is worth, and the "= $X" line under it must not lag a frame.
    entry_ref = ctx.view.ticket_market ? entry_touch : ctx.view.ticket_px;
    if (entry_ref <= 0)
        entry_ref = sizing_ref;

    // --- Size + unit selector + % slider ---
    ImGui::SetNextItemWidth(kFieldWidth);
    if (ImGui::InputText("##ticket_sz", g_prefs.sz_buf, sizeof(g_prefs.sz_buf))) {
        Px parsed{};
        if (parse_fixed(g_prefs.sz_buf, &parsed)) {
            ctx.view.ticket_sz = g_prefs.size_in_usd
                                     ? portfolio::qty_from_notional(parsed, entry_ref)
                                     : parsed;
        }
    }
    const bool sz_field_active = ImGui::IsItemActive();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0F);
    if (ImGui::BeginCombo("##ticket_sz_unit", g_prefs.size_in_usd ? "USDC" : coin_name)) {
        if (ImGui::Selectable(coin_name, !g_prefs.size_in_usd) && g_prefs.size_in_usd) {
            g_prefs.size_in_usd = false;
            refresh_size_buf();
        }
        if (ImGui::Selectable("USDC", g_prefs.size_in_usd) && !g_prefs.size_in_usd) {
            g_prefs.size_in_usd = true;
            refresh_size_buf();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Size");
    // Whichever unit is not being typed in, shown underneath, so a USD-denominated order still
    // says how much coin it is about to buy (and the reverse).
    if (entry_ref > 0 && ctx.view.ticket_sz > 0) {
        char alt_buf[48];
        if (g_prefs.size_in_usd) {
            format_qty(ctx.view.ticket_sz, ctx.sz_decimals, alt_buf, sizeof(alt_buf));
            ImGui::TextColored(kColorTextMuted, "= %s %s", alt_buf, coin_name);
        } else {
            format_usd(notional(entry_ref, ctx.view.ticket_sz), alt_buf, sizeof(alt_buf));
            ImGui::TextColored(kColorTextMuted, "= %s USDC", alt_buf);
        }
    }

    // The venue's maxTradeSzs is the authoritative "largest size this side can open right now":
    // it already folds in free margin, the selected leverage, the existing position and the
    // margin mode. availableToTrade is the *margin* (USDC) behind that number, not a notional
    // -- sizing off it directly under-sizes the ticket by exactly the leverage factor, which is
    // what this slider used to do.
    const Qty venue_max_qty =
        sizing_data_ready ? (ctx.view.ticket_is_buy ? ctx.portfolio.asset_data.max_trade_buy
                                                    : ctx.portfolio.asset_data.max_trade_sell)
                          : 0;

    // The venue's `maxTradeSzs` is the only ceiling there is: it already folds in free margin,
    // the selected leverage, the existing position and the margin mode, and Hyperliquid
    // rejects anything past it with a reason that reaches the toast line below. There is no
    // second local cap to reconcile it with.
    const Qty slider_max_qty = venue_max_qty;

    // Hidden label so the trailing text can say WHICH max without changing the widget's ImGui
    // ID mid-drag: "100%" of a locally-capped max is not 100% of the account's buying power,
    // and the slider is where that surprise would otherwise land.
    ImGui::SetNextItemWidth(-160.0F);
    if (ImGui::SliderFloat("##ticket_pct", &g_prefs.pct_slider, 0.0F, 100.0F, "%.0f%%")) {
        const int64_t pct = static_cast<int64_t>(g_prefs.pct_slider);
        if (slider_max_qty > 0) {
            ctx.view.ticket_sz =
                static_cast<Qty>(static_cast<__int128>(slider_max_qty) * pct / 100);
        } else if (!sizing_data_ready && ctx.portfolio.account_valid && sizing_ref > 0) {
            // Fallback while the first activeAssetData response is in flight: free collateral
            // is margin, so it has to be multiplied by leverage to become a notional.
            const __int128 budget =
                static_cast<__int128>(portfolio::free_collateral(
                    ctx.portfolio.account, ctx.portfolio.spot, ctx.portfolio.spot_valid)) *
                ctx.view.ticket_leverage * pct / 100;
            const __int128 sz128 = budget * kScale / sizing_ref;
            ctx.view.ticket_sz = sz128 > 0 ? static_cast<Qty>(sz128) : 0;
        } else {
            // A valid zero budget is authoritative; never replace it with account-level
            // withdrawable cash while the venue is telling us this side cannot open.
            ctx.view.ticket_sz = 0;
        }
        refresh_size_buf();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("% of max");

    // --- Rounding preview: "what you see is what gets signed" (docs/02 §7) ---
    const exec::AssetPrecision precision{ctx.sz_decimals};
    // A market order lifts the offer / hits the bid, so the price the ticket must *show* and
    // value the order at is the execution feed's touch on the side we cross into -- not the
    // mid, and not the slippage-padded limit that actually gets signed (docs/02 §6.1, §6.3).
    const Px touch_px = side == Side::Buy ? ctx.instrument.bbo.execution_ask_px()
                                          : ctx.instrument.bbo.execution_bid_px();
    const Px reference = touch_px > 0 ? touch_px : ctx.instrument.bbo.execution_mid();

    Px final_px = 0;
    // The protective IOC limit sent to the venue; equals final_px for a resting limit order.
    Px signed_px = 0;
    bool have_price = true;
    if (ctx.view.ticket_market) {
        if (reference > 0) {
            final_px = exec::Rounder::round_px(reference, side, precision, exec::RoundMode::Passive);
            signed_px = exec::Rounder::marketable_px(reference, side,
                                                     exec::Rounder::kDefaultSlippageBps, precision);
        } else {
            have_price = false;
        }
    } else {
        final_px =
            exec::Rounder::round_px(ctx.view.ticket_px, side, precision, exec::RoundMode::Passive);
        signed_px = final_px;
    }

    ImGui::Checkbox("Reduce only", &ctx.view.ticket_reduce_only);
    ImGui::SameLine();
    ImGui::Checkbox("TP/SL", &g_prefs.tpsl_enabled);

    // --- TP/SL legs ---------------------------------------------------------------------
    // Attached to both limit and market parents: the entry price a percentage is measured
    // against is `final_px`, which is the limit price on a limit order and the crossing touch
    // on a market order, so the same two fields mean the right thing either way.
    TpSlLeg tp_leg{};
    TpSlLeg sl_leg{};
    if (g_prefs.tpsl_enabled) {
        const Qty preview_sz = exec::Rounder::round_sz(ctx.view.ticket_sz, precision);
        // Switching a field's unit rewrites its buffer to the equivalent value in the new
        // unit, so toggling never silently reinterprets "2" as a $2 trigger (or 2%).
        auto unit_combo = [&](const char* id, TpSlUnit* unit, char* buf, size_t cap,
                              const TpSlLeg& leg) {
            ImGui::SetNextItemWidth(80.0F);
            if (!ImGui::BeginCombo(id, tpsl_unit_label(*unit)))
                return;
            auto option = [&](TpSlUnit candidate) {
                if (!ImGui::Selectable(tpsl_unit_label(candidate), *unit == candidate) ||
                    *unit == candidate)
                    return;
                *unit = candidate;
                if (!leg.valid)
                    return;
                if (candidate == TpSlUnit::Price) {
                    format_px(leg.trigger, ctx.sz_decimals, buf, cap);
                    return;
                }
                // Magnitude only: the sign is implied by the leg and the side.
                const int64_t mag = leg.pct_1e8 < 0 ? -leg.pct_1e8 : leg.pct_1e8;
                std::snprintf(buf, cap, "%lld.%02lld", static_cast<long long>(mag * 100 / kScale),
                              static_cast<long long>((mag * 100 % kScale) * 100 / kScale));
            };
            option(TpSlUnit::Price);
            option(TpSlUnit::Pct);
            ImGui::EndCombo();
        };

        auto leg_row = [&](const char* field_id, const char* combo_id, const char* label,
                           char* buf, size_t cap, TpSlUnit* unit, bool is_tp) -> TpSlLeg {
            ImGui::SetNextItemWidth(kFieldWidth);
            ImGui::InputText(field_id, buf, cap);
            // Resolved after the input so the preview reflects this frame's keystroke rather
            // than lagging it, and before the combo so a unit switch converts the value the
            // trader is looking at.
            const TpSlLeg leg =
                resolve_tpsl_leg(buf, *unit, is_tp, ctx.view.ticket_is_buy, final_px, preview_sz,
                                 ctx.view.ticket_leverage, precision);
            ImGui::SameLine();
            unit_combo(combo_id, unit, buf, cap, leg);
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
            // Always shows the unit the trader did NOT type in: a percentage resolves to the
            // trigger price that will be signed, an absolute price resolves to the position
            // move it represents. The coin's own move is appended when leverage makes the two
            // differ, so "-10%" on a 10x position still says out loud that BTC only has to
            // fall 1%. The PnL sits on the same line since it is what the leg is for.
            if (leg.valid) {
                char px_disp[32], pct_disp[32], px_pct_disp[32], pnl_disp[32];
                format_px(leg.trigger, ctx.sz_decimals, px_disp, sizeof(px_disp));
                format_pct(leg.pct_1e8, 2, pct_disp, sizeof(pct_disp));
                format_pct(leg.px_pct_1e8, 2, px_pct_disp, sizeof(px_pct_disp));
                format_usd(leg.pnl < 0 ? -leg.pnl : leg.pnl, pnl_disp, sizeof(pnl_disp));
                const ImVec4 color = leg.pnl >= 0 ? kColorBid : kColorAsk;
                const char* verb = leg.pnl >= 0 ? "gain" : "loss";
                if (ctx.view.ticket_leverage > 1) {
                    ImGui::TextColored(color, "= %s (%s at %ux -- %s move) -- %s %s", px_disp,
                                       pct_disp, ctx.view.ticket_leverage, px_pct_disp, verb,
                                       pnl_disp);
                } else {
                    ImGui::TextColored(color, "= %s (%s) -- %s %s", px_disp, pct_disp, verb,
                                       pnl_disp);
                }
            } else if (leg.present && leg.error) {
                ImGui::TextColored(kColorAsk, "%s", leg.error);
            }
            return leg;
        };

        tp_leg = leg_row("##ticket_tp", "##ticket_tp_unit", "Take profit", g_prefs.tp_buf,
                         sizeof(g_prefs.tp_buf), &g_prefs.tp_unit, true);
        sl_leg = leg_row("##ticket_sl", "##ticket_sl_unit", "Stop loss", g_prefs.sl_buf,
                         sizeof(g_prefs.sl_buf), &g_prefs.sl_unit, false);
    }
    // A leg the trader typed but that cannot be sent must stop the submit: the venue rejects
    // a NORMAL_TPSL group wholesale, so letting it through would silently drop the parent
    // order too.
    const bool tpsl_blocked = (tp_leg.present && !tp_leg.valid) || (sl_leg.present && !sl_leg.valid);

    auto push_margin_setting = [&](app::UiCommandKind kind) {
        if (!ctx.portfolio.account_valid)
            return;
        app::UiCommand command{};
        command.kind = kind;
        command.asset = ctx.instrument.asset;
        command.leverage = ctx.view.ticket_leverage;
        command.is_cross = ctx.view.ticket_cross;
        if (!ctx.bridge.push_command(command))
            event_store().note_local("command queue full -- setting not sent, try again", 2);
    };

    ctx.view.ticket_leverage =
        std::clamp(ctx.view.ticket_leverage, uint32_t{1}, std::max(max_leverage, uint32_t{1}));
    int leverage_i = static_cast<int>(ctx.view.ticket_leverage);
    if (ImGui::SliderInt("Leverage", &leverage_i, 1, static_cast<int>(max_leverage), "%dx")) {
        ctx.view.ticket_leverage = static_cast<uint32_t>(leverage_i);
        g_prefs.settings_touched = true;
        push_margin_setting(app::UiCommandKind::SetLeverage);
    }
    ImGui::BeginDisabled(only_isolated);
    if (ImGui::RadioButton("Cross", ctx.view.ticket_cross)) {
        ctx.view.ticket_cross = true;
        g_prefs.settings_touched = true;
        push_margin_setting(app::UiCommandKind::SetMarginMode);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Isolated", !ctx.view.ticket_cross)) {
        ctx.view.ticket_cross = false;
        g_prefs.settings_touched = true;
        push_margin_setting(app::UiCommandKind::SetMarginMode);
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // --- Rounding preview: "what you see is what gets signed" (docs/02 §7) ---
    const Qty final_sz = exec::Rounder::round_sz(ctx.view.ticket_sz, precision);

    const Usd order_value = have_price ? notional(final_px, final_sz) : 0;

    // Whether a resting limit price is already through the touch, i.e. it will take liquidity
    // instead of posting it. Decided on the bbo feed rather than L2Book (docs/02 §6.1), and
    // computed here because both the est-fill line and the maker/taker fee row depend on it --
    // two answers to the same question would be a bug waiting to happen.
    const Px execution_bid = ctx.instrument.bbo.execution_bid_px();
    const Px execution_ask = ctx.instrument.bbo.execution_ask_px();
    const bool limit_crosses =
        !ctx.view.ticket_market && have_price &&
        ((side == Side::Buy && execution_ask > 0 && final_px >= execution_ask) ||
         (side == Side::Sell && execution_bid > 0 && final_px <= execution_bid));

    // "What you see is what gets signed" (docs/02 §7) used to be served by a pair of read-only
    // "Rounded price / Rounded size" lines under the inputs. Snapping the input buffers to the
    // rounded values once the field loses focus says the same thing in one place instead of
    // two: the number in the box IS the number that gets signed. Only done while the user is
    // not editing, so rounding never fights a half-typed value.
    if (!px_field_active && !ctx.view.ticket_market && have_price && ctx.view.ticket_px > 0) {
        ctx.view.ticket_px = final_px;
        format_px(final_px, ctx.sz_decimals, g_prefs.px_buf, sizeof(g_prefs.px_buf));
    }
    if (!sz_field_active && final_sz != ctx.view.ticket_sz) {
        ctx.view.ticket_sz = final_sz;
        refresh_size_buf();
    }

    if (ctx.view.ticket_market && have_price) {
        char cap_disp[32];
        format_px(signed_px, ctx.sz_decimals, cap_disp, sizeof(cap_disp));
        ImGui::TextColored(kColorTextMuted, "Slippage limit: %s (%.1f%%)", cap_disp,
                           exec::Rounder::kDefaultSlippageBps / 100.0);
    }

    // --- Est fill line (docs/02 §6.3, §7) -- modelled from the live book, using the bbo touch
    // as the reference, never L2Book::mid() (docs/02 §6.1).
    //
    // A market order sweeps; a LIMIT order does not. Running the sweep model on a limit order
    // (which this line used to do unconditionally) printed the market's price for an order that
    // is nowhere near the market -- a buy limit 3% below the touch reported an estimated fill
    // ABOVE its own limit, which is a price the venue can never give it. A limit order can only
    // take the depth at or better than its own limit; the remainder rests.
    const bool have_fill_model = have_price && final_sz > 0 && reference > 0;
    if (!have_fill_model) {
        ImGui::TextColored(kColorTextMuted, "Est fill: --");
    } else if (ctx.view.ticket_market) {
        const exec::SlippageEstimate slippage =
            exec::SlippageModel::estimate(ctx.instrument.book, side, final_sz, reference);
        char avg_disp[32], est_pct[32];
        format_px(slippage.avg_px, ctx.sz_decimals, avg_disp, sizeof(avg_disp));
        // slippage_bps is basis points; format_pct wants a kScale-scaled fraction (1 bps =
        // 1/10000, so value_1e8 = slippage_bps * (kScale / 10000)).
        format_pct(static_cast<int64_t>(slippage.slippage_bps) * (kScale / 10'000), 2, est_pct,
                   sizeof(est_pct));
        ImGui::TextColored(kColorTextMuted, "Est fill: %s  (%s slippage)%s", avg_disp, est_pct,
                           slippage.full_fill ? "" : "  [book too thin]");
        if (!slippage.full_fill) {
            char unfilled_disp[32];
            format_qty(slippage.unfilled, ctx.sz_decimals, unfilled_disp, sizeof(unfilled_disp));
            ImGui::TextColored(kColorWarning, "%s unfilled at visible depth", unfilled_disp);
        }
    } else if (!limit_crosses) {
        // The bbo (not L2Book) decides whether the order crosses, so this branch and the maker
        // fee row below can never disagree about the same order.
        char limit_disp[32];
        format_px(final_px, ctx.sz_decimals, limit_disp, sizeof(limit_disp));
        ImGui::TextColored(kColorTextMuted, "Est fill: %s  (rests -- does not cross the book)",
                           limit_disp);
    } else {
        const exec::LimitFillEstimate fill = exec::SlippageModel::estimate_limit(
            ctx.instrument.book, side, final_sz, final_px, reference);
        char limit_disp[32];
        format_px(final_px, ctx.sz_decimals, limit_disp, sizeof(limit_disp));
        if (fill.immediate <= 0) {
            // bbo says it crosses but the (slower) L2 book has not caught up yet.
            ImGui::TextColored(kColorTextMuted, "Est fill: %s  (crosses -- book not yet updated)",
                               limit_disp);
        } else {
            char avg_disp[32], est_pct[32];
            format_px(fill.avg_px, ctx.sz_decimals, avg_disp, sizeof(avg_disp));
            format_pct(static_cast<int64_t>(fill.slippage_bps) * (kScale / 10'000), 2, est_pct,
                       sizeof(est_pct));
            ImGui::TextColored(kColorTextMuted, "Est fill: %s  (%s slippage)", avg_disp, est_pct);
            if (fill.resting > 0) {
                char rest_disp[32];
                format_qty(fill.resting, ctx.sz_decimals, rest_disp, sizeof(rest_disp));
                ImGui::TextColored(kColorWarning, "%s of it rests at %s", rest_disp, limit_disp);
            }
        }
    }

    // --- Account-aware preview -------------------------------------------------------------
    // activeAssetData is the venue's authoritative, side-specific buying-power answer. The
    // account snapshot remains useful for the cross-margin liquidation estimate, while the
    // fee snapshot supplies the user's actual effective tier rather than a hard-coded default.
    const bool data_for_asset =
        ctx.portfolio.account_valid && ctx.portfolio.asset_data_valid &&
        ctx.portfolio.asset_data_asset == ctx.instrument.asset;
    const Px available_mark = data_for_asset && ctx.portfolio.asset_data.mark > 0
                                  ? ctx.portfolio.asset_data.mark
                                  : ctx.instrument.ctx.mark_px();
    // Free collateral -- the USDC not backing a position, which is what can fund a new one.
    // Same basis as the Balances tab: `withdrawable` (which already nets out margin locked by
    // resting orders, unlike `total_margin_used`) PLUS the USDC that has never been deployed
    // into perps, which `withdrawable` cannot see. Without the second term this line read $37
    // against the venue's own $1,471, and the gate below then blocked the exact size the
    // slider above offered.
    //
    // Still NOT read from activeAssetData's `availableToTrade`, even though free_collateral()
    // now reproduces it to within a couple of cents. That field is `maxTradeSzs * mark /
    // leverage`, and on the side that CLOSES an existing position the venue is dividing
    // capacity that releases margin rather than spending it: on a 0.75 BTC short at 10x it
    // reported $12.7k of "available margin" against an account worth $6.8k. Only the opening
    // side is a free-margin answer, and which side that is depends on the position, so free
    // margin stays an account-level fact computed from account-level inputs.
    const Usd available_margin =
        ctx.portfolio.account_valid
            ? portfolio::free_collateral(ctx.portfolio.account, ctx.portfolio.spot,
                                         ctx.portfolio.spot_valid)
            : 0;
    // The venue's own side-specific ceiling -- what Hyperliquid would reject on.
    const Qty max_trade_qty =
        data_for_asset
            ? (ctx.view.ticket_is_buy ? ctx.portfolio.asset_data.max_trade_buy
                                      : ctx.portfolio.asset_data.max_trade_sell)
            : 0;

    const bool fee_is_taker = ctx.view.ticket_market || limit_crosses;
    const bool fee_ready = ctx.portfolio.account_valid && ctx.portfolio.fee_rates_valid;
    const int64_t fee_rate = fee_ready
                                 ? (fee_is_taker ? ctx.portfolio.fee_rates.taker_rate
                                                 : ctx.portfolio.fee_rates.maker_rate)
                                 : 0;
    const Usd estimated_fee = fee_ready ? portfolio::fee_estimate(order_value, fee_rate) : 0;

    const Qty existing_szi = current_position ? current_position->value.szi : 0;
    const Qty signed_order_szi = ctx.view.ticket_is_buy ? final_sz : -final_sz;
    const Qty resulting_szi = existing_szi + signed_order_szi;
    Px preview_entry = final_px;
    if (current_position && existing_szi != 0 && resulting_szi != 0 && signed_order_szi != 0) {
        const bool order_same_direction = (existing_szi > 0) == (signed_order_szi > 0);
        const bool result_same_direction = (existing_szi > 0) == (resulting_szi > 0);
        if (order_same_direction)
            preview_entry = blended_entry(current_position->value.entry_px, existing_szi,
                                          final_px, signed_order_szi);
        else if (result_same_direction)
            preview_entry = current_position->value.entry_px;
    }

    // The margin this order actually costs, which is the margin on the position it LEAVES
    // BEHIND minus the margin already posted against the position it trades through -- not the
    // full order notional over leverage. Buying 1.6 BTC against a 0.75 BTC short does not need
    // $12.7k: it flattens the short (freeing its $5.9k) and leaves a 0.86 BTC long, so its real
    // cost is the ~$0.9k difference. Charging the full notional made the old "Margin required"
    // line agree with the equally inflated `availableToTrade` above -- two wrong numbers whose
    // ratio happened to be right, so the gate passed while both displayed figures were fiction.
    Usd margin_required = 0;
    if (!ctx.view.ticket_reduce_only && final_sz > 0) {
        const Qty resulting_abs = resulting_szi < 0 ? -resulting_szi : resulting_szi;
        const Px margin_px = available_mark > 0 ? available_mark : final_px;
        const Usd resulting_margin = portfolio::initial_margin(
            notional(margin_px, resulting_abs), ctx.view.ticket_leverage);
        // Only margin in the same mode is fungible with the free margin above: an isolated
        // position's margin is not part of the cross pool, so releasing it does not fund a
        // cross order. When the modes disagree, charge the order in full rather than promise
        // an offset the venue will not give.
        const Usd released =
            current_position && existing_szi != 0 &&
                    (current_position->value.is_cross != 0) == ctx.view.ticket_cross
                ? current_position->value.margin_used
                : 0;
        margin_required = std::max<Usd>(0, resulting_margin - released);
    }
    const Usd maintenance_rate =
        portfolio::maintenance_rate_for_max_leverage(max_leverage);
    Px liquidation_px = resulting_szi != 0 && current_position && current_position->value.liq_px > 0
                            ? current_position->value.liq_px
                            : 0;
    bool liquidation_is_estimate = false;

    if (ctx.portfolio.account_valid && have_price && resulting_szi != 0 && maintenance_rate > 0) {
        const Usd resulting_value = notional(preview_entry,
                                             resulting_szi < 0 ? -resulting_szi : resulting_szi);
        Usd margin_available = 0;
        if (ctx.view.ticket_cross) {
            const Usd current_maintenance =
                current_position && current_position->value.is_cross
                    ? portfolio::maintenance_margin(current_position->value.position_value,
                                                    maintenance_rate)
                    : 0;
            const Usd other_maintenance =
                std::max<Usd>(0, ctx.portfolio.account.cross_maintenance_margin -
                                     current_maintenance);
            const Usd total_maintenance =
                other_maintenance +
                portfolio::maintenance_margin(resulting_value, maintenance_rate);
            // Cross equity, not `account_value`: the latter is `marginSummary` and includes
            // every isolated position's margin and PnL, which is collateral this cross
            // position can never draw on. Using it pushed the estimated liquidation price
            // further away by the whole isolated book. `cross_maintenance_margin`, which
            // `other_maintenance` comes from, is cross-only for the same reason -- the two
            // have to be quoted on the same side of that line.
            margin_available = ctx.portfolio.account.cross_account_value - total_maintenance;
        } else {
            Usd isolated_margin = 0;
            if (current_position && !current_position->value.is_cross) {
                const bool increasing = existing_szi != 0 &&
                                        ((existing_szi > 0) == (signed_order_szi > 0));
                if (increasing)
                    isolated_margin = current_position->value.margin_used + margin_required;
                else if (resulting_szi != 0 && ((existing_szi > 0) == (resulting_szi > 0)))
                    isolated_margin = portfolio::initial_margin(
                        notional(current_position->value.entry_px,
                                 resulting_szi < 0 ? -resulting_szi : resulting_szi),
                        ctx.view.ticket_leverage);
                else
                    isolated_margin = portfolio::initial_margin(resulting_value,
                                                                 ctx.view.ticket_leverage);
            } else {
                isolated_margin = portfolio::initial_margin(resulting_value,
                                                             ctx.view.ticket_leverage);
            }
            margin_available =
                isolated_margin -
                portfolio::maintenance_margin(resulting_value, maintenance_rate);
        }

        if (margin_available >= 0) {
            const Px estimate = portfolio::estimated_liquidation_price(
                preview_entry, resulting_szi, margin_available, maintenance_rate);
            if (estimate > 0) {
                liquidation_px = estimate;
                liquidation_is_estimate = true;
            }
        }
    }

    if (ImGui::BeginTable("ticket_account_summary", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("ticket_summary_label", ImGuiTableColumnFlags_WidthStretch,
                                0.62F);
        ImGui::TableSetupColumn("ticket_summary_value", ImGuiTableColumnFlags_WidthStretch,
                                0.38F);

        auto summary_usd = [&](const char* label, Usd value, ImVec4 color = kColorTextPrimary,
                               bool fine = false) {
            char value_buf[48];
            if (fine)
                format_usd_fine(value, value_buf, sizeof(value_buf));
            else
                format_usd(value, value_buf, sizeof(value_buf));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextColored(color, "%s USDC", value_buf);
        };
        summary_usd("Order value", order_value);
        summary_usd("Margin required", margin_required);

        char fee_label[48];
        std::snprintf(fee_label, sizeof(fee_label), "Est. fee (%s)",
                      fee_is_taker ? "taker" : "maker");
        if (fee_ready) {
            // Fine precision: on a small order the fee is a fraction of a cent, and rounding
            // it to "$0.00" reads as "this trade is free" rather than "too small to show".
            summary_usd(fee_label, estimated_fee,
                        estimated_fee <= 0 ? kColorBid : kColorTextPrimary, /*fine=*/true);
        } else {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", fee_label);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Loading rate...");
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // Account-level and side-independent, matching the Balances tab exactly. "Margin
        // required" above is already net of the margin this order releases, so the two lines
        // compare directly: required <= available is the whole solvency question.
        ImGui::TextDisabled("Available margin");
        ImGui::TableNextColumn();
        if (ctx.portfolio.account_valid) {
            char usd_buf[32];
            format_usd(available_margin, usd_buf, sizeof(usd_buf));
            ImGui::TextColored(available_margin > 0 ? kColorTextPrimary : kColorWarning,
                               "%s USDC", usd_buf);
        } else if (ctx.portfolio.account_valid || event_store().has_user_conn()) {
            // A live user socket means we are signed in; the account snapshot is simply still
            // in flight. Saying "connect account" there reads as though the unlock failed.
            ImGui::TextDisabled("Loading account limits...");
        } else {
            ImGui::TextDisabled("Connect account");
        }

        if (slider_max_qty > 0) {
            const Px max_price = available_mark;
            char max_qty_buf[32], max_usd_buf[32], max_buf[96];
            format_qty(slider_max_qty, ctx.sz_decimals, max_qty_buf, sizeof(max_qty_buf));
            format_usd(max_price > 0 ? notional(max_price, slider_max_qty) : 0, max_usd_buf,
                       sizeof(max_usd_buf));
            std::snprintf(max_buf, sizeof(max_buf), "%s %s (%s USDC)", max_qty_buf, coin_name,
                          max_usd_buf);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // Notional, i.e. already multiplied by the selected leverage -- which is why it can
            // legitimately be many times the available margin on the line above. On the side
            // opposing an open position `maxTradeSzs` also covers flattening it, so it dwarfs
            // free margin; "+close" says so. The label column is narrow, so this stays terse
            // rather than clipping -- the tooltip carries the full sentence.
            const bool max_includes_close =
                existing_szi != 0 && (existing_szi > 0) != ctx.view.ticket_is_buy;
            ImGui::TextDisabled("Max %s %ux%s", ctx.view.ticket_is_buy ? "buy" : "sell",
                                ctx.view.ticket_leverage,
                                max_includes_close ? " +close" : "");
            if (max_includes_close && ImGui::IsItemHovered())
                ImGui::SetTooltip("Includes closing the open position: this size flattens it "
                                  "and opens the other way with the margin that frees up.");
            ImGui::TableNextColumn();
            ImGui::TextColored(kColorTextPrimary, "%s", max_buf);
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled(liquidation_is_estimate ? "Est. liquidation" : "Liquidation price");
        ImGui::TableNextColumn();
        if (liquidation_px > 0) {
            char liq_buf[32];
            format_px(liquidation_px, ctx.sz_decimals, liq_buf, sizeof(liq_buf));
            ImGui::TextColored(kColorWarning, "%s", liq_buf);
        } else {
            ImGui::TextDisabled("--");
        }
        ImGui::EndTable();
    }

    // A stop placed at or beyond the liquidation price never triggers: the venue closes the
    // position first, at the liquidation price and with the liquidation fee, so the "loss $X"
    // the stop-loss row promises is not the loss that actually happens. This is easy to walk
    // into with a percentage stop, because the percentage is a move of the PRICE while
    // liquidation arrives at roughly 100%/leverage of price move -- a 15% stop is simply
    // unreachable on a 10x position.
    // Direction comes from `resulting_szi`, the position the stop would actually protect, which
    // is also what `liquidation_px` was computed for.
    if (sl_leg.valid && portfolio::stop_beyond_liquidation(sl_leg.trigger, liquidation_px,
                                                           resulting_szi)) {
        char sl_disp[32], liq_disp[32];
        format_px(sl_leg.trigger, ctx.sz_decimals, sl_disp, sizeof(sl_disp));
        format_px(liquidation_px, ctx.sz_decimals, liq_disp, sizeof(liq_disp));
        ImGui::TextColored(kColorWarning,
                           "Stop %s is past the %sliquidation price %s -- you are liquidated "
                           "before the stop can trigger",
                           sl_disp, liquidation_is_estimate ? "estimated " : "", liq_disp);
    }

    const bool account_blocked = !ctx.portfolio.account_valid;
    const bool account_data_loading = ctx.portfolio.account_valid && !data_for_asset &&
                                      !ctx.view.ticket_reduce_only;
    // Net new margin against free margin -- both account-level and in the same units.
    //
    // `margin_required > 0` keeps an order that needs no new margin -- one that only reduces or
    // flattens -- from ever being blocked on margin. It cannot be the thing that makes the
    // account unsafe, and it is the way out of a fully-committed account.
    const bool available_blocked = ctx.portfolio.account_valid && !ctx.view.ticket_reduce_only &&
                                   have_price && margin_required > 0 &&
                                   margin_required > available_margin;
    const bool max_trade_blocked = data_for_asset && !ctx.view.ticket_reduce_only &&
                                   max_trade_qty > 0 && final_sz > max_trade_qty;
    if (account_blocked) {
        if (event_store().has_user_conn())
            ImGui::TextColored(kColorWarning, "Loading account snapshot -- orders paused.");
        else
            ImGui::TextColored(kColorWarning, "Blocked: connect an account to trade.");
    } else if (account_data_loading) {
        ImGui::TextColored(kColorWarning, "Loading account limits -- opening orders are paused.");
    } else if (available_blocked) {
        char excess_value[32];
        format_usd(margin_required - available_margin, excess_value, sizeof(excess_value));
        ImGui::TextColored(kColorAsk, "Blocked: needs %s USDC more margin than is available",
                           excess_value);
    } else if (max_trade_blocked) {
        char excess_qty[32], excess_value[32];
        const Qty excess_size = final_sz - max_trade_qty;
        format_qty(excess_size, ctx.sz_decimals, excess_qty, sizeof(excess_qty));
        format_usd(notional(final_px, excess_size), excess_value, sizeof(excess_value));
        ImGui::TextColored(kColorAsk, "Blocked: max order size exceeded by %s (%s USDC)",
                           excess_qty, excess_value);
    } else if (tpsl_blocked) {
        // The per-field line above already says which value is wrong; this says why it stops
        // the order rather than just the leg.
        ImGui::TextColored(kColorAsk, "Blocked: fix or clear the %s trigger -- the venue "
                                      "rejects the whole order group otherwise",
                           (tp_leg.present && !tp_leg.valid) ? "take profit" : "stop loss");
    }

    // Everything that stops a submit is now either a missing input or the venue's own
    // arithmetic (margin, max trade size). There is no local notional/leverage/price-band
    // gate: Hyperliquid enforces its rules on every order and its rejection surfaces on the
    // note line under the button.
    const bool submit_blocked =
        !have_price || final_sz <= 0 || account_blocked || account_data_loading ||
        available_blocked || max_trade_blocked || tpsl_blocked;

    ImGui::Separator();
    ImGui::BeginDisabled(submit_blocked);
    // The "##submit" suffix is load-bearing, not cosmetic. ImGui derives a widget's identity
    // from its label, and the side selector at the top of this panel carries these exact two
    // strings -- so without a distinguishing suffix the submit button shares an ID with the
    // Buy/Long toggle, ImGui routes the click to whichever it saw first, and pressing submit
    // silently does nothing. The text before "##" is still what gets drawn.
    const char* label = ctx.view.ticket_is_buy ? "Buy / Long##ticket_submit"
                                               : "Sell / Short##ticket_submit";
    ImGui::PushStyleColor(ImGuiCol_Button, ctx.view.ticket_is_buy ? kColorBid : kColorAsk);
    if (ImGui::Button(label, ImVec2(-1, 0))) {
        pc_order main_order{};
        main_order.asset = ctx.instrument.asset;
        main_order.is_buy = ctx.view.ticket_is_buy ? 1 : 0;
        main_order.reduce_only = ctx.view.ticket_reduce_only ? 1 : 0;
        main_order.tif = ctx.view.ticket_market ? PC_TIF_IOC : PC_TIF_GTC;
        main_order.tpsl = PC_TPSL_NONE;
        main_order.limit_px = signed_px;
        main_order.sz = final_sz;
        make_local_cloid(main_order.cloid, ctx.now_ms);

        pc_order_req req{};
        req.n_orders = 0;
        req.orders[req.n_orders++] = main_order;

        // The legs were resolved above from the same entry price the preview showed, so the
        // trigger sent here is exactly the one displayed -- no second parse, and no chance of
        // the percentage being re-measured against a price that moved mid-frame.
        if (g_prefs.tpsl_enabled && (tp_leg.valid || sl_leg.valid)) {
            req.grouping = PC_GROUP_NORMAL_TPSL;
            auto attach = [&](const TpSlLeg& leg, uint8_t kind, uint64_t cloid_salt) {
                if (!leg.valid || req.n_orders >= 4)
                    return;
                pc_order child{};
                child.asset = ctx.instrument.asset;
                child.is_buy = !ctx.view.ticket_is_buy;  // opposite side, reduce-only
                child.reduce_only = 1;
                child.tif = PC_TIF_GTC;
                child.tpsl = kind;
                child.trigger_px = leg.trigger;
                child.limit_px = leg.trigger;
                child.is_market_trigger = 1;
                child.sz = final_sz;
                make_local_cloid(child.cloid, ctx.now_ms + cloid_salt);
                req.orders[req.n_orders++] = child;
            };
            attach(tp_leg, PC_TPSL_TP, 1);
            attach(sl_leg, PC_TPSL_SL, 2);
        } else {
            req.grouping = PC_GROUP_NA;
        }

        app::UiCommand cmd{};
        cmd.kind = app::UiCommandKind::PlaceOrder;
        cmd.asset = ctx.instrument.asset;
        cmd.order = req;
        // UiCommandRing is bounded (256 slots, low-rate by design); surface a full ring rather
        // than silently dropping the trader's submit.
        char qty_disp[32], px_disp[32];
        format_qty(final_sz, ctx.sz_decimals, qty_disp, sizeof(qty_disp));
        format_px(signed_px, ctx.sz_decimals, px_disp, sizeof(px_disp));
        const uint8_t child_legs = static_cast<uint8_t>(req.n_orders - 1);
        if (ctx.bridge.push_command(cmd)) {
            std::snprintf(g_prefs.submit_note, sizeof(g_prefs.submit_note),
                          "Sent: %s %s %s @ %s%s%s", ctx.view.ticket_is_buy ? "buy" : "sell",
                          qty_disp, coin_name, px_disp,
                          ctx.view.ticket_market ? " (market)" : "",
                          child_legs > 0 ? " + TP/SL" : "");
            g_prefs.submit_note_error = false;
            // Also logged, so the Status tab keeps the session's full order history.
            event_store().note_local(g_prefs.submit_note, 0);
        } else {
            std::snprintf(g_prefs.submit_note, sizeof(g_prefs.submit_note),
                          "Not sent: command queue full -- try again");
            g_prefs.submit_note_error = true;
            event_store().note_local("command queue full -- order not sent, try again", 2);
        }
        g_prefs.submit_note_ms = ctx.now_ms;
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    // Mirror any new warning/error toast into the note area. Placed after the submit handler so
    // a rejection that arrives in the same frame as a click wins over the "Sent" line -- the
    // failure is the more important of the two, and the order of events is what it says.
    const uint64_t pushed = event_store().toasts_pushed();
    const uint64_t have = event_store().toast_count();
    if (pushed > g_prefs.seen_toasts) {
        // Only the toasts still resident in the ring can be read back; anything older than that
        // was displaced while this panel was not looking and is simply skipped.
        const uint64_t first = pushed - have > g_prefs.seen_toasts ? pushed - have
                                                                   : g_prefs.seen_toasts;
        for (uint64_t n = first; n < pushed; ++n) {
            const ToastRow& t = event_store().toast_at(static_cast<size_t>(n - (pushed - have)));
            if (t.severity == 0)
                continue;  // info; the "Sent" line already covers the only one this panel emits
            std::snprintf(g_prefs.submit_note, sizeof(g_prefs.submit_note), "%s", t.text);
            g_prefs.submit_note_error = true;
            g_prefs.submit_note_ms = ctx.now_ms;
        }
    }
    g_prefs.seen_toasts = pushed;

    // "Sent" is an acknowledgement that the order left this panel, not that the venue accepted
    // it -- the ack, the fill and any rejection all arrive later and belong to Open Orders and
    // the Status tab. Saying more than that here would be a promise this panel cannot keep.
    constexpr uint64_t kSubmitNoteMs = 6'000;
    if (g_prefs.submit_note[0] != '\0') {
        if (ctx.now_ms - g_prefs.submit_note_ms > kSubmitNoteMs)
            g_prefs.submit_note[0] = '\0';
        else
            ImGui::TextColored(g_prefs.submit_note_error ? kColorAsk : kColorBid, "%s",
                               g_prefs.submit_note);
    }

    ImGui::End();
}

}  // namespace pc::ui
