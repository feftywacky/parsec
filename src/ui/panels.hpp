#pragma once
// The panel contract. Every panel is a free function taking a PanelContext and drawing into
// its own docked ImGui window; none of them holds state beyond view preferences, and none of
// them may call pc_* or touch engine state directly (docs/02 §7). User intent leaves a panel
// only through PanelContext::bridge.push_command().
//
// Panels are drawn in the order AppWindow::draw_panels() calls them, once per frame, always
// between ImGui::NewFrame() and ImGui::Render().
#include <cstdint>

#include "app/ui_bridge.hpp"
#include "core/units.hpp"
#include "md/market_store.hpp"
#include "parsec/parsec.h"

namespace pc::ui {

// Per-session view preferences. Owned by AppWindow, mutated freely by panels (UI thread only,
// so no synchronisation) and persisted to ~/.parsec/ui.json in Phase 6.
struct ViewState {
    uint32_t active_asset{PC_ASSET_NONE};
    uint8_t interval{PC_IV_1M};  // chart timeframe, PC_IV_*
    bool chart_log_scale{false};

    // Book display aggregation. Maps to the venue's nSigFigs/mantissa, which is a DISPLAY
    // control only and must never reach execution (docs/03 §W3).
    int book_aggregation{0};

    // Whether the book's Size/Total columns are denominated in USDC notional rather than in
    // the coin itself. Display only, like book_aggregation.
    bool book_size_in_usd{false};

    // Order ticket working state. `ticket_px` is the raw user input; the rounded price the
    // ticket previews (and what actually gets signed) comes from exec::Rounder.
    Px ticket_px{};
    Qty ticket_sz{};
    bool ticket_is_buy{true};
    bool ticket_reduce_only{false};
    bool ticket_market{false};
    uint32_t ticket_leverage{1};
    bool ticket_cross{true};

    // Set by panel_book when a level is clicked, consumed by panel_ticket: "click a book level
    // -> the limit price populates the ticket" (docs/02 §7, the most-used interaction).
    bool price_pick_pending{false};
    Px picked_px{};
};

// Everything a panel may read this frame. Snapshots are already loaded across the seqlock by
// AppWindow, so every panel in a frame sees the same consistent view.
struct PanelContext {
    const app::InstrumentSnapshot& instrument;
    const app::AssetUniverseSnapshot& universe;
    const app::PortfolioSnapshot& portfolio;

    // Direct read access to the active asset's candle series and trade tape. These are far too
    // large to copy through a seqlock, so they are read in place via their generation-counter
    // protocol -- CandleSeries::copy_recent() / TradeTape::copy_recent(). May be null before
    // the first event for this asset arrives; panels must handle that.
    const md::AssetMarket* market;

    app::UiBridge& bridge;
    ViewState& view;
    uint64_t now_ms{};

    // szDecimals for the active asset, resolved from `meta`. Sizes render at this precision
    // and prices at the venue's own precision for the asset (docs/07 Phase 2: getting this
    // wrong makes fat-finger errors easier to miss).
    uint8_t sz_decimals{};

    // Resolved network. Never inferred in a panel -- the network badge is a safety control.
    bool mainnet{false};
};

// Milliseconds until the next funding settlement. Hyperliquid funds hourly on the hour (UTC)
// and the wire carries no explicit next-funding timestamp, so this is derived from the frame's
// clock (PanelContext::now_ms) -- shared here so the header strip and the funding panel can
// never disagree about the same countdown.
inline uint64_t ms_to_next_funding(uint64_t now_ms) noexcept {
    constexpr uint64_t kHourMs = 60ULL * 60ULL * 1000ULL;
    return kHourMs - (now_ms % kHourMs);
}

// --- Phase 2: the read-only terminal ---
// The top strip: which network this build is pointed at, whether the feed is live, and the
// venue/engine latency figures. Drawn inside the dockspace host's menu bar rather than in a
// panel -- these are session-wide facts about the connection, not facts about the selected
// instrument, and they were competing with the price metrics for room in the header strip.
void draw_menu_bar_status(PanelContext& ctx);

void draw_instruments(PanelContext& ctx);
void draw_chart(PanelContext& ctx);
void draw_book(PanelContext& ctx);
void draw_trades(PanelContext& ctx);

// --- Phases 3-5: identity, order entry, portfolio, safety net ---
void draw_ticket(PanelContext& ctx);
void draw_positions(PanelContext& ctx);
void draw_open_orders(PanelContext& ctx);
void draw_balances(PanelContext& ctx);
void draw_fills(PanelContext& ctx);
void draw_funding(PanelContext& ctx);
void draw_order_history(PanelContext& ctx);
void draw_status_bar(PanelContext& ctx);

}  // namespace pc::ui
