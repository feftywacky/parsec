#pragma once
// Session-local history built from app::UiEventRing (docs/02-architecture.md §4.3's discrete
// event side of the engine/UI boundary: fills, order acks, order updates, toasts, connection
// state). AppWindow (not owned by this agent) does not currently drain that ring itself, so
// every panel that needs event history drains it here instead.
//
// This is safe to call from more than one panel per frame: `event_store().drain(ctx.bridge)`
// pops everything currently queued into the buffers below the FIRST time it is called in a
// frame; every subsequent call that frame finds the ring already empty and is a cheap no-op.
// That means panel draw order never matters and no panel needs to "own" draining -- any panel
// that wants fresh data just calls drain() unconditionally at the top of its own draw
// function.
//
// None of pc_fill, pc_order_update or pc_order_ack (include/parsec/parsec.h) carries an asset
// id of its own -- only the top-level pc_event does. app::UiEvent forwards it as
// UiEvent::asset, and every row below copies it, so a fill/order row can name its own coin and
// a per-row action (cancel, close) targets the asset the row is actually about rather than
// whichever instrument happens to be on screen.
#include <algorithm>
#include <array>
#include <cstdint>

#include "app/ui_bridge.hpp"
#include "core/units.hpp"
#include "parsec/parsec.h"
#include "portfolio/pnl.hpp"

namespace pc::ui {

struct FillRow {
    uint32_t asset{PC_ASSET_NONE};
    uint64_t time_ms{};  // venue wall clock; 0 when the source event carried none
    uint8_t dir{};       // PC_DIR_*: what this fill did to the position
    uint64_t oid{}, tid{};
    Px px{};
    Qty qty{};
    Usd fee{};  // already includes builderFee (docs/03 §W2.9) -- never add to it again
    Usd closed_pnl{};
    bool is_buy{};
    bool is_taker{};  // `crossed` on the wire
    uint64_t recv_time_ns{};
};

struct OrderRow {
    uint32_t asset{PC_ASSET_NONE};
    uint64_t oid{};
    uint16_t status{};  // PC_ORD_* (or PC_ORD_REJECTED, synthesized from a PC_ACK_ERR ack)
    Px px{};
    Qty sz{}, orig_sz{};
    // Where a trigger order actually rests. `px` is the limit it converts to when it fires,
    // which for a market trigger is deliberately ~5% away from the market -- plotting that as
    // the order's price would draw the stop in entirely the wrong place. 0 when unknown.
    Px trigger_px{};
    bool is_trigger{};
    uint8_t tpsl{};              // PC_TPSL_* for a trigger order
    bool is_market_trigger{};    // a fired trigger becomes a market order, not a limit one
    uint64_t time_ms{};          // venue wall clock; 0 when the source event carried none
    bool is_buy{};
    bool reduce_only{};
    uint64_t recv_time_ns{};
    char err[PC_ERR_LEN]{};  // populated only for the synthesized ack-rejection rows
};

// Session-realized P&L for one asset, accumulated from the fill stream. `pnl` is net of fees
// (docs' realized-P&L definition: closed_pnl - fee, portfolio::realized_pnl_from_fill), which
// is the number a trader compares against unrealized P&L; `fees` is kept alongside so the
// gross figure is still recoverable. Both cover only the fills this session has seen -- the
// live userFills stream plus whatever the venue's reconnect backfill replayed -- not the
// account's lifetime history, which no wire field here reports.
struct RealizedRow {
    Usd pnl{};
    Usd fees{};
    uint32_t fills{};
};

// One funding payment, as the venue settles them hourly. `usdc` is signed the way the wire
// signs it: NEGATIVE is paid, positive is received (see pc_funding). `szi` is the position
// size at the funding timestamp, whose sign gives the side.
struct FundingRow {
    uint32_t asset{PC_ASSET_NONE};
    uint64_t time_ms{};
    Usd usdc{};
    Qty szi{};
    int64_t rate_1e8{};
    uint32_t n_samples{};
};

struct ToastRow {
    char text[128]{};
    uint8_t severity{};  // 0 info, 1 warn, 2 error
    uint64_t recv_time_ns{};
};

class UiEventStore {
public:
    static constexpr size_t kMaxToasts = 64;
    static constexpr size_t kMaxFills = 256;
    static constexpr size_t kMaxOpenOrders = 128;
    static constexpr size_t kMaxHistory = 256;
    static constexpr size_t kMaxFunding = 256;
    // Matches portfolio::PositionBook::kMaxAssets -- the dense asset index space the venue's
    // `meta` universe is numbered in.
    static constexpr size_t kMaxAssets = 512;

    // Pops every event currently queued in `bridge` and files it into the buffers below.
    // Idempotent within a frame once the ring is empty -- see the class comment.
    void drain(app::UiBridge& bridge) noexcept;

    // Files a locally-sourced notice (not from the engine) into the same toast list the status
    // bar renders, e.g. "command queue full" when a panel's push_command() fails. Public
    // because that failure can happen in any panel, not just the ones that otherwise touch
    // this store.
    void note_local(const char* text, uint8_t severity) noexcept;

    [[nodiscard]] bool has_market_conn() const noexcept { return has_market_conn_; }
    [[nodiscard]] const pc_conn& market_conn() const noexcept { return market_conn_; }
    [[nodiscard]] bool has_user_conn() const noexcept { return has_user_conn_; }
    [[nodiscard]] const pc_conn& user_conn() const noexcept { return user_conn_; }

