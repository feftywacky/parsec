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
// KNOWN GAP, called out loudly because it limits what this store can show: none of pc_fill,
// pc_order_update, or pc_order_ack (include/parsec/parsec.h) carries the asset id the
// fill/order belongs to -- only the top-level pc_event does, and app::UiEvent
// (src/app/ui_bridge.hpp) does not forward it. Every row below is therefore necessarily
// asset-less until that field is threaded through app::UiEvent. This is a real limitation for
// a multi-asset session, not a display choice -- see the report from this pass.
#include <array>
#include <cstdint>

#include "app/ui_bridge.hpp"
#include "core/units.hpp"
#include "parsec/parsec.h"

namespace pc::ui {

struct FillRow {
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
    uint64_t oid{};
    uint16_t status{};  // PC_ORD_* (or PC_ORD_REJECTED, synthesized from a PC_ACK_ERR ack)
    Px px{};
    Qty sz{}, orig_sz{};
    bool is_buy{};
    bool reduce_only{};
    uint64_t recv_time_ns{};
    char err[PC_ERR_LEN]{};  // populated only for the synthesized ack-rejection rows
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

    // Toasts, oldest-first iteration: index i in [0, toast_count()) via toast_at(i).
    [[nodiscard]] size_t toast_count() const noexcept { return toast_count_; }
    [[nodiscard]] const ToastRow& toast_at(size_t i) const noexcept {
        return toasts_[(toast_head_ - toast_count_ + i) % kMaxToasts];
    }

    [[nodiscard]] size_t fill_count() const noexcept { return fill_count_; }
    [[nodiscard]] const FillRow& fill_at(size_t i) const noexcept {
        // Most-recent-first, which is how a fills table is read.
        return fills_[(fill_head_ - 1 - i + kMaxFills) % kMaxFills];
    }

    [[nodiscard]] size_t open_order_count() const noexcept { return open_order_count_; }
    [[nodiscard]] const OrderRow& open_order_at(size_t i) const noexcept { return open_orders_[i]; }

    [[nodiscard]] size_t history_count() const noexcept { return history_count_; }
    [[nodiscard]] const OrderRow& history_at(size_t i) const noexcept {
        return history_[(history_head_ - 1 - i + kMaxHistory) % kMaxHistory];
    }

    [[nodiscard]] uint64_t dropped_events() const noexcept { return dropped_; }

private:
    void on_fill(const pc_fill& f, uint64_t t) noexcept;
    void on_ack(const pc_order_ack& a, uint64_t t) noexcept;
    void on_order_update(const pc_order_update& u, uint64_t t) noexcept;
    void push_toast(const ToastRow& row) noexcept;
    void push_history(const OrderRow& row) noexcept;
    void upsert_open(const OrderRow& row) noexcept;
    void remove_open(uint64_t oid) noexcept;

    bool has_market_conn_{};
    pc_conn market_conn_{};
    bool has_user_conn_{};
    pc_conn user_conn_{};

    std::array<ToastRow, kMaxToasts> toasts_{};
    size_t toast_head_{};
    size_t toast_count_{};

    std::array<FillRow, kMaxFills> fills_{};
    size_t fill_head_{};
    size_t fill_count_{};

    // Linear, not circular -- open orders are add/remove-by-oid, not an append-only log.
    std::array<OrderRow, kMaxOpenOrders> open_orders_{};
    size_t open_order_count_{};

    std::array<OrderRow, kMaxHistory> history_{};
    size_t history_head_{};
    size_t history_count_{};

    uint64_t dropped_{};
};

// Process-wide singleton (UI thread only, like every other pc::ui:: state). A function-local
// static keeps construction lazy and avoids a static-init-order dependency between panels.
UiEventStore& event_store() noexcept;

}  // namespace pc::ui
