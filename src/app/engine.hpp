#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "app/config.hpp"
#include "app/ui_bridge.hpp"
#include "exec/order_state.hpp"
#include "md/market_store.hpp"
#include "md/staleness.hpp"
#include "parsec/parsec.h"
#include "portfolio/account_state.hpp"
#include "portfolio/position_book.hpp"
#include "portfolio/reconciler.hpp"
#include "risk/dead_mans_switch.hpp"
#include "risk/kill_switch.hpp"
#include "risk/rate_budget.hpp"

namespace pc::app {

// Owns the Rust FFI engine handle and runs the single-threaded event-processing loop on a
// dedicated worker thread (see run()). All md::/exec::/portfolio:: state is only ever mutated
// from that thread and is never copied out of it -- md::MarketStore holds ~1.9 MB of candles
// and tape per subscribed asset and is deliberately move-only so it cannot accidentally be
// snapshotted wholesale (that mistake previously produced a 165 MB by-value copy per frame).
//
// The UI thread sees this class only through bridge(): small trivially-copyable snapshots
// through seqlock slots, discrete events through an SPSC ring. See app/ui_bridge.hpp, which
// is the contract.
class Engine {
public:
    // `cfg.mainnet` is the sole network selector -- there is deliberately no separate `bool
    // mainnet` parameter that could disagree with it (an earlier version of this constructor
    // had exactly that footgun).
    explicit Engine(Config cfg = Config::defaults());
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool start();
    void stop();

    // The engine/UI boundary. Safe to call from either thread; see UiBridge for which methods
    // belong to which side.
    [[nodiscard]] UiBridge& bridge() noexcept { return bridge_; }
    [[nodiscard]] const UiBridge& bridge() const noexcept { return bridge_; }

    // Dense asset index of the instrument the UI is currently focused on. The engine publishes
    // InstrumentSnapshot for this asset only, since that snapshot carries full book depth.
    // UI-thread read handle for the active asset's candle series and trade tape, which are too
    // large to cross the seqlock. Safe to call from the UI thread: MarketStore publishes each
    // slot pointer with release/acquire and never clears one (see md/market_store.hpp), and the
    // buffers themselves are read through their own generation-counter protocol.
    [[nodiscard]] const md::AssetMarket* ui_market(uint32_t asset) const noexcept {
        return markets_.find(asset);
    }

    // The resolved network. The status bar must show this at all times in a distinct colour
    // (docs/07 Phase 7 requirement 5), so it has to reach the UI rather than being assumed.
    [[nodiscard]] bool mainnet() const noexcept { return mainnet_; }

    [[nodiscard]] uint32_t active_asset() const noexcept {
        return active_asset_.load(std::memory_order_relaxed);
    }

private:
    void run();
    void resolve_active_asset() noexcept;
    void publish_asset_universe() noexcept;
    void select_asset(uint32_t asset, uint8_t interval) noexcept;
    void apply_event(const pc_event& event) noexcept;
    void publish(uint64_t now_ms) noexcept;
    void drain_ui_commands() noexcept;
    void subscribe_interval(uint32_t asset, uint8_t interval) noexcept;
    // Re-subscribes the two l2Book streams at a new price granularity (UiCommand::
    // SetBookAggregation). Unsubscribing must replay the granularity currently in force --
    // the venue matches an unsubscribe against the exact subscription payload -- which is why
    // it is tracked here rather than passed in.
    void set_book_aggregation(int8_t n_sig_figs, uint8_t mantissa) noexcept;

    // -- the safety net (docs/02 §4.2's tick_timers, docs/07 Phase 5) --
    // Runs every loop iteration (see run()); each sub-timer is internally cadence-gated so
    // calling this every ~0.5ms poll tick costs nothing beyond a few integer comparisons on
    // the iterations where nothing is actually due.
    void tick_timers(uint64_t now_ms) noexcept;
    void tick_dead_mans_switch(uint64_t now_ms) noexcept;
    void tick_reconciler(uint64_t now_ms) noexcept;
    // Runs the reconciler over every asset touched by the position snapshot batch that just
    // closed (a PC_F_SNAPSHOT_BEGIN..PC_F_SNAPSHOT_END run of PC_EV_POSITION events) --
    // called from apply_event on PC_F_SNAPSHOT_END, not on a timer, since the comparison is
    // only meaningful once a full snapshot has actually landed.
    void reconcile_positions() noexcept;
    // Cancels every resting order and, if `flatten`, sends a reduce-only IOC order against
    // every open position at an aggressive-but-rounded price (exec::Rounder). Only ever called
    // from drain_ui_commands's KillSwitchTrigger case.
    void flatten_all_positions() noexcept;

    pc_engine* ffi_{};
    std::atomic<bool> running_{false};
    std::thread thread_{};

    // --- engine-thread-only state, never touched by the UI thread ---
    md::MarketStore markets_{};
    md::StalenessConfig staleness_cfg_{};
    portfolio::PositionBook positions_{};
    pc_account account_{};
    bool account_valid_{};
    exec::OrderStateBook orders_{};
    std::vector<pc_event> poll_buffer_{};
    char active_coin_[PC_COIN_LEN]{};
    bool universe_published_{};
    // Bitset of PC_IV_* timeframes already subscribed for the active coin; see
    // subscribe_interval().
    uint64_t subscribed_intervals_{};

    // Last WebSocket ping/pong round trip on the market socket, microseconds. Fed by the
    // pong-carried PC_EV_CONN heartbeat, republished to the UI in every SafetySnapshot.
    uint32_t market_rtt_us_{};

    // Price granularity currently in force on the l2Book subscriptions. -1/0 == the venue's
    // native granularity, which is what a fresh subscription starts at.
    int8_t book_n_sig_figs_{-1};
    uint8_t book_mantissa_{};

    UiBridge bridge_{};
    std::atomic<uint32_t> active_asset_{PC_ASSET_NONE};

    // -- config + safety net, engine-thread-only except where noted --
    Config config_{};
    bool mainnet_{};

    risk::DeadMansSwitch dms_{};
    uint64_t dms_deadline_ms_{};  // 0 until the first heartbeat goes out
    bool dms_active_{};

    risk::KillSwitch kill_switch_{};
    risk::RateBudget rate_budget_{};

    portfolio::Reconciler reconciler_{};
    // Fill-nudged local projection, compared against `positions_` (which is fed directly by
    // the venue's own position pushes and is therefore already the authoritative side of the
    // comparison -- see docs/02 §6.4). Kept separate from `positions_` so a divergence has two
    // actual sides to diverge between, instead of comparing a value against itself.
    portfolio::PositionBook optimistic_positions_{};
    portfolio::AccountState account_state_{};
    std::vector<uint32_t> reconcile_batch_{};  // assets touched by the snapshot batch in flight
    uint64_t last_reconcile_fetch_ms_{};
    uint32_t reconcile_divergence_count_{};
    bool reconcile_alarm_{};
};

// Pure, allocation-free gate consulted by drain_ui_commands() before every PlaceOrder reaches
// pc_place_order (docs/02 §6.5, docs/07 Phase 5: "[kill switch] must actually block new
// orders"). Factored out as a free function, rather than inlined into the switch statement, so
// it is unit-testable without spinning up a full Engine (which would require the FFI/network
// layer) -- see tests/cpp/test_engine_timers.cpp.
[[nodiscard]] inline bool blocks_new_orders(const risk::KillSwitch& kill_switch,
                                            const risk::RateBudget& rate_budget) noexcept {
    return kill_switch.blocks_new_orders() || rate_budget.should_reject_new_orders();
}

}  // namespace pc::app
