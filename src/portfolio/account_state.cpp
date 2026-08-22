#include "portfolio/account_state.hpp"

namespace pc::portfolio {

void AccountState::apply_authoritative(const pc_account& acct) noexcept {
    authoritative_ = acct;
    optimistic_ = acct;
    has_snapshot_ = true;
}

void AccountState::apply_fill_delta(Usd realized_pnl_delta, Usd fee) noexcept {
    optimistic_.account_value += realized_pnl_delta - fee;
    optimistic_.withdrawable += realized_pnl_delta - fee;
}

}  // namespace pc::portfolio
