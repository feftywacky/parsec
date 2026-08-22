#include "exec/order_state.hpp"

#include <cstring>

namespace pc::exec {
namespace {

bool is_terminal(OrderState s) noexcept {
    return s == OrderState::Filled || s == OrderState::Canceled || s == OrderState::Rejected;
}

// Maps pc_order_update::status (PC_ORD_* in parsec.h) onto the local state machine drawn in
// docs/02-architecture.md SS6.2.
OrderState from_order_update_status(uint16_t status) noexcept {
    switch (status) {
        case PC_ORD_OPEN:
        case PC_ORD_TRIGGERED:
            return OrderState::Resting;
        case PC_ORD_FILLED:
            return OrderState::Filled;
        case PC_ORD_CANCELED:
        case PC_ORD_MARGIN_CANCELED:
            return OrderState::Canceled;
        case PC_ORD_REJECTED:
            return OrderState::Rejected;
        case PC_ORD_UNKNOWN:
        default:
            // 03 SS W2.7: the status enum is open-ended -- classify by the suffix convention
            // where we recognize it, and fall back to Unknown (forces reconciliation) rather
            // than guessing for anything new.
            return OrderState::Unknown;
    }
}

// Maps pc_order_ack::status (PC_ACK_* in parsec.h) onto the local state machine.
OrderState from_ack_status(uint16_t status) noexcept {
    switch (status) {
        case PC_ACK_RESTING:
        case PC_ACK_WAITING_FOR_FILL:
        case PC_ACK_WAITING_FOR_TRIGGER:
        case PC_ACK_SUCCESS:
            return OrderState::Resting;
        case PC_ACK_FILLED:
            return OrderState::Filled;
        case PC_ACK_ERR:
            return OrderState::Rejected;
        case PC_ACK_TIMEOUT:
        default:
            // The venue never definitively answered this submission; treat exactly like a
            // PendingNew deadline expiry -- Unknown, forces reconciliation, never resend.
            return OrderState::Unknown;
    }
}

}  // namespace

OrderRecord* OrderStateBook::create(pc_req_id req_id, const pc_order& order,
                                    uint64_t deadline_ms) noexcept {
    if (size_ == kCapacity)
        return nullptr;
    auto& r = orders_[size_++];
    r = {};
    r.req_id = req_id;
    r.order = order;
    std::memcpy(r.cloid, order.cloid, 16);
    r.deadline_ms = deadline_ms;
    return &r;
}

OrderRecord* OrderStateBook::find_req_id(pc_req_id req_id) noexcept {
    if (req_id == 0)
        return nullptr;  // 0 is "no command", never a valid correlation key
    for (size_t i = 0; i < size_; ++i)
        if (orders_[i].req_id == req_id)
            return &orders_[i];
    return nullptr;
}

OrderRecord* OrderStateBook::find_oid(uint64_t oid) noexcept {
    if (oid == 0)
        return nullptr;
    for (size_t i = 0; i < size_; ++i)
        if (orders_[i].oid == oid)
            return &orders_[i];
    return nullptr;
}

OrderRecord* OrderStateBook::find_cloid(const uint8_t cloid[16]) noexcept {
    for (size_t i = 0; i < size_; ++i)
        if (std::memcmp(orders_[i].cloid, cloid, 16) == 0)
            return &orders_[i];
    return nullptr;
}

// See the class comment in order_state.hpp: correlation is strictly req_id -> record. A miss
// is dropped, not guessed. This is deliberately the ONLY place that binds oid <- req_id;
// every other path (apply_update, apply_fill) trusts an oid that was already bound here.
void OrderStateBook::apply_ack(pc_req_id req_id, const pc_order_ack& ack) noexcept {
    auto* r = find_req_id(req_id);
    if (!r)
        return;  // unrecognized submission; let the PendingNew deadline (expire()) handle it
    if (ack.oid != 0)
        r->oid = ack.oid;
    r->state = from_ack_status(ack.status);
    if (r->state != OrderState::PendingNew)
        r->deadline_ms = 0;  // no longer waiting on an ack
}

void OrderStateBook::apply_update(const pc_order_update& update) noexcept {
    auto* r = find_oid(update.oid);
    if (!r)
        return;
    if (is_terminal(r->state))
        return;  // terminal states don't un-terminate on a stale/late update
    r->state = from_order_update_status(update.status);
}

void OrderStateBook::apply_fill(const pc_fill& fill) noexcept {
    auto* r = find_oid(fill.oid);
    if (!r)
        return;
    if (r->state == OrderState::Filled || r->state == OrderState::Canceled ||
        r->state == OrderState::Rejected)
        return;
    // A fill always means the order is at least resting/partially filled; whether it is fully
    // filled is authoritatively reported by the accompanying orderUpdates message (03 SS W2.7),
    // which apply_update will apply. Locally we only ever optimistically mark "some fill
    // happened" -- OrderRouter/Reconciler is where local fills get reconciled against
    // userFills (docs/02 SS6.2: "the router never trusts a local fill").
    r->state = OrderState::PartiallyFilled;
}

void OrderStateBook::expire(uint64_t now_ms) noexcept {
    for (size_t i = 0; i < size_; ++i) {
        auto& r = orders_[i];
        if (r.state == OrderState::PendingNew && r.deadline_ms && now_ms >= r.deadline_ms)
            r.state = OrderState::Unknown;
    }
}

}  // namespace pc::exec
