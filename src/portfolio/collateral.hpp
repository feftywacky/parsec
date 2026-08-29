#pragma once

#include <algorithm>

#include "core/units.hpp"
#include "parsec/parsec.h"
#include "portfolio/order_preview.hpp"

namespace pc::portfolio {

// The USDC in the account that is NOT deployed as perp equity.
//
// Hyperliquid runs ONE USDC collateral pool. `spotClearinghouseState`'s USDC `total` is every
// USDC the account holds, and the part currently working as perp equity is counted inside it
// -- so `total - marginSummary.accountValue` is the balance sitting idle, and subtracting the
// perp side is what makes this "undeployed" rather than a double count of the same dollars.
//
// Deliberately NOT `total - hold`. `hold` is the venue's own locked figure and lags the mark;
// the venue's Balances tab uses it and it is right for a *displayed* balance, but it ran ~$20
// off the venue's own margin answer on a measured account, so it does not belong in a margin
// decision. See free_collateral().
inline Usd undeployed_usdc(Usd account_value, const pc_spot& spot) noexcept {
    return std::max<Usd>(0, spot.total - account_value);
}

// Free collateral: what can actually back a NEW position.
//
// `withdrawable` alone is the perp side's answer and misses every USDC that has not been
// deployed into perps yet. On a measured mainnet account it read $37.46 while the venue's own
// `activeAssetData.availableToTrade` for the opening side read $1,471.24 -- a 39x
// under-report that made the ticket block orders at sizes its own `maxTradeSzs` slider
// offered. Adding the undeployed balance reproduces the venue's figure to within $0.02
// (tests/cpp/test_collateral.cpp holds the snapshot).
//
// `account_value` is `marginSummary.accountValue` (cross AND isolated): isolated collateral is
// deployed too, so it must be netted out of the pool along with the cross side.
//
// Falls back to `withdrawable` alone until a spot snapshot lands: understating free margin
// blocks an order, overstating it lets one through for the venue to reject.
inline Usd free_collateral(Usd account_value, Usd withdrawable, const pc_spot& spot,
                           bool spot_valid) noexcept {
    const Usd perp = std::max<Usd>(0, withdrawable);
    return spot_valid ? perp + undeployed_usdc(account_value, spot) : perp;
}

inline Usd free_collateral(const pc_account& account, const pc_spot& spot,
                           bool spot_valid) noexcept {
    return free_collateral(account.account_value, account.withdrawable, spot, spot_valid);
}

// The USDC actually posted to open this position -- entry notional divided by leverage.
//
// NOT `margin_used`, which is the venue's marked-to-market requirement and moves with the
// price: on a cross position it is `position_value / leverage`, and on an isolated one it is
// the position's whole current equity (`positionValue + rawUsd`), so both fold unrealized P&L
// into a number that reads like a deposit. This one is fixed at entry and answers "how much
// real money is in this trade", which is what sizing decisions are made against.
//
// Approximate for a position built in several fills at different leverage settings, or one an
// isolated top-up has added collateral to: the venue publishes only the average entry price
// and the current leverage, so those cases are priced at the current leverage.
inline Usd position_cost_basis(const pc_position& p) noexcept {
    const Qty abs_size = p.szi < 0 ? -p.szi : p.szi;
    return initial_margin(notional(p.entry_px, abs_size), p.leverage);
}

// Free collateral with open P&L stripped out: the cash actually in the account, as opposed to
// the venue's marked-to-market buying power.
//
// NOT `free_collateral() - total_unrealized`, which was this function's first form and is
// wrong at low leverage. Free collateral only ever contains the PART of an open gain that is
// not immediately re-locked as margin: on a cross position the venue moves withdrawable by
// `d_unrealized - d_unrealized / leverage`, so at 1x it does not move at all -- the whole gain
// is absorbed by the position's own growing requirement. Subtracting the full unrealized P&L
// there removed money that was never counted in the first place, and a $385 open profit at 1x
// showed up as a $385 gap between two lines that should have been equal.
//
// Built from the account instead: equity valued at entry is `account_value - unrealized`, the
// posted margin is the summed cost basis, and idle USDC has never been in a position at all.
//
//     cash = undeployed + (account_value - total_unrealized) - total_cost_basis
//
// Capped at `free`, which nets out margin locked by RESTING orders -- something no position
// figure can see. Without the cap, cash could exceed the money the venue would actually let a
// new order spend.
inline Usd cash_collateral(Usd free, Usd account_value, Usd total_unrealized,
                           Usd total_cost_basis, const pc_spot& spot, bool spot_valid) noexcept {
    const Usd idle = spot_valid ? undeployed_usdc(account_value, spot) : 0;
    const Usd cash = idle + (account_value - total_unrealized) - total_cost_basis;
    return std::max<Usd>(0, std::min(cash, free));
}

}  // namespace pc::portfolio
