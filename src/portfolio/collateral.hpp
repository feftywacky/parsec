#pragma once

#include <algorithm>

#include "core/units.hpp"
#include "parsec/parsec.h"

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

}  // namespace pc::portfolio
