#include "md/candle_series.hpp"

#include <algorithm>

namespace pc::md {

void CandleSeries::apply(const pc_candle& candle) noexcept {
    generation_.fetch_add(1, std::memory_order_acq_rel);  // -> odd: write in progress
    std::atomic_signal_fence(std::memory_order_acq_rel);
    if (size_ && at(size_ - 1).open_ms == candle.open_ms) {
        values_[(first_ + size_ - 1) % kCapacity] = candle;
    } else if (size_ < kCapacity) {
        values_[(first_ + size_++) % kCapacity] = candle;
    } else {
        values_[first_] = candle;
        first_ = (first_ + 1) % kCapacity;
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
    generation_.fetch_add(1, std::memory_order_release);  // -> even: stable
}

void CandleSeries::backfill(const pc_candle* items, size_t count) noexcept {
    if (count == 0)
        return;
    const uint64_t horizon = size_ ? at(0).open_ms : UINT64_MAX;
    // items[0..i) are strictly older than what's already cached (ascending, per REST order).
    size_t i = count;
    while (i > 0 && items[i - 1].open_ms >= horizon)
        --i;
    if (i == 0)
        return;
    generation_.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_signal_fence(std::memory_order_acq_rel);
    // Prepend from the newest of the old range down to the oldest, so the final order stays
    // ascending: items[i-1] lands immediately before `horizon`, items[0] ends up at the front.
    for (size_t j = i; j > 0; --j) {
        if (size_ >= kCapacity)
            break;  // already holding kCapacity recent candles; older history isn't kept
        first_ = (first_ + kCapacity - 1) % kCapacity;
        values_[first_] = items[j - 1];
        ++size_;
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
    generation_.fetch_add(1, std::memory_order_release);
}

void CandleSeries::fold_trade(Px px, Qty sz, uint64_t time_ms, uint64_t interval_ms,
                              uint8_t interval_id) noexcept {
    if (interval_ms == 0)
        return;
    const uint64_t open_ms = time_ms - (time_ms % interval_ms);
    // A print older than the newest bar (out-of-order tape, or a trade that lands while a
    // backfill is still being stitched in) must not append: the series is kept strictly
    // ascending, and appending behind the head would corrupt that ordering for every reader.
    // Dropping it is right rather than merely safe -- the bar it belongs to is already closed.
    if (size_ && open_ms < at(size_ - 1).open_ms)
        return;

    generation_.fetch_add(1, std::memory_order_acq_rel);  // -> odd: write in progress
    std::atomic_signal_fence(std::memory_order_acq_rel);
    if (size_ && at(size_ - 1).open_ms == open_ms) {
        pc_candle& bar = values_[(first_ + size_ - 1) % kCapacity];
        bar.c = px;
        if (px > bar.h)
            bar.h = px;
        if (px < bar.l)
            bar.l = px;
        bar.v += sz;
        ++bar.n;
    } else {
        // A trade past the current bucket opens a new one. Buckets with no prints are simply
        // absent rather than carried forward flat -- see the header comment.
        pc_candle bar{};
        bar.open_ms = open_ms;
        bar.close_ms = open_ms + interval_ms - 1;
        bar.o = px;
        bar.h = px;
        bar.l = px;
        bar.c = px;
        bar.v = sz;
        bar.n = 1;
        bar.interval = interval_id;
        if (size_ < kCapacity) {
            values_[(first_ + size_++) % kCapacity] = bar;
        } else {
            values_[first_] = bar;
            first_ = (first_ + 1) % kCapacity;
        }
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
    generation_.fetch_add(1, std::memory_order_release);  // -> even: stable
}

size_t CandleSeries::copy_recent(pc_candle* out, size_t out_cap) const noexcept {
    for (;;) {
        const auto g0 = generation_.load(std::memory_order_acquire);
        if (g0 & 1)
            continue;  // write in progress, spin
        std::atomic_signal_fence(std::memory_order_acq_rel);
        const size_t n = std::min(size_, out_cap);
        const size_t start = size_ - n;
        for (size_t k = 0; k < n; ++k)
            out[k] = values_[(first_ + start + k) % kCapacity];
        std::atomic_signal_fence(std::memory_order_acq_rel);
        const auto g1 = generation_.load(std::memory_order_acquire);
        if (g1 == g0)
            return n;
        // torn read (writer ran concurrently) -- retry.
    }
}

}  // namespace pc::md
