#pragma once
#include <array>
#include <cstdint>

#include "parsec/parsec.h"
namespace pc::portfolio {

struct Position {
    bool active{};
    // Dense asset index this position belongs to. PositionBook is keyed by it, but the UI
    // snapshot compacts the sparse table into a list, so the row has to carry its own id --
    // otherwise every positions/fills table renders without a symbol.
    uint32_t asset{PC_ASSET_NONE};
    pc_position value{};
};

// Flat, fixed-size table keyed by asset id (not a map) so apply()/find() are branch-predictable
// O(1) with no allocation on the hot event-processing path.
class PositionBook {
public:
    static constexpr size_t kMaxAssets = 512;

    void apply(uint32_t asset, const pc_position& p) noexcept {
        if (asset < kMaxAssets)
            values_[asset] = {true, asset, p};
    }
    const Position* find(uint32_t asset) const noexcept {
        return asset < kMaxAssets && values_[asset].active ? &values_[asset] : nullptr;
    }
    void clear() noexcept { values_ = {}; }

private:
    std::array<Position, kMaxAssets> values_{};
};

}  // namespace pc::portfolio
