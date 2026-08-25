#pragma once
// Resolving a dense asset index into the things a table row needs to render itself.
//
// Every event-sourced row (fill, order, position) carries the asset id it belongs to, but the
// venue symbol and that asset's szDecimals live in the `meta` universe snapshot. Formatting a
// row at the ACTIVE instrument's precision -- which is what these panels did before the asset
// id was threaded through -- silently prints another coin's price and size at the wrong number
// of decimals, which is the same class of error as showing the wrong symbol.
#include <cstdint>

#include "app/ui_bridge.hpp"
#include "parsec/parsec.h"

namespace pc::ui {

struct AssetLabel {
    const char* coin{"--"};  // never a bare index: an unresolved id says so rather than lying
    uint8_t sz_decimals{};
    uint32_t max_leverage{};
    bool resolved{};
};

// `fallback_sz_decimals` is used only when the id cannot be resolved (an event for an asset
// that is not in the universe snapshot yet); pass the active instrument's precision so the row
// still renders sensibly instead of collapsing to 0 decimals.
inline AssetLabel lookup_asset(const app::AssetUniverseSnapshot& universe, uint32_t asset,
                               uint8_t fallback_sz_decimals = 0) noexcept {
    AssetLabel out{};
    out.sz_decimals = fallback_sz_decimals;
    if (asset == PC_ASSET_NONE)
        return out;
    for (uint32_t i = 0; i < universe.count; ++i) {
        const app::AssetOption& option = universe.assets[i];
        if (option.asset != asset)
            continue;
        if (option.name[0] != '\0')
            out.coin = option.name;
        out.sz_decimals = option.sz_decimals;
        out.max_leverage = option.max_leverage;
        out.resolved = true;
        return out;
    }
    return out;
}

}  // namespace pc::ui
