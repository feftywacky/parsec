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

void CandleSeries::clear() noexcept {
    if (size_ == 0)
        return;
    // Same odd/even protocol the other mutators use -- a UI-thread copy_recent() may be
    // mid-flight, and it must see either the full series or an empty one, never a stale
    // `first_` paired with a zeroed `size_`.
    generation_.fetch_add(1, std::memory_order_acq_rel);  // -> odd: write in progress
    std::atomic_signal_fence(std::memory_order_acq_rel);
    first_ = 0;
    size_ = 0;
    std::atomic_signal_fence(std::memory_order_acq_rel);
    generation_.fetch_add(1, std::memory_order_release);  // -> even: stable
}

void CandleSeries::backfill(const pc_candle* items, size_t count) noexcept {
    if (count == 0)
        return;

    // Full ascending merge of `items` into the cached series, newest-first into `scratch` so
    // the walk can stop as soon as kCapacity bars are picked. A prepend-only merge (all that
    // was needed for the cold-start case) cannot repair an INTERIOR hole, and holes are the
    // normal outcome of a laptop sleeping: the socket drops, the venue's candle stream resumes
    // at the present, apply() appends that live bar straight after the pre-sleep one, and every
    // bar covering the sleep sits at an open_ms *newer* than the oldest cached bar -- exactly
    // the range an older-only merge discards. The re-arm on reconnect (app::Engine's PC_EV_CONN
    // arm) already re-fetches the snapshot that spans the hole; merging it is what makes the
    // re-fetch mean anything.
    //
    // Cached bars win ties: a live bar (or the in-progress bucket apply() keeps rewriting) is
    // always at least as fresh as the same bar out of a REST snapshot that may have been in
    // flight for a while.
    //
    // 312 KB is far too large for the stack, and a per-series member would multiply it by every
    // (asset, interval) pair; backfill() is engine-thread only (see the header's cross-thread
    // contract), so one buffer per thread is enough.
    static thread_local std::array<pc_candle, kCapacity> scratch;

    size_t i = size_;  // cached bars [0, i) still to consider, ascending
    size_t j = count;  // REST bars   [0, j) still to consider, ascending
    size_t n = 0;      // bars picked so far, filled from the back of `scratch`
    while (n < kCapacity && (i > 0 || j > 0)) {
        const pc_candle* pick;
        if (i > 0 && j > 0) {
            const uint64_t cached_open = at(i - 1).open_ms;
            const uint64_t rest_open = items[j - 1].open_ms;
            if (cached_open >= rest_open) {
                if (cached_open == rest_open)
                    --j;  // same bucket from both sources -- drop the REST copy
                pick = &at(--i);
            } else {
                pick = &items[--j];
            }
        } else if (i > 0) {
            pick = &at(--i);
        } else {
            pick = &items[--j];
        }
        scratch[kCapacity - 1 - n] = *pick;
        ++n;
    }
    if (n == size_ && j == 0 && i == 0)
        return;  // the snapshot added nothing -- don't churn the generation counter

    generation_.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_signal_fence(std::memory_order_acq_rel);
    for (size_t k = 0; k < n; ++k)
        values_[k] = scratch[kCapacity - n + k];
    first_ = 0;
    size_ = n;
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
