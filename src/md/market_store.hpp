#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <type_traits>

#include "md/asset_ctx.hpp"
#include "md/bbo.hpp"
#include "md/book_merge.hpp"
#include "md/candle_series.hpp"
#include "md/l2_book.hpp"
#include "md/staleness.hpp"
#include "md/trade_tape.hpp"
#include "parsec/parsec.h"

namespace pc::md {

inline constexpr uint8_t kIntervalCount = PC_IV_COUNT;

// Bucket width per PC_IV_* id. The sub-minute entries have no venue equivalent; they are the
// widths md::CandleSeries::fold_trade() aggregates the trades stream into.
inline constexpr uint64_t kIntervalMs[kIntervalCount] = {
    1'000,             // PC_IV_1S   -- folded locally
    5'000,             // PC_IV_5S   -- folded locally
    10'000,            // PC_IV_10S  -- folded locally
    30'000,            // PC_IV_30S  -- folded locally
    60'000,            // PC_IV_1M
    5 * 60'000,        // PC_IV_5M
    15 * 60'000,       // PC_IV_15M
    30 * 60'000,       // PC_IV_30M
    60 * 60'000,       // PC_IV_1H
    4 * 60 * 60'000,   // PC_IV_4H
    24 * 60 * 60'000,  // PC_IV_1D
};

// Everything the engine tracks for one asset. Engine-thread-only: it owns md::CandleSeries and
// md::TradeTape, which are read cross-thread through their own generation-counter range-copy
// protocol (see their headers), not by whole-struct copy -- so AssetMarket itself is never
// copied and never crosses a SnapshotSlot. `InstrumentSnapshot` below is the small,
// trivially-copyable subset that actually does.
struct AssetMarket {
    L2Book book{};      // `l2Book` default subscription: 20 levels, ~5.4s cadence
    L2Book fast_book{}; // `l2Book fast:true`: 5 levels, ~530ms cadence (see md/book_merge.hpp)
    Bbo bbo{};
    TradeTape trades{};
    std::array<CandleSeries, kIntervalCount> candles{};
    AssetCtx ctx{};
    AssetStaleness staleness{};
};

// The trivially-copyable, ~1.5 KB per-instrument snapshot that actually crosses the engine/UI
// seqlock (docs/05 §5.1's "latest order-book snapshot", singular -- this is that snapshot).
// Candle series and the trade tape are excluded deliberately: kCapacity x kIntervalCount
// candles is ~1.9 MB per asset, far past SnapshotSlot's 64 KiB structural cap
// (core/seqlock.hpp), and copying it every frame would be wasted work when a chart only ever
// draws its visible window (docs/02 §7). Panels read those directly off the engine-owned
// AssetMarket via CandleSeries::copy_recent() / TradeTape::copy_recent() instead.
struct InstrumentSnapshot {
    uint32_t asset{PC_ASSET_NONE};
    L2Book book{};
    Bbo bbo{};
    AssetCtx ctx{};
    AssetStalenessState staleness{};

    [[nodiscard]] bool valid() const noexcept { return asset != PC_ASSET_NONE; }
};
static_assert(std::is_trivially_copyable_v<InstrumentSnapshot>);

// Engine-thread-only, single-owner store of every subscribed asset's market data, keyed by the
// venue's dense asset index (docs/02 §6.1). NEVER copied: per docs/02 §4.2 all md:: state is
// single-threaded and mutated only from the engine thread, and at ~330 KB (one interval's
// candles) to ~3.5 MB (all kIntervalCount intervals) per fully-populated asset, copying this
// store -- let alone triple-buffering it -- was a real regression caught during review (a
// naive `SnapshotSlot<MarketStore>` sized to ~495 MB and segfaulted on first use). Per-asset
// storage is allocated lazily, one `std::unique_ptr<AssetMarket>` on first event for that
// asset, rather than eagerly for all `kMaxAssets` index slots: subscribe-time allocation is
// not the market-data hot path (docs/02 §1's no-allocation rule is a steady-state constraint,
// not a subscribe-time one), and a realistic session watches a handful of coins out of
// testnet's measured 210-asset universe, not all 512 index slots.
class MarketStore {
public:
    static constexpr size_t kMaxAssets = 512;

    void apply(const pc_event& event) noexcept;

    // Null for an asset that has never received an event, for PC_ASSET_NONE (0xFFFFFFFF, far
    // out of range), and for any other out-of-range index -- callers never need a separate
    // bounds check before calling find().
    [[nodiscard]] const AssetMarket* find(uint32_t asset) const noexcept;
    [[nodiscard]] AssetMarket* find_mut(uint32_t asset) noexcept;

    // Fills `out` with the small cross-thread snapshot for `asset` (see InstrumentSnapshot).
    // Returns false and resets `out` to its default (asset == PC_ASSET_NONE) if the asset is
    // unknown. `now_ms` is the caller's wall clock, used for the staleness signals that are
    // evaluated at read time rather than stored (docs/03 §W6).
    bool snapshot(uint32_t asset, uint64_t now_ms, const StalenessConfig& cfg,
                  InstrumentSnapshot& out) const noexcept;

    // Signal 3 (docs/03 §W6): feed the reply of an `exchangeStatus` request here. See
    // md::L1ClockTracker for why this lives account-wide rather than per-asset.
    void note_l1_clock(uint64_t l1_time_ms, uint64_t local_now_ms) noexcept {
        l1_clock_.note(l1_time_ms, local_now_ms);
    }

private:
    AssetMarket& get_or_create(uint32_t asset) noexcept;

    // Two parallel arrays, deliberately. `owners_` is the actual ownership and is touched only
    // by the engine thread; `slots_` is the cross-thread-readable view.
    //
    // The UI thread reads candles and the trade tape straight out of an AssetMarket (via
    // CandleSeries::copy_recent() / TradeTape::copy_recent(), which carry their own
    // generation-counter protocol) rather than through a snapshot, because those buffers are
    // far too large to copy per frame. That means the UI thread calls find() concurrently with
    // the engine thread lazily allocating new assets, so the *pointer* itself has to be
    // published atomically: a plain pointer write here would be a data race, and could expose a
    // half-constructed AssetMarket to a reader. Release/acquire on `slots_` gives the reader a
    // fully-constructed object or nothing at all.
    //
    // A slot, once published, is never cleared or reallocated for the lifetime of the store,
    // so a pointer the UI has already loaded stays valid.
    // REST `candleSnapshot` history arrives as a run of PC_EV_CANDLE events bracketed by
    // PC_F_SNAPSHOT_BEGIN/END, oldest first. Those must NOT go through CandleSeries::apply(),
    // which only ever appends at the head: the live `candle` subscription usually lands its
    // first (current) candle before the REST reply arrives, so appending the historical run
    // after it leaves the series out of ascending order -- and every consumer, including the
    // chart's binary-search viewport clip, assumes ascending. They are buffered here instead
    // and handed to CandleSeries::backfill(), which prepends them ahead of what is already
    // cached. Engine-thread-only, like the rest of this store; one batch is ever in flight.
    std::vector<pc_candle> candle_snapshot_{};
    uint8_t candle_snapshot_interval_{0xFF};
    uint32_t candle_snapshot_asset_{PC_ASSET_NONE};

    std::array<std::unique_ptr<AssetMarket>, kMaxAssets> owners_{};
    std::array<std::atomic<AssetMarket*>, kMaxAssets> slots_{};
    L1ClockTracker l1_clock_{};
};

}  // namespace pc::md
