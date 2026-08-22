#include "md/trade_tape.hpp"

#include <algorithm>

namespace pc::md {

void TradeTape::apply(const pc_trade& trade) noexcept {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_signal_fence(std::memory_order_acq_rel);
    values_[next_] = trade;
    next_ = (next_ + 1) % kCapacity;
    if (size_ < kCapacity)
        ++size_;
    std::atomic_signal_fence(std::memory_order_acq_rel);
    generation_.fetch_add(1, std::memory_order_release);
}

size_t TradeTape::copy_recent(pc_trade* out, size_t out_cap) const noexcept {
    for (;;) {
        const auto g0 = generation_.load(std::memory_order_acquire);
        if (g0 & 1)
            continue;
        std::atomic_signal_fence(std::memory_order_acq_rel);
        const size_t n = std::min(size_, out_cap);
        for (size_t k = 0; k < n; ++k)
            out[k] = values_[(next_ + kCapacity - 1 - k) % kCapacity];
        std::atomic_signal_fence(std::memory_order_acq_rel);
        const auto g1 = generation_.load(std::memory_order_acquire);
        if (g1 == g0)
            return n;
    }
}

}  // namespace pc::md
