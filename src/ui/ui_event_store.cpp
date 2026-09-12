#include "ui/ui_event_store.hpp"

#include <cstdio>
#include <cstring>

#include "portfolio/pnl.hpp"

namespace pc::ui {

void UiEventStore::drain(app::UiBridge& bridge) noexcept {
    app::UiEvent ev{};
    while (bridge.try_pop_event(ev)) {
        switch (ev.kind) {
            case app::UiEventKind::Fill:
                on_fill(ev.asset, ev.u.fill, ev.recv_time_ns, ev.exch_time_ms);
                break;
            case app::UiEventKind::Funding:
                on_funding(ev);
                break;
            case app::UiEventKind::OrderAck:
                on_ack(ev.asset, ev.u.ack, ev.recv_time_ns);
                break;
            case app::UiEventKind::OrderUpdate:
                on_order_update(ev);
                break;
            case app::UiEventKind::Toast: {
                ToastRow row{};
                std::strncpy(row.text, ev.u.toast.text, sizeof(row.text) - 1);
                row.severity = ev.u.toast.severity;
                row.recv_time_ns = ev.recv_time_ns;
                push_toast(row);
                break;
            }
            case app::UiEventKind::Rate:
                // Rate-budget state reaches the UI through SafetySnapshot (the status bar reads
                // it there), so there is nothing session-historical to file here.
                break;
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

bool UiEventStore::claim_fill_tid(uint64_t tid) noexcept {
    if (tid == 0)
        return true;  // no id to dedupe on; taking it is the lesser evil against dropping it
    for (size_t i = 0; i < kMaxFills; ++i) {
        if (seen_tids_[i] == tid)
            return false;
    }
    seen_tids_[seen_tid_head_ % kMaxFills] = tid;
    ++seen_tid_head_;
    return true;
}

portfolio::PositionRealized UiEventStore::position_realized(uint32_t asset,
                                                            Qty szi) const noexcept {
    // Arrival order is not time order (the REST backfill replays newest-first, interleaved
    // with the live stream), so this coin's fills are sorted by venue time before the walk.
    std::array<const FillRow*, kMaxFills> mine{};
    size_t n = 0;
    for (size_t i = 0; i < fill_count_; ++i) {
        const FillRow& f = fill_at(i);
        if (f.asset == asset)
            mine[n++] = &f;
    }
    std::sort(mine.begin(), mine.begin() + static_cast<std::ptrdiff_t>(n),
              [](const FillRow* a, const FillRow* b) {
                  return a->time_ms != b->time_ms ? a->time_ms > b->time_ms : a->tid > b->tid;
              });
    return portfolio::realized_since_open(szi, mine.data(), n);
}

void UiEventStore::on_fill(uint32_t asset, const pc_fill& f, uint64_t t,
                           uint64_t exch_time_ms) noexcept {
    if (!claim_fill_tid(f.tid))
        return;

    // Realized P&L is accumulated here rather than recomputed by whoever renders it, so every
    // reader (positions, fills, a future summary line) agrees on one number -- and so the
    // accumulation happens exactly once per fill, on the same dedupe gate above.
    const Usd realized = portfolio::realized_pnl_from_fill(f);
    if (asset < kMaxAssets) {
        realized_[asset].pnl += realized;
        realized_[asset].fees += f.fee;
        ++realized_[asset].fills;
    }
    realized_total_.pnl += realized;
    realized_total_.fees += f.fee;
    ++realized_total_.fills;

    FillRow row{};
    row.asset = asset;
    row.time_ms = exch_time_ms;
    row.dir = f.dir;
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

void UiEventStore::on_ack(uint32_t asset, const pc_order_ack& a, uint64_t t) noexcept {
    // PC_ACK_RESTING/FILLED/etc carry no price/size/side of their own (see pc_order_ack) --
    // the richer `orderUpdates` stream (on_order_update below) is what actually populates the
    // open-orders and history tables. The one ack outcome worth surfacing on its own is
    // PC_ACK_ERR: the venue accepted the request but rejected this order, and that rejection
    // may otherwise never reach the user if the engine doesn't also emit a Toast for it.
    if (a.status != PC_ACK_ERR)
        return;

    ToastRow toast{};
    // A rejected order was never assigned an oid, so printing it only ever said "order 0".
    if (a.oid != 0)
        std::snprintf(toast.text, sizeof(toast.text), "order %llu rejected: %s",
                      static_cast<unsigned long long>(a.oid), a.err);
    else
        std::snprintf(toast.text, sizeof(toast.text), "order rejected: %s", a.err);
    toast.severity = 2;
    toast.recv_time_ns = t;
    push_toast(toast);

    OrderRow row{};
    row.asset = asset;
    row.oid = a.oid;
    row.status = PC_ORD_REJECTED;
    row.recv_time_ns = t;
    std::strncpy(row.err, a.err, sizeof(row.err) - 1);
    push_history(row);
}

void UiEventStore::on_order_update(const app::UiEvent& ev) noexcept {
    const pc_order_update& u = ev.u.order_update;
    const bool historical = (ev.flags & PC_F_HISTORICAL) != 0;

    // A `frontendOpenOrders` batch is the venue's authoritative answer to "what is resting
    // right now", so the open list is REBUILT from it rather than merged into. Merging was the
    // bug behind orders that had long since filled or been cancelled sitting in the panel
    // forever: any order whose terminal update was missed -- dropped from a full ring, arrived
    // while the socket was down, or carried a status this client does not model -- had no way
    // out of the list once it was in. Rebuilding cannot leak, because anything the venue no
    // longer lists is simply not re-added.
    //
    // Historical batches are excluded: they describe orders' pasts and are bracketed with the
    // same flags, so treating them as a resting-order snapshot would wipe the real one and
    // then refill it with dead orders.
    if (!historical && (ev.flags & PC_F_SNAPSHOT_BEGIN))
        open_order_count_ = 0;

    OrderRow row{};
    row.asset = ev.asset;
    row.time_ms = ev.exch_time_ms;
    row.oid = u.oid;
    row.status = u.status;
    row.px = u.px;
    row.sz = u.sz;
    row.orig_sz = u.orig_sz;
    row.trigger_px = u.trigger_px;
    row.is_trigger = u.is_trigger != 0;
    row.tpsl = u.tpsl;
    row.is_market_trigger = u.is_market_trigger != 0;
    row.is_buy = u.is_buy != 0;
    row.reduce_only = u.reduce_only != 0;
    row.recv_time_ns = ev.recv_time_ns;

    // The empty-snapshot marker: "nothing is resting" carries no order, and exists only so the
    // clear above happens at all (see fetch_open_orders). Nothing to file.
    if (u.oid == 0 && (ev.flags & PC_F_SNAPSHOT))
        return;

    // Only an order the venue calls OPEN is resting. Everything else -- filled, cancelled,
    // rejected, triggered, or a status this client does not model -- belongs to history. The
    // previous rule listed the four statuses that were terminal and treated everything else,
    // PC_ORD_UNKNOWN included, as still open, which is exactly backwards: an unrecognised
    // status is the case where claiming an order is still live is least defensible.
    if (row.status == PC_ORD_OPEN && !historical) {
        upsert_open(row);
        return;
    }

    // Same omission as upsert_open handles, but on the way out: a live terminal update carries
    // none of reduce_only/is_trigger/tpsl/is_market_trigger/trigger_px (docs/03 §W2.7), and
    // remove_open is about to discard the resting row that did. Backfill from it first, or a
    // market stop that fires mid-session is filed in history as a plain "Limit" priced at its
    // ~5% slippage bound. Historical batches carry the flags themselves and are left alone.
    if (!historical) {
        for (size_t i = 0; i < open_order_count_; ++i) {
            const OrderRow& prior = open_orders_[i];
            if (prior.oid != u.oid)
                continue;
            row.reduce_only = row.reduce_only || prior.reduce_only;
            if (prior.is_trigger && !row.is_trigger) {
                row.trigger_px = prior.trigger_px;
                row.is_trigger = true;
                row.tpsl = prior.tpsl;
                row.is_market_trigger = prior.is_market_trigger;
            }
            break;
        }
        remove_open(u.oid);
    }

    // A venue rejection reaches the UI twice: first as a PC_ACK_ERR ack (which carries the
    // REASON but no oid, price or size -- see on_ack), then as this orderUpdate (which carries
    // everything except the reason). Filed as two rows they read as two separate rejections of
    // the same order, one of them blank. Folding the update into the ack row it belongs to
    // gives one row that says both what was rejected and why.
    if (row.status == PC_ORD_REJECTED && merge_into_pending_rejection(row))
        return;
    push_history(row);
}

// The ack-sourced rejection row is only ever the newest one in the log, and only for a moment:
// the matching orderUpdate follows it within the same batch of events. Matching on "newest row,
// same asset, ack-shaped (no oid), reason still unclaimed" is therefore precise enough without
// inventing a correlation id the wire does not carry.
bool UiEventStore::merge_into_pending_rejection(const OrderRow& update) noexcept {
    if (history_count_ == 0)
        return false;
    OrderRow& newest = history_[(history_head_ - 1) % kMaxHistory];
    if (newest.status != PC_ORD_REJECTED || newest.oid != 0 || newest.err[0] == '\0' ||
        newest.asset != update.asset)
        return false;
    char reason[PC_ERR_LEN];
    std::memcpy(reason, newest.err, sizeof(reason));
    newest = update;
    std::memcpy(newest.err, reason, sizeof(newest.err));
    return true;
}

void UiEventStore::on_funding(const app::UiEvent& ev) noexcept {
    // The venue settles funding hourly per coin, so (asset, hour) identifies a payment. The
    // 30-day REST backfill and the live stream overlap by design; without this, reconnecting
    // would double every payment already on screen and the session total with it.
    for (size_t i = 0; i < funding_count_; ++i) {
        const FundingRow& seen = funding_at(i);
        if (seen.asset == ev.asset && seen.time_ms == ev.exch_time_ms)
            return;
    }

    FundingRow row{};
    row.asset = ev.asset;
    row.time_ms = ev.exch_time_ms;
    row.usdc = ev.u.funding.usdc;
    row.szi = ev.u.funding.szi;
    row.rate_1e8 = ev.u.funding.rate_1e8;
    row.n_samples = ev.u.funding.n_samples;

    funding_[funding_head_ % kMaxFunding] = row;
    ++funding_head_;
    if (funding_count_ < kMaxFunding)
        ++funding_count_;
    funding_total_ += row.usdc;
}

void UiEventStore::note_local(const char* text, uint8_t severity) noexcept {
    ToastRow row{};
    std::strncpy(row.text, text, sizeof(row.text) - 1);
    row.severity = severity;
    push_toast(row);
}

void UiEventStore::push_toast(const ToastRow& row) noexcept {
    toasts_[static_cast<size_t>(toast_head_ % kMaxToasts)] = row;
    ++toast_head_;
    if (toast_count_ < kMaxToasts)
        ++toast_count_;
}

void UiEventStore::push_history(const OrderRow& row) noexcept {
    // The one-shot historicalOrders backfill and the live orderUpdates stream overlap around
    // session start, so the same terminal order can arrive twice. (oid, status) identifies it:
    // an order reaches each terminal status once. Rejections carry no oid and are left alone --
    // merge_into_pending_rejection() already owns that case.
    if (row.oid != 0) {
        for (size_t i = 0; i < history_count_; ++i) {
            OrderRow& seen = history_[(history_head_ - 1 - i + kMaxHistory) % kMaxHistory];
            if (seen.oid != row.oid || seen.status != row.status)
                continue;
            // Prefer whichever copy actually carries a venue timestamp and trigger detail --
            // the REST shape is the richer of the two (see pc_order_update's comment).
            if (seen.time_ms == 0 && row.time_ms != 0)
                seen = row;
            else if (!seen.is_trigger && row.is_trigger)
                seen = row;
            return;
        }
    }
    history_[history_head_ % kMaxHistory] = row;
    ++history_head_;
    if (history_count_ < kMaxHistory)
        ++history_count_;
}

void UiEventStore::upsert_open(const OrderRow& row) noexcept {
    for (size_t i = 0; i < open_order_count_; ++i) {
        if (open_orders_[i].oid != row.oid)
            continue;
        const OrderRow& prior = open_orders_[i];
        // The live `orderUpdates` payload carries no trigger fields at all (docs/03 §W2.7),
        // so an update for a resting stop would otherwise erase what the frontendOpenOrders
        // snapshot already established about it -- and the chart would stop drawing it.
        const bool keep_trigger = prior.is_trigger && !row.is_trigger;
        OrderRow merged = row;
        // `reduce_only` is in the same boat and is never on the live payload either, so it is
        // kept unconditionally -- otherwise a reduce-only close blanks its Reduce Only cell
        // after every partial fill until the next snapshot heals it.
        merged.reduce_only = prior.reduce_only;
        if (keep_trigger) {
            merged.trigger_px = prior.trigger_px;
            merged.is_trigger = true;
            merged.tpsl = prior.tpsl;
            // Without this a market trigger that goes terminal mid-session is pushed into
            // history typed "Limit", priced at its ~5% slippage bound.
            merged.is_market_trigger = prior.is_market_trigger;
        }
        open_orders_[i] = merged;
        return;
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
