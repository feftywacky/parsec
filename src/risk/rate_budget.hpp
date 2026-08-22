#pragma once
#include <cstdint>
namespace pc::risk {

// Tracks the per-address action budget (docs/03-hyperliquid-api.md SS7: 1 request per 1 USDC
// of cumulative traded volume since address inception, 10000-request initial buffer). The
// venue never reports the cap directly over the wire -- PC_EV_RATE (pc_rate) only carries
// `remaining` and a reset time -- so this estimates the cap as the highest `remaining` value
// observed since startup (or since the last reset), which only ever improves as more events
// arrive and is conservative by construction: it can only under-estimate how much budget is
// actually available, never over-estimate it.
//
// docs/02-architecture.md SS8: "risk layer starts rejecting new orders at <10% budget."
class RateBudget {
public:
    static constexpr int32_t kRejectThresholdBps = 1'000;  // 10%

    // Feed every PC_EV_RATE event here.
    void update(int64_t remaining, uint64_t reset_ms) noexcept;

    // Fraction of the estimated budget remaining, in basis points (0..10000). With no data yet
    // this reads 10000 (optimistic default -- the check that consumes this treats "unknown"
    // as "don't block", since blocking every order before the first PC_EV_RATE arrives would
    // make the client unusable at startup).
    int32_t remaining_bps() const noexcept;

    bool should_reject_new_orders() const noexcept { return remaining_bps() < kRejectThresholdBps; }

    int64_t remaining() const noexcept { return remaining_; }
    uint64_t reset_ms() const noexcept { return reset_ms_; }

    // Test/reset hook: forget the learned high-water mark (e.g. across a reconnect where the
    // address itself might have changed).
    void reset() noexcept;

private:
    int64_t remaining_{-1};
    int64_t high_water_{0};
    uint64_t reset_ms_{0};
};

}  // namespace pc::risk
