#pragma once
#include "core/units.hpp"
#include "parsec/parsec.h"
namespace pc::portfolio {

// Account-level truth, kept in the same two-tier shape as PositionBook (docs/02
// architecture.md SS6.4): `authoritative` is the venue's own `clearinghouseState` snapshot,
// re-pushed in full every ~4s; `optimistic` is nudged locally between snapshots so the UI's
// account value/margin move in the same frame a fill lands, rather than waiting for the next
// snapshot. Reconciler is what brings `optimistic` back in line when it has drifted.
class AccountState {
public:
    // Re-bases both the authoritative and optimistic views on a fresh venue snapshot.
    void apply_authoritative(const pc_account& acct) noexcept;

    // Nudges the optimistic projection for a fill that just landed locally, before the next
    // clearinghouseState snapshot confirms it. `realized_pnl_delta` and `fee` are both already
    // in Usd (1e8-scaled); `fee` must be the wire `fee` field, which already includes
    // `builderFee` -- adding builderFee again here would double-count it (03 SS W2.9).
    void apply_fill_delta(Usd realized_pnl_delta, Usd fee) noexcept;

    const pc_account* authoritative() const noexcept {
        return has_snapshot_ ? &authoritative_ : nullptr;
    }
    const pc_account& optimistic() const noexcept { return optimistic_; }
    bool has_snapshot() const noexcept { return has_snapshot_; }

private:
    pc_account authoritative_{};
    pc_account optimistic_{};
    bool has_snapshot_{};
};

}  // namespace pc::portfolio
