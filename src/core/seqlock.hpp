#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
namespace pc {

// Engine -> UI "latest snapshot" slot (docs/05-ui.md §5.1). The reference design in that doc
// is a raw seqlock (odd/even generation counter guarding a single buffer): lock-free, but
// `value_ = desired` is a non-atomic write racing a non-atomic read, which the doc itself
// flags as formally UB even though it is correct on every real architecture, and offers a
// triple buffer with an atomic index swap as the alternative.
//
// This is that alternative, chosen deliberately: a small, fixed pool of buffers plus a
// per-buffer reader refcount. store() always writes into a buffer no reader currently holds,
// so the writer never touches memory a reader is reading -- no torn reads, no UB, at the cost
// of one extra buffer of memory and a refcount bump/drop on every load(). Either design is
// defensible per the doc; this one was picked because `T` here (market/portfolio snapshots)
// is copied every UI frame at 60 fps, and "provably no data race" was judged worth more than
// the few extra bytes for something read that often.
//
// Structural size cap: this slot exists to carry small "latest state" snapshots (docs/05
// §5.1's "latest order-book snapshot", not an entire multi-asset store). A regression once
// put a ~165 MB `MarketStore` behind this template, which made `SnapshotSlot<MarketStore>`
// ~495 MB (three buffers) and blew the default thread stack the moment something tried to
// return one by value. The assert below makes that class of mistake fail to compile instead
// of segfaulting at startup. 64 KiB is generous headroom over the ~1.5 KB instrument
// snapshots and ~5 KB portfolio snapshots this is actually used for (src/app/ui_bridge.hpp).
template <typename T>
class SnapshotSlot {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(sizeof(T) <= 64 * 1024,
                  "SnapshotSlot is for small latest-state snapshots (docs/05 §5.1), not a "
                  "whole engine-side store -- see the class comment. If T legitimately needs "
                  "to be this large, it almost certainly needs a different cross-thread "
                  "scheme (generation-counter range copy, e.g. md::CandleSeries::copy_recent), "
                  "not a bigger buffer here.");

public:
    // Publishes `value` as the new latest snapshot. Always finds a buffer index with zero
    // active readers before writing to it -- see the class comment below for why this cannot
    // livelock against a single reader thread (the documented usage pattern: one UI thread
    // calling load()/load(T&) at its own pace).
    void store(const T& value) noexcept {
        const auto current = published_.load(std::memory_order_acquire);
        unsigned next = current;
        while (next == current || readers_[next].load(std::memory_order_acquire) != 0) {
            next = (next + 1) % buffers_.size();
        }
        buffers_[next] = value;
        published_.store(next, std::memory_order_release);
    }

    // Fills `out` with the latest published value. Preferred over the by-value overload below
    // on a hot path (once per frame) since it makes the copy destination explicit instead of
    // relying on the caller/compiler to elide a return -- the exact habit that let the old
    // `MarketStore`-sized misuse of this template return hundreds of megabytes on the stack.
    void load(T& out) const noexcept {
        for (;;) {
            const auto index = published_.load(std::memory_order_acquire);
            readers_[index].fetch_add(1, std::memory_order_acquire);
            if (published_.load(std::memory_order_acquire) == index) {
                out = buffers_[index];
                readers_[index].fetch_sub(1, std::memory_order_release);
                return;
            }
            readers_[index].fetch_sub(1, std::memory_order_release);
        }
    }

    // Convenience wrapper for call sites that want a value, not an out-param. Safe now that
    // `T` is capped at 64 KiB by the static_assert above.
    [[nodiscard]] T load() const noexcept {
        T out;
        load(out);
        return out;
    }

private:
    // Livelock argument for store()'s search loop, spelled out because it is load-bearing and
    // not obvious from the code alone:
    //
    // Precondition: exactly one reader thread calls load()/load(T&), and it does so
    // sequentially (docs/02 §4.3 -- the UI thread never touches this concurrently with
    // itself). So at any instant, at most one buffer index can have a nonzero reader count:
    // the index that thread's in-flight load() most recently observed via `published_`.
    //
    // store()'s loop excludes at most two of the three indices: `current` (so publishing
    // always advances to a different buffer) and, if it exists, the one buffer the single
    // reader might currently hold. With three buffers and at most two exclusions, a free
    // index is guaranteed to exist and the loop terminates in at most two iterations -- it is
    // not an unbounded spin, it is a bounded search over a 3-element ring. This breaks down
    // only if a second concurrent reader is introduced (readers_ would then need its own
    // capacity to match), which is why buffers_.size() and the single-reader precondition
    // must change together.
    static constexpr std::size_t kFalseSharingRange = 128;  // Apple Silicon cache line
    alignas(kFalseSharingRange) std::array<T, 3> buffers_{};
    alignas(kFalseSharingRange) std::atomic<unsigned> published_{0};
    mutable std::array<std::atomic<unsigned>, 3> readers_{};
};
}  // namespace pc
