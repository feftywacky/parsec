#pragma once
#include <array>
#include <cstdint>

#include "parsec/parsec.h"
namespace pc::exec {

enum class OrderState : uint8_t {
    PendingNew,
    Resting,
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected,
    Unknown
};

struct OrderRecord {
    pc_req_id req_id{};  // req_id returned by pc_place_order for this order's submission
    uint8_t cloid[16]{};
    uint64_t oid{};          // 0 until the venue's ack assigns one
    uint64_t deadline_ms{};  // PendingNew ack deadline; 0 once acked
    pc_order order{};
    OrderState state{OrderState::PendingNew};
};

// Local order book keyed primarily by req_id (the FFI's own correlation id, returned
// synchronously by pc_place_order -- see docs/02-architecture.md SS5.3) and secondarily by
// oid, once the venue assigns one.
//
// pc_order_ack (include/parsec/parsec.h) carries an oid but no cloid, so an ack cannot be
// matched to "the order with this cloid" directly. It CAN always be matched to "the order
// submitted under this req_id", because pc_event::req_id always echoes the id the placing
// command returned (docs/02 SS5.4). That is the only correlation this book trusts: if a
// req_id lookup fails, the ack is dropped rather than guessed onto some other PendingNew
// record -- with two orders in flight, guessing by array position silently attaches an ack to
// the wrong order (that was the bug this rewrite fixes). An order whose ack never arrives
// simply rides out its PendingNew deadline and flips to Unknown via expire(), which is exactly
// the documented "never auto-resend, force reconciliation" path (docs/02 SS6.2, SS8).
class OrderStateBook {
public:
    static constexpr size_t kCapacity = 1024;
    static constexpr uint64_t kDefaultAckTimeoutMs = 5'000;

    // Registers a freshly-submitted order as PendingNew under the req_id pc_place_order just
    // returned. `deadline_ms` is typically now_ms + order_ack_timeout_ms.
    OrderRecord* create(pc_req_id req_id, const pc_order& order, uint64_t deadline_ms) noexcept;

    OrderRecord* find_req_id(pc_req_id) noexcept;
    OrderRecord* find_oid(uint64_t) noexcept;
    OrderRecord* find_cloid(const uint8_t cloid[16]) noexcept;

    // Correlates strictly on req_id (see class comment). Binds the venue-assigned oid to the
    // matched record and advances its state. If no record is registered under this req_id,
    // the ack is dropped -- never bound heuristically to some other record.
    void apply_ack(pc_req_id req_id, const pc_order_ack& ack) noexcept;

    // orderUpdates and fills carry oid, which is reliable once an ack has bound it.
    void apply_update(const pc_order_update& update) noexcept;
    void apply_fill(const pc_fill& fill) noexcept;

    // Sweeps every still-PendingNew record whose deadline has passed to Unknown. Called once
    // per engine tick (docs/02 SS4.2 `tick_timers()`).
    void expire(uint64_t now_ms) noexcept;

    size_t size() const noexcept { return size_; }
    OrderRecord& at(size_t i) noexcept { return orders_[i]; }

private:
    std::array<OrderRecord, kCapacity> orders_{};
    size_t size_{};
};

}  // namespace pc::exec
