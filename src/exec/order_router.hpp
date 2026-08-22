#pragma once
#include <cstdint>

#include "exec/order_state.hpp"
#include "exec/rounder.hpp"
#include "parsec/parsec.h"
#include "risk/pre_trade.hpp"
namespace pc::exec {

// Everything OrderRouter needs to know about a single trader intent. The caller (engine/app
// layer) fills this in from the ticket plus current market/account state; OrderRouter itself
// touches no I/O and holds no market/portfolio state beyond its own order book.
struct RouteRequest {
    uint32_t asset{};
    Side side{Side::Buy};
    Qty raw_qty{};
    Px raw_px{};  // ignored when is_market is set

    uint8_t tif{PC_TIF_GTC};
    bool reduce_only{};

    // Market ("aggressive IOC") orders: price is derived from reference_px, not raw_px.
    // docs/03-hyperliquid-api.md "Market orders -- how they are actually done".
    bool is_market{};
    Px reference_px{};  // the bbo mid; required when is_market is set
    uint32_t slippage_bps{Rounder::kDefaultSlippageBps};

    uint8_t tpsl{PC_TPSL_NONE};
    Px trigger_px{};
    uint8_t is_market_trigger{};

    uint32_t leverage{1};  // the asset's currently configured leverage, for the risk check
};

// Result of build_order(): either a fully rounded, risk-checked pc_order ready to hand to
// pc_place_order verbatim, or a reason it was refused before ever reaching the network.
struct RouteResult {
    bool ok{};
    pc_order order{};
    // Populated when !ok. If the rejection came from a risk check, `risk` names it and carries
    // the excess; otherwise `reason` is a router-local message (e.g. rounding produced an
    // unrepresentable price/size) and `risk.check` is left at its "ok" default.
    risk::CheckOutcome risk{};
    const char* reason{nullptr};
};

// Owns local truth about every order this session has sent (docs/02-architecture.md SS6.2):
// generates cloids, applies Rounder and the pre-trade risk checks before anything can reach
// pc_place_order, and tracks each order through OrderStateBook once it has actually been
// submitted. OrderRouter deliberately never calls any pc_* function itself -- it hands the app
// layer a validated request, and the app layer is the one that talks to the FFI boundary and
// reports back the req_id via on_submitted().
class OrderRouter {
public:
    explicit OrderRouter(uint64_t session_id) noexcept : session_id_(session_id) {}

    // Rounds price/size, runs the pre-trade risk gate, and (on success) returns a populated
    // pc_order with a fresh cloid. Does not touch OrderStateBook -- that only happens once the
    // caller has actually submitted the order, via on_submitted().
    RouteResult build_order(const RouteRequest& req, AssetPrecision precision,
                            const risk::RiskContext& risk_ctx, const risk::Limits& limits) noexcept;

    // Call once pc_place_order has returned req_id for `order` (as built by build_order), to
    // start tracking it as PendingNew with an ack deadline of now_ms + ack_timeout_ms.
    OrderRecord* on_submitted(
        pc_req_id req_id, const pc_order& order, uint64_t now_ms,
        uint64_t ack_timeout_ms = OrderStateBook::kDefaultAckTimeoutMs) noexcept;

    OrderStateBook& state() noexcept { return book_; }
    const OrderStateBook& state() const noexcept { return book_; }

private:
    void make_cloid(uint8_t out[16]) noexcept;

    uint64_t session_id_;
    uint64_t next_seq_{1};
    OrderStateBook book_;
};

}  // namespace pc::exec
