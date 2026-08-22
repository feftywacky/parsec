#pragma once
#include <array>
#include <cstdint>
#include <optional>

#include "core/units.hpp"
#include "parsec/parsec.h"
#include "portfolio/position_book.hpp"
namespace pc::portfolio {

// One reconciliation event, always logged with both values so a human (or a test) can see
// exactly what diverged -- docs/02 SS6.4: "logs a RECONCILE_DIVERGENCE line with both values".
struct DivergenceLog {
    uint32_t asset{};
    Qty local_szi{};
    Qty venue_szi{};
    Qty size_diff{};  // abs(local_szi - venue_szi), the magnitude that tripped the check
    // True on the SECOND consecutive divergence for this asset -- docs/02 SS6.4: "if it
    // happens twice in a row) trips a soft alarm in the UI". A single divergence is treated
    // as an ordinary snap-to-venue; only a repeat is loud.
    bool alarm{};
};

// Compares the optimistic local position projection against the authoritative venue snapshot
// (`clearinghouseState`, ~4s cadence) and snaps local state to venue truth on divergence beyond
// tolerance. Silent drift between what the client thinks it holds and what it actually holds is
// treated as the most dangerous bug class in a trading client (docs/02 SS6.4), so every
// divergence is reported, not just logged quietly.
class Reconciler {
public:
    // Compares `local` (the optimistic position for `asset`) against `venue` (the freshly
    // received authoritative snapshot for the same asset). If the size divergence is within
    // `tolerance`, resets this asset's consecutive-divergence counter and returns nullopt --
    // nothing to report, `local` is left untouched. Otherwise snaps `local` to `venue` in
    // place, bumps the counter, and returns a DivergenceLog with `alarm` set once the counter
    // reaches 2 (i.e. this asset has now diverged on two checks in a row).
    std::optional<DivergenceLog> reconcile(uint32_t asset, pc_position& local,
                                           const pc_position& venue, Qty tolerance) noexcept;

    // A clean reconciliation elsewhere in the account (or a fresh snapshot after a reconnect)
    // should not leave a stale "this asset diverged last time" flag hanging around forever;
    // call this once per asset that reconciled cleanly, or on a full snapshot resync.
    void reset(uint32_t asset) noexcept;

private:
    std::array<uint8_t, PositionBook::kMaxAssets> consecutive_{};
};

}  // namespace pc::portfolio