    // Monotonic count of every toast ever pushed this session -- unlike toast_count(), which
    // saturates at kMaxToasts and so cannot be used as a "have I seen this one?" high-water
    // mark once the ring is full.
    [[nodiscard]] uint64_t toasts_pushed() const noexcept { return toast_head_; }

    // Toasts, oldest-first iteration: index i in [0, toast_count()) via toast_at(i).
    [[nodiscard]] size_t toast_count() const noexcept { return toast_count_; }
    [[nodiscard]] const ToastRow& toast_at(size_t i) const noexcept {
        return toasts_[static_cast<size_t>((toast_head_ - toast_count_ + i) % kMaxToasts)];
    }

    // Realized P&L accumulated for one asset (or account-wide via realized_total()). See
    // RealizedRow: session-scoped, net of fees, deduplicated by trade id so the REST backfill
    // replaying a fill the live stream already delivered cannot double-count it.
    [[nodiscard]] const RealizedRow& realized(uint32_t asset) const noexcept {
        static constexpr RealizedRow kEmpty{};
        return asset < kMaxAssets ? realized_[asset] : kEmpty;
    }
    [[nodiscard]] const RealizedRow& realized_total() const noexcept { return realized_total_; }

    // Realized P&L of the position now open in `asset` (signed size `szi`): only the fills
    // since it opened from flat or flipped, not every fill the coin has had this session.
    // See portfolio::realized_since_open.
    [[nodiscard]] portfolio::PositionRealized position_realized(uint32_t asset,
                                                                Qty szi) const noexcept;

    [[nodiscard]] size_t fill_count() const noexcept { return fill_count_; }
    [[nodiscard]] const FillRow& fill_at(size_t i) const noexcept {
        // Most-recent-first, which is how a fills table is read.
        return fills_[(fill_head_ - 1 - i + kMaxFills) % kMaxFills];
    }

    [[nodiscard]] size_t open_order_count() const noexcept { return open_order_count_; }
    [[nodiscard]] const OrderRow& open_order_at(size_t i) const noexcept { return open_orders_[i]; }

    // Funding payments, most-recent-first like the fills log.
    [[nodiscard]] size_t funding_count() const noexcept { return funding_count_; }
    [[nodiscard]] const FundingRow& funding_at(size_t i) const noexcept {
        return funding_[(funding_head_ - 1 - i + kMaxFunding) % kMaxFunding];
    }
    // Net funding over the rows held, signed like FundingRow::usdc.
    [[nodiscard]] Usd funding_total() const noexcept { return funding_total_; }

    [[nodiscard]] size_t history_count() const noexcept { return history_count_; }
    [[nodiscard]] const OrderRow& history_at(size_t i) const noexcept {
        return history_[(history_head_ - 1 - i + kMaxHistory) % kMaxHistory];
    }

    [[nodiscard]] uint64_t dropped_events() const noexcept { return dropped_; }

private:
    void on_fill(uint32_t asset, const pc_fill& f, uint64_t t, uint64_t exch_time_ms) noexcept;
    void on_ack(uint32_t asset, const pc_order_ack& a, uint64_t t) noexcept;
    void on_funding(const app::UiEvent& ev) noexcept;
    void on_order_update(const app::UiEvent& ev) noexcept;
    // True the first time a trade id is seen. Guards against the same fill arriving twice --
    // once live, once from the reconnect backfill -- which would otherwise both duplicate a
    // fills row and double-count realized P&L.
    bool claim_fill_tid(uint64_t tid) noexcept;
    // Folds a rejected orderUpdate into the ack-sourced rejection row it belongs to, so one
    // rejection is one row. Returns false when there is no such row to fold into.
    bool merge_into_pending_rejection(const OrderRow& update) noexcept;
    void push_toast(const ToastRow& row) noexcept;
    void push_history(const OrderRow& row) noexcept;
    void upsert_open(const OrderRow& row) noexcept;
    void remove_open(uint64_t oid) noexcept;

    bool has_market_conn_{};
    pc_conn market_conn_{};
    bool has_user_conn_{};
    pc_conn user_conn_{};

    std::array<ToastRow, kMaxToasts> toasts_{};
    uint64_t toast_head_{};
    size_t toast_count_{};

    std::array<FillRow, kMaxFills> fills_{};
    size_t fill_head_{};
    size_t fill_count_{};

    // Linear, not circular -- and rebuilt wholesale from each frontendOpenOrders snapshot
    // rather than accumulated, so a missed terminal update cannot strand a dead order here.
    std::array<OrderRow, kMaxOpenOrders> open_orders_{};
    size_t open_order_count_{};

    std::array<OrderRow, kMaxHistory> history_{};
    size_t history_head_{};
    size_t history_count_{};

    std::array<FundingRow, kMaxFunding> funding_{};
    size_t funding_head_{};
    size_t funding_count_{};
    Usd funding_total_{};

    std::array<RealizedRow, kMaxAssets> realized_{};
    RealizedRow realized_total_{};
    // Ring of recently seen trade ids, for claim_fill_tid(). Sized to the fills buffer: a
    // backfill never replays more than the venue's own recent window, so anything older than
    // this has long since stopped being re-sent.
    std::array<uint64_t, kMaxFills> seen_tids_{};
    size_t seen_tid_head_{};

    uint64_t dropped_{};
};

// Process-wide singleton (UI thread only, like every other pc::ui:: state). A function-local
// static keeps construction lazy and avoids a static-init-order dependency between panels.
UiEventStore& event_store() noexcept;

}  // namespace pc::ui
