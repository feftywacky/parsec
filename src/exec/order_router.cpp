#include "exec/order_router.hpp"

#include <cstring>

namespace pc::exec {

// cloid layout per docs/02-architecture.md SS6.2: [8B session id][8B monotonic seq]. Written
// in native byte order -- the venue only requires the 16 bytes be unique and echoed back
// unchanged, it never interprets them as a number.
void OrderRouter::make_cloid(uint8_t out[16]) noexcept {
    const uint64_t seq = next_seq_++;
    std::memcpy(out, &session_id_, 8);
    std::memcpy(out + 8, &seq, 8);
}

RouteResult OrderRouter::build_order(const RouteRequest& req, AssetPrecision precision,
                                     const risk::RiskContext& risk_ctx) noexcept {
    RouteResult result{};

    const Qty qty = Rounder::round_sz(req.raw_qty, precision);
    if (qty <= 0) {
        result.reason = "size rounds to zero at this asset's precision";
        return result;
    }

    Px px{};
    if (req.is_market) {
        if (req.reference_px <= 0) {
            result.reason = "market order needs a reference price (bbo mid)";
            return result;
        }
        px = Rounder::marketable_px(req.reference_px, req.side, req.slippage_bps, precision);
    } else {
        px = Rounder::round_px(req.raw_px, req.side, precision, RoundMode::Passive);
    }
    if (px <= 0) {
        result.reason = "price rounds to zero (unrepresentable at this asset's precision)";
        return result;
    }

    const auto outcome = risk::check_order(risk_ctx);
    if (!outcome.ok) {
        result.risk = outcome;
        return result;
    }

    pc_order order{};
    order.asset = req.asset;
    order.is_buy = req.side == Side::Buy ? 1 : 0;
    order.reduce_only = req.reduce_only ? 1 : 0;
    order.tif = req.is_market ? PC_TIF_IOC : req.tif;
    order.tpsl = req.tpsl;
    order.limit_px = px;
    order.sz = qty;
    order.trigger_px = req.trigger_px;
    order.is_market_trigger = req.is_market_trigger;
    make_cloid(order.cloid);

    result.ok = true;
    result.order = order;
    return result;
}

OrderRecord* OrderRouter::on_submitted(pc_req_id req_id, const pc_order& order, uint64_t now_ms,
                                       uint64_t ack_timeout_ms) noexcept {
    return book_.create(req_id, order, now_ms + ack_timeout_ms);
}

}  // namespace pc::exec
