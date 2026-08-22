#include "ui/ui_event_store.hpp"

#include <cstdio>
#include <cstring>

namespace pc::ui {

void UiEventStore::drain(app::UiBridge& bridge) noexcept {
    app::UiEvent ev{};
    while (bridge.try_pop_event(ev)) {
        switch (ev.kind) {
            case app::UiEventKind::Fill:
                on_fill(ev.u.fill, ev.recv_time_ns);
                break;
            case app::UiEventKind::OrderAck:
                on_ack(ev.u.ack, ev.recv_time_ns);
                break;
            case app::UiEventKind::OrderUpdate:
                on_order_update(ev.u.order_update, ev.recv_time_ns);
                break;
            case app::UiEventKind::Toast: {
                ToastRow row{};
                std::strncpy(row.text, ev.u.toast.text, sizeof(row.text) - 1);
                row.severity = ev.u.toast.severity;
                row.recv_time_ns = ev.recv_time_ns;
                push_toast(row);
                break;
            }
            case app::UiEventKind::ConnState:
                if (ev.u.conn.socket == PC_SOCK_MARKET) {
                    market_conn_ = ev.u.conn;
                    has_market_conn_ = true;
                } else {
                    user_conn_ = ev.u.conn;
                    has_user_conn_ = true;
                }
                break;
        }
    }
    dropped_ = bridge.dropped_events();
}

void UiEventStore::on_fill(const pc_fill& f, uint64_t t) noexcept {
    FillRow row{};
    row.oid = f.oid;
    row.tid = f.tid;
    row.px = f.px;
    row.qty = f.qty;
    row.fee = f.fee;
    row.closed_pnl = f.closed_pnl;
    row.is_buy = f.is_buy != 0;
    row.is_taker = f.is_taker != 0;
    row.recv_time_ns = t;
    fills_[fill_head_ % kMaxFills] = row;
    ++fill_head_;
    if (fill_count_ < kMaxFills)
        ++fill_count_;
}

void UiEventStore::on_ack(const pc_order_ack& a, uint64_t t) noexcept {
    // PC_ACK_RESTING/FILLED/etc carry no price/size/side of their own (see pc_order_ack) --
    // the richer `orderUpdates` stream (on_order_update below) is what actually populates the
    // open-orders and history tables. The one ack outcome worth surfacing on its own is
    // PC_ACK_ERR: the venue accepted the request but rejected this order, and that rejection
    // may otherwise never reach the user if the engine doesn't also emit a Toast for it.
    if (a.status != PC_ACK_ERR)
        return;

    ToastRow toast{};
    std::snprintf(toast.text, sizeof(toast.text), "order %llu rejected: %s",
                  static_cast<unsigned long long>(a.oid), a.err);
    toast.severity = 2;
    toast.recv_time_ns = t;
    push_toast(toast);

    OrderRow row{};
    row.oid = a.oid;
    row.status = PC_ORD_REJECTED;
    row.recv_time_ns = t;
    std::strncpy(row.err, a.err, sizeof(row.err) - 1);
    push_history(row);
}

void UiEventStore::on_order_update(const pc_order_update& u, uint64_t t) noexcept {
    OrderRow row{};
    row.oid = u.oid;
    row.status = u.status;
    row.px = u.px;
    row.sz = u.sz;
    row.orig_sz = u.orig_sz;
    row.is_buy = u.is_buy != 0;
    row.reduce_only = u.reduce_only != 0;
    row.recv_time_ns = t;

    const bool terminal = u.status == PC_ORD_FILLED || u.status == PC_ORD_CANCELED ||
                          u.status == PC_ORD_REJECTED || u.status == PC_ORD_MARGIN_CANCELED;
    if (terminal) {
        remove_open(u.oid);
        push_history(row);
    } else {
        upsert_open(row);
    }
}

void UiEventStore::note_local(const char* text, uint8_t severity) noexcept {
    ToastRow row{};
    std::strncpy(row.text, text, sizeof(row.text) - 1);
    row.severity = severity;
    push_toast(row);
}

void UiEventStore::push_toast(const ToastRow& row) noexcept {
    toasts_[toast_head_ % kMaxToasts] = row;
    ++toast_head_;
    if (toast_count_ < kMaxToasts)
        ++toast_count_;
}

void UiEventStore::push_history(const OrderRow& row) noexcept {
    history_[history_head_ % kMaxHistory] = row;
    ++history_head_;
    if (history_count_ < kMaxHistory)
        ++history_count_;
}

void UiEventStore::upsert_open(const OrderRow& row) noexcept {
    for (size_t i = 0; i < open_order_count_; ++i) {
        if (open_orders_[i].oid == row.oid) {
            open_orders_[i] = row;
            return;
        }
    }
    if (open_order_count_ < kMaxOpenOrders)
        open_orders_[open_order_count_++] = row;
    // else: at capacity -- silently drop rather than allocate. A real session's resting-order
    // count is nowhere near 128; hitting this means something else has gone wrong upstream.
}

void UiEventStore::remove_open(uint64_t oid) noexcept {
    for (size_t i = 0; i < open_order_count_; ++i) {
        if (open_orders_[i].oid == oid) {
            open_orders_[i] = open_orders_[open_order_count_ - 1];
            --open_order_count_;
            return;
        }
    }
}

UiEventStore& event_store() noexcept {
    static UiEventStore store;
    return store;
}

}  // namespace pc::ui
