#pragma once
// How an order describes itself in a table cell. Shared by Open Orders and Order History so
// the same resting order cannot be named one thing while it rests and another once it is
// terminal -- these two panels are read side by side, and disagreeing about an order's type or
// trigger is worse than either label being slightly off on its own.
//
// Everything here is derived from the flags that survive the FFI (is_trigger / tpsl /
// is_market_trigger / reduce_only) rather than from a venue-supplied string.
#include <cstdio>

#include "parsec/parsec.h"
#include "ui/ui_event_store.hpp"
#include "ui/widgets/number_fmt.hpp"

namespace pc::ui {

// The venue's own naming for the order type. A trigger order's type is the single most
// load-bearing thing in these tables: "Stop Market" and "Limit" at the same price behave
// nothing alike.
inline const char* type_text(const OrderRow& r) noexcept {
    if (!r.is_trigger)
        return "Limit";
    if (r.tpsl == PC_TPSL_TP)
        return r.is_market_trigger ? "Take Profit Market" : "Take Profit Limit";
    if (r.tpsl == PC_TPSL_SL)
        return r.is_market_trigger ? "Stop Market" : "Stop Limit";
    return r.is_market_trigger ? "Trigger Market" : "Trigger Limit";
}

// What the order does to the position. A reduce-only order can only ever close, and its side
// tells you which way the position it closes points; an ordinary order opens.
inline const char* direction_text(const OrderRow& r) noexcept {
    if (r.reduce_only)
        return r.is_buy ? "Close Short" : "Close Long";
    return r.is_buy ? "Open Long" : "Open Short";
}

// "Price above 0.19948" / "Price below 0.15931" -- the condition the venue is actually
// watching. Derived from the leg and the closing side: a stop on a short fires when price
// rises, a take-profit on the same short fires when it falls.
inline void trigger_condition(const OrderRow& r, uint8_t sz_decimals, char* out,
                              size_t cap) noexcept {
    if (!r.is_trigger || r.trigger_px <= 0) {
        std::snprintf(out, cap, "--");
        return;
    }
    char px_buf[32];
    format_px(r.trigger_px, sz_decimals, px_buf, sizeof(px_buf));
    // A buy trigger closes a short: a stop on it fires above, a take-profit below. A sell
    // trigger closes a long and is the mirror.
    const bool above = (r.tpsl == PC_TPSL_SL) == r.is_buy;
    std::snprintf(out, cap, "Price %s %s", above ? "above" : "below", px_buf);
}

}  // namespace pc::ui
