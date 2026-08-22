#pragma once
// The engine -> UI boundary (docs/02 §4.3, docs/05 §5). This header is the CONTRACT between
// the engine thread (single writer) and the UI thread (single reader): the UI thread must
// NEVER call pc_* directly and must NEVER touch md::/exec::/portfolio:: state itself -- it
// only ever reads through the slots and rings defined here, and only ever writes user intent
// through UiCommandRing. Every type that actually crosses a boundary below is trivially
// copyable (enforced by static_assert) so it can live in a SnapshotSlot or an SpscRing with no
// allocation and no locks (docs/02 §1's hard constraints). UiBridge itself is not copied; it
// is constructed once and shared by reference between the two threads.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/seqlock.hpp"
#include "core/spsc_ring.hpp"
#include "md/asset_ctx.hpp"
#include "md/market_store.hpp"
#include "parsec/parsec.h"
#include "portfolio/position_book.hpp"

namespace pc::app {

// ============================================================================================
// Engine -> UI: latest-only snapshots (seqlock-style triple buffer, docs/05 §5.1)
// ============================================================================================

// `md::InstrumentSnapshot` (src/md/market_store.hpp) IS the per-instrument snapshot type --
// defined in md/ rather than duplicated here, since it's built directly off md::MarketStore's
// internals (md::MarketStore::snapshot()). Aliased here so callers only need to include
// app/ui_bridge.hpp to find the whole cross-thread contract in one place.
using InstrumentSnapshot = md::InstrumentSnapshot;
using InstrumentSnapshotSlot = SnapshotSlot<InstrumentSnapshot>;

// One selectable asset from the venue's `meta` universe. The UI receives this snapshot instead
// of calling pc_asset_info() directly, so coin selection stays inside the app without making the
// rendering thread depend on the FFI.
struct AssetOption {
    uint32_t asset{PC_ASSET_NONE};
    char name[PC_COIN_LEN]{};
    uint8_t sz_decimals{};
};
static_assert(std::is_trivially_copyable_v<AssetOption>);

// The complete selectable perp universe. This is metadata only; market data is subscribed lazily
// for the selected asset, so publishing all names does not create 512 live feeds.
struct AssetUniverseSnapshot {
    static constexpr size_t kMaxAssets = 512;
    uint32_t count{};
    std::array<AssetOption, kMaxAssets> assets{};
};
static_assert(std::is_trivially_copyable_v<AssetUniverseSnapshot>);
using AssetUniverseSnapshotSlot = SnapshotSlot<AssetUniverseSnapshot>;

// Account-wide state, published as one unit so the UI never renders a torn mix of positions
// and account totals (mirrors PC_F_SNAPSHOT_BEGIN/END bracketing at the FFI layer, 02 §5.4).
struct PortfolioSnapshot {
    // Real accounts hold at most a few dozen concurrent positions; capped well below
    // portfolio::PositionBook::kMaxAssets (512, indexed by the full asset universe) so this
    // snapshot stays small. If every slot is ever legitimately full, positions beyond the cap
    // are simply not shown in this snapshot -- portfolio::PositionBook itself is unaffected.
    static constexpr size_t kMaxPositions = 64;
    pc_account account{};
    bool account_valid{};
    uint32_t position_count{};
    std::array<portfolio::Position, kMaxPositions> positions{};
};
static_assert(std::is_trivially_copyable_v<PortfolioSnapshot>);
using PortfolioSnapshotSlot = SnapshotSlot<PortfolioSnapshot>;

// The risk/safety-net state (src/risk/) that the status bar needs to render instead of the
// "not wired" placeholders it used to show (docs/07 Phase 5: "surface it in the status bar as
// a real indicator, not a hidden log line"). Published once per Engine::tick_timers() call --
// see app/engine.cpp -- so it is at most one ~0.5ms poll tick stale, not one UI frame stale.
struct SafetySnapshot {
    // Dead man's switch (risk::DeadMansSwitch). `dms_deadline_ms` is the absolute unix-ms
    // deadline currently armed with the venue via pc_schedule_cancel; the status bar computes
    // its own countdown against that using its own now_ms rather than the engine publishing a
    // pre-computed remaining duration that would go stale between publishes.
    bool dms_active{};  // false until the first heartbeat has actually gone out (see
                        // Engine::tick_dead_mans_switch: gated on an authenticated session)
    uint64_t dms_deadline_ms{};
    uint32_t dms_triggers_remaining_today{10};  // risk::DeadMansSwitch::kMaxTriggersPerUtcDay

