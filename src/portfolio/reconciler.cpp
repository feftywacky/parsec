#include "portfolio/reconciler.hpp"

#include "core/units.hpp"

namespace pc::portfolio {

std::optional<DivergenceLog> Reconciler::reconcile(uint32_t asset, pc_position& local,
                                                   const pc_position& venue,
                                                   Qty tolerance) noexcept {
    const Qty diff = abs_px(local.szi - venue.szi);
    if (asset >= PositionBook::kMaxAssets)
        return std::nullopt;  // out of range; nothing to track, caller's problem to size right

    if (diff <= tolerance) {
        consecutive_[asset] = 0;
        return std::nullopt;
    }

    DivergenceLog log{};
    log.asset = asset;
    log.local_szi = local.szi;
    log.venue_szi = venue.szi;
    log.size_diff = diff;

    // Snap to venue truth. This is the whole point: an optimistic projection that has drifted
    // is worse than useless, so it is replaced outright rather than merged.
    local = venue;

    if (consecutive_[asset] < 255)
        ++consecutive_[asset];
    log.alarm = consecutive_[asset] >= 2;

    return log;
}

void Reconciler::reset(uint32_t asset) noexcept {
    if (asset < PositionBook::kMaxAssets)
        consecutive_[asset] = 0;
}

}  // namespace pc::portfolio
