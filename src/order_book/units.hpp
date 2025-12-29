#pragma once

#include <cstdint>

namespace ob {

struct BtcPerpConfig {
    // Hyperliquid precision model
    // Sizes are represented with `szDecimals` decimal places.
    // Prices are represented with up to (6 - szDecimals) decimal places.
    // szDecimals for an asset is found in the meta response to the info endpoint
    // https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/info-endpoint
    static constexpr int32_t sz_decimals = 5;

    // Fixed-point scales (powers of 10).
    // sz_decimals=5 => size_scale=100000 => 1 unit = 0.00001 BTC
    // max_price_decimals=1 => price_scale=10 => 1 unit = 0.1 price
    static constexpr int64_t size_scale = 100000;
    static constexpr int64_t price_scale = 10;

    static constexpr double price_to_double(int64_t price_scaled) {
        return static_cast<double>(price_scaled) / static_cast<double>(price_scale);
    }

    static constexpr double size_to_double(int64_t size_scaled) {
        return static_cast<double>(size_scaled) / static_cast<double>(size_scale);
    }
};

} // namespace ob