    // Market-socket WebSocket ping/pong round trip, microseconds; 0 until first measured.
    // The ONLY true latency figure in the UI -- the per-feed numbers in the header strip are
    // push cadences, which describe how often the venue sends rather than how long a packet
    // takes to arrive.
    uint32_t market_rtt_us{};

    // Rate budget (risk::RateBudget), fed by PC_EV_RATE.
    int32_t rate_budget_bps{10'000};
    int64_t rate_budget_remaining{-1};

    // Kill switch (risk::KillSwitch).
    bool kill_switch_armed{};

    // Reconciler (portfolio::Reconciler), accumulated for the session.
    uint32_t reconcile_divergence_count{};
    bool reconcile_alarm{};  // sticky once true; cleared only by an explicit ack (none wired
                             // yet -- see this pass's report)
};
static_assert(std::is_trivially_copyable_v<SafetySnapshot>);
using SafetySnapshotSlot = SnapshotSlot<SafetySnapshot>;

// ============================================================================================
// Engine -> UI: discrete events, every one matters (docs/05 §5.2: fills, acks, toasts)
// ============================================================================================

enum class UiEventKind : uint8_t { Fill, OrderAck, OrderUpdate, Toast, ConnState, Rate };

struct UiToast {
    char text[128]{};
    uint8_t severity{};  // 0 info, 1 warn, 2 error
};

struct UiEvent {
    UiEventKind kind{};
    // Dense asset index the event belongs to, copied from pc_event::asset. The payload structs
    // (pc_fill, pc_order_update, pc_order_ack) do not carry one, so without this a fills or
    // open-orders row cannot say which instrument it is about.
    uint32_t asset{PC_ASSET_NONE};
    uint64_t recv_time_ns{};
    union {
        pc_fill fill;
        pc_order_ack ack;
        pc_order_update order_update;
        pc_conn conn;
        pc_rate rate;
        UiToast toast;
    } u{};
};
static_assert(std::is_trivially_copyable_v<UiEvent>);

inline constexpr size_t kUiEventRingCapacity = 4096;
using UiEventRing = SpscRing<UiEvent, kUiEventRingCapacity>;

// ============================================================================================
// UI -> Engine: low-rate user intents (docs/05 §5.2, opposite direction)
// ============================================================================================

// Deliberately not `pc_order_req` directly: keeping a thin UI-side wrapper means a future
// "requested from panel X" / "confirmed via double-click" metadata field can be added without
// touching the frozen FFI struct in include/parsec/parsec.h.
enum class UiCommandKind : uint8_t {
    PlaceOrder,
    CancelOrder,
    CancelByCloid,
    CancelAll,
    SetLeverage,
    SetMarginMode,
    SetInterval,  // switch the chart timeframe: subscribe + REST-backfill that interval
    SetActiveAsset,
    // Re-subscribe the l2Book streams at a coarser price granularity so the ladder is complete
    // at the selected step. Display-only in intent, but it IS a subscription change (the venue
    // aggregates server-side), so it goes through the engine like any other -- panels never
    // touch subscriptions directly.
    SetBookAggregation,
    // Manual global stop (risk::KillSwitch, docs/07 Phase 5): cancel-all always; flatten only
    // if `flatten` is set, and only ever behind a UI confirmation dialog -- "confirm-on-flatten,
    // no confirm on cancel-all" is a UI-side rule, not enforced here (this ring has no notion
    // of "the user already confirmed"; the panel that pushes this command owns that gate).
    KillSwitchTrigger,
    KillSwitchRearm,  // explicit, distinct from trigger so re-arming is never a side effect
};

struct UiCommand {
    UiCommandKind kind{};
    uint32_t asset{PC_ASSET_NONE};
    uint64_t oid{};
    uint8_t cloid[16]{};
    uint32_t leverage{};   // SetLeverage
    bool is_cross{};       // SetLeverage / SetMarginMode
    uint8_t interval{};    // SetInterval, PC_IV_*
    int8_t n_sig_figs{-1};  // SetBookAggregation: < 0 means the venue's native granularity
    uint8_t mantissa{};     // SetBookAggregation: 0 means "send null"
    bool flatten{};        // KillSwitchTrigger
    pc_order_req order{};  // only populated for PlaceOrder
};
static_assert(std::is_trivially_copyable_v<UiCommand>);

inline constexpr size_t kUiCommandRingCapacity = 256;  // user-driven, inherently low rate
using UiCommandRing = SpscRing<UiCommand, kUiCommandRingCapacity>;

// ============================================================================================
// The bridge
// ============================================================================================

// Owns every cross-thread structure the UI touches. The engine thread is the sole writer of
// the publish_*() methods and the sole reader of try_pop_command(); the UI thread is the exact
// mirror image. Neither side locks; see core/seqlock.hpp and core/spsc_ring.hpp for the
// underlying guarantees.
//
// Sizing note: at kUiEventRingCapacity=4096 x sizeof(UiEvent) (dominated by pc_order_ack's
// 192-byte error string) this object is roughly 1 MB -- construct it with `std::make_unique`
// like app::Engine does for itself (see src/main.cpp's comment), not on the stack.
//
// NOT trivially copyable (it owns atomics inside SnapshotSlot/SpscRing) and not meant to be:
// construct once, share by reference between the engine and UI threads.
class UiBridge {
public:
    // -- engine thread --
    void publish_instrument(const InstrumentSnapshot& snapshot) noexcept {
        instrument_.store(snapshot);
    }
    void publish_universe(const AssetUniverseSnapshot& snapshot) noexcept {
        universe_.store(snapshot);
    }
    void publish_portfolio(const PortfolioSnapshot& snapshot) noexcept {
        portfolio_.store(snapshot);
    }
    void publish_safety(const SafetySnapshot& snapshot) noexcept { safety_.store(snapshot); }

