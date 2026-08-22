#pragma once
#include <array>
#include <atomic>
#include <cstddef>

#include "parsec/parsec.h"
namespace pc::md {

// Fixed-size circular buffer of the most recent trades (docs/02 §6.1: "ring of the last 512
// prints per coin"). Same cross-thread contract and rationale as md::CandleSeries: apply()
// runs on the engine thread, copy_recent() on the UI thread, coordinated by a seqlock-style
// generation counter instead of a whole-struct copy through a SnapshotSlot. At 512 x ~32 B =
// 16 KB this technically fits under SnapshotSlot's 64 KiB cap, but a tape/chart panel only
// ever wants the newest handful of prints most frames, and using one cross-thread scheme for
// every md:: type that needs range reads (this and CandleSeries) is simpler to reason about
// than mixing two.
class TradeTape {
public:
    static constexpr size_t kCapacity = 512;

    // Engine thread only.
    void apply(const pc_trade& trade) noexcept;

    // Engine thread only, no cross-thread guarantee.
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] const pc_trade& newest(size_t offset = 0) const noexcept {
        return values_[(next_ + kCapacity - 1 - offset) % kCapacity];
    }

    // UI thread. Copies up to `out_cap` trades, newest-first, into `out`; retries internally
    // on a torn read. Returns the number of trades actually copied.
    [[nodiscard]] size_t copy_recent(pc_trade* out, size_t out_cap) const noexcept;

private:
    std::array<pc_trade, kCapacity> values_{};
    size_t next_{};
    size_t size_{};
    std::atomic<uint64_t> generation_{0};
};

}  // namespace pc::md
