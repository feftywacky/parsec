#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/units.hpp"
#include "parsec/parsec.h"
namespace pc::md {

// Fixed-capacity ring of one (coin, interval) candle series, oldest at logical index 0. Lives
// inside the engine-owned per-asset store (md::MarketStore) and is deliberately NOT part of
// any SnapshotSlot: at up to kCapacity x 64 B =~ 312 KB, a single series already dwarfs
// SnapshotSlot's 64 KiB structural cap (core/seqlock.hpp), and copying it wholesale every UI
// frame would be wasted work anyway -- a chart only ever draws its visible window, not 5000
// candles (docs/02 §7's LOD note).
//
// Cross-thread contract: apply()/backfill() run only on the engine thread; copy_recent() runs
// only on the UI thread. Both sides coordinate through `generation_`, the exact odd/even
// seqlock protocol docs/05-ui.md §5.1 describes for L2Book -- applied here to a *range copy*
// of a large append-mostly buffer instead of a whole-struct assignment, since the object is
// too large to double/triple-buffer wholesale. Same tradeoff the doc accepts for the raw
// seqlock: the plain reads of `values_`/`first_`/`size_` inside the generation window are
// non-atomic and race the writer, which is formally UB under the C++ memory model but correct
// on every mainstream architecture (no observed value is ever a mix of old/new bytes for a
// POD `pc_candle`, and a torn logical read -- e.g. `size_` updated but `values_` not yet -- is
// caught by the generation recheck and retried).
class CandleSeries {
public:
    static constexpr size_t kCapacity = 5000;  // matches the venue's candleSnapshot REST cap

    // Engine thread only. Overwrites the in-progress bucket while `open_ms` matches the last
    // stored candle, appends (evicting the oldest once full) when it advances.
    void apply(const pc_candle& candle) noexcept;

    // Engine thread only. Merges REST `candleSnapshot` history (docs/03 §`candleSnapshot`,
    // max 5000 candles) into the cached series, keeping the newest kCapacity bars. `items`
    // must be ascending by `open_ms`, matching the REST response order. The merge is a full
    // one, not a prepend: it fills holes anywhere in the series, including the interior hole a
    // laptop sleeping leaves behind (socket drops, the stream resumes at the present, and the
    // bars covering the sleep are newer than the oldest cached bar). Where both sources hold
    // the same bucket the cached bar wins, so a live tick is never clobbered by stale REST
    // data, and switching to a timeframe that has already been streaming live never loses data
    // to a backfill response that lands late (docs/07 Phase 2: "timeframe switching must not
    // discard cached data").
    void backfill(const pc_candle* items, size_t count) noexcept;

    // Engine thread only. Folds one trade print into the bucket it belongs to, opening a new
    // bucket when the trade crosses an interval boundary. This is how the sub-minute
    // timeframes exist at all: the venue serves no candles below 1m (`candleSnapshot` rejects
    // "1s".."30s" outright), so PC_IV_1S..PC_IV_30S are built here from the `trades` stream.
    //
    // The consequence is deliberate and visible in the UI: a locally folded series has NO
    // history. It starts at the first print after the subscription opens, and a gap with no
    // trades produces no candle at all rather than a flat one -- there is no source of truth
    // for a bar that never printed. `interval_ms` is the bucket width; `now_ms` is the venue
    // block clock for the print, never local time.
    void fold_trade(Px px, Qty sz, uint64_t time_ms, uint64_t interval_ms,
                    uint8_t interval_id) noexcept;

    // Engine thread only. Drops every cached candle, returning the series to its cold-start
    // state. Called when an asset's streams are unsubscribed (a coin switch): what is left
    // cached at that moment describes a window the series will no longer have continuous
    // coverage of, and -- unlike a reconnect gap, which backfill() now stitches shut -- it
    // belongs to a different coin entirely, so merging the next snapshot into it would splice
    // two instruments' bars into one series.
    void clear() noexcept;

    // Engine thread only, no cross-thread guarantee -- for engine-side logic (e.g. deciding
    // whether a timeframe needs a backfill fetch at all).
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] const pc_candle& at(size_t index) const noexcept {
        return values_[(first_ + index) % kCapacity];
    }

    // UI thread. Copies up to `out_cap` of the most recent candles, oldest-first, into `out`.
    // Retries internally on a torn read (see class comment); returns the number of candles
    // actually copied, which may be less than `out_cap` if the series doesn't hold that many
    // yet.
    [[nodiscard]] size_t copy_recent(pc_candle* out, size_t out_cap) const noexcept;

private:
    std::array<pc_candle, kCapacity> values_{};
    size_t first_{};
    size_t size_{};
    // Odd while apply()/backfill() is mutating shared state, even otherwise.
    std::atomic<uint64_t> generation_{0};
};

}  // namespace pc::md
