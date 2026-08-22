#include "risk/rate_budget.hpp"

namespace pc::risk {

void RateBudget::update(int64_t remaining, uint64_t reset_ms) noexcept {
    remaining_ = remaining;
    reset_ms_ = reset_ms;
    if (remaining > high_water_)
        high_water_ = remaining;
}

int32_t RateBudget::remaining_bps() const noexcept {
    if (high_water_ <= 0)
        return 10'000;
    if (remaining_ <= 0)
        return 0;
    const int64_t bps = remaining_ * 10'000 / high_water_;
    return bps > 10'000 ? 10'000 : static_cast<int32_t>(bps);
}

void RateBudget::reset() noexcept {
    remaining_ = -1;
    high_water_ = 0;
    reset_ms_ = 0;
}

}  // namespace pc::risk