    // Returns false (and counts the drop) if the ring is full. A slow or stalled UI thread
    // must never be able to block the engine loop, so this never retries or waits.
    bool push_event(const UiEvent& event) noexcept {
        if (events_.try_push(event))
            return true;
        dropped_events_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    [[nodiscard]] uint64_t dropped_events() const noexcept {
        return dropped_events_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool try_pop_command(UiCommand& out) noexcept { return commands_.try_pop(out); }

    // -- UI thread --
    void load_instrument(InstrumentSnapshot& out) const noexcept { instrument_.load(out); }
    void load_universe(AssetUniverseSnapshot& out) const noexcept { universe_.load(out); }
    void load_portfolio(PortfolioSnapshot& out) const noexcept { portfolio_.load(out); }
    void load_safety(SafetySnapshot& out) const noexcept { safety_.load(out); }
    [[nodiscard]] bool try_pop_event(UiEvent& out) noexcept { return events_.try_pop(out); }
    [[nodiscard]] bool push_command(const UiCommand& cmd) noexcept {
        return commands_.try_push(cmd);
    }

private:
    InstrumentSnapshotSlot instrument_{};
    AssetUniverseSnapshotSlot universe_{};
    PortfolioSnapshotSlot portfolio_{};
    SafetySnapshotSlot safety_{};
    UiEventRing events_{};
    UiCommandRing commands_{};
    std::atomic<uint64_t> dropped_events_{0};
};

// Helper builders (implemented in ui_bridge.cpp) for the engine thread. Not required to use
// this header -- the engine can fill the snapshot/event structs by hand -- but they save every
// engine-side caller from re-deriving the same "scan positions", "hash a toast string" logic.
UiEvent make_toast(const char* text, uint8_t severity, uint64_t recv_time_ns) noexcept;
PortfolioSnapshot make_portfolio_snapshot(const pc_account& account, bool account_valid,
                                          const portfolio::PositionBook& positions) noexcept;
}  // namespace pc::app
