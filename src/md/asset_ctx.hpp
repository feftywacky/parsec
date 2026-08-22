#pragma once
#include <cstdint>
#include <type_traits>

#include "core/units.hpp"
#include "parsec/parsec.h"

namespace pc::md {

// Mark/oracle/funding/OI context for one asset (docs/02 §6.1): everything the header strip
// and the risk math need besides the touch price itself. Fed by `activeAssetCtx`, ~1 msg/s
// (03 §W2.12). Named accessors instead of exposing `pc_asset_ctx` fields directly so call
// sites read as intent ("mark_px()", "funding_rate_bps()") rather than bare struct field
// access, and so a future venue-side rename of the wire field doesn't ripple through every
// caller.
class AssetCtx {
public:
    void apply(const pc_asset_ctx& update, uint64_t exch_time_ms, uint64_t recv_time_ns) noexcept {
        value = update;
        exch_time_ms_ = exch_time_ms;
        recv_time_ns_ = recv_time_ns;
    }

    [[nodiscard]] Px mark_px() const noexcept { return value.mark; }
    [[nodiscard]] Px oracle_px() const noexcept { return value.oracle; }
    [[nodiscard]] Px mid_px() const noexcept { return value.mid; }
    [[nodiscard]] Px prev_day_px() const noexcept { return value.prev_day; }
    [[nodiscard]] Usd day_notional_volume() const noexcept { return value.day_ntl_vlm; }
    [[nodiscard]] Usd open_interest() const noexcept { return value.open_interest; }
    // Raw funding rate, scaled by kScale like every other fixed-point quantity here (the wire
    // value is a small decimal string, e.g. "0.0000125" -- parsed by Rust at the edge, never a
    // float on this side per docs/02 §2).
    [[nodiscard]] int64_t funding_rate_1e8() const noexcept { return value.funding_1e8; }
    [[nodiscard]] uint64_t exch_time_ms() const noexcept { return exch_time_ms_; }
    [[nodiscard]] uint64_t recv_time_ns() const noexcept { return recv_time_ns_; }

    // Public raw payload for the same reason as md::Bbo::value -- trivially copyable POD, and
    // needed whole for duplicate-payload hashing in md::AssetStaleness.
    pc_asset_ctx value{};

private:
    uint64_t exch_time_ms_{};
    uint64_t recv_time_ns_{};
};
static_assert(std::is_trivially_copyable_v<AssetCtx>);

}  // namespace pc::md
