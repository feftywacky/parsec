#pragma once
#include <atomic>
#include <cstdint>
#include <string>
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

    // -- account connection (docs/06 §2). UI-thread safe: all three go straight to the
    // FFI, which serializes internally, and none of them touch engine-thread state.

    // PC_AUTH_* -- NO_KEYSTORE (connect an account), LOCKED, UNLOCKING, UNLOCKED, FAILED,
    // EXPIRED (a keystore exists but its agent approval lapsed; no passphrase opens it).
    [[nodiscard]] int auth_status() const noexcept {
        return ffi_ != nullptr ? pc_auth_status(ffi_) : PC_AUTH_NO_KEYSTORE;
    }

    // Starts an unlock attempt and returns immediately -- Argon2id costs ~3.5s (docs/06 §3),
    // far too long to block a frame on, so the caller polls auth_status(). `passphrase` is a
    // WRITABLE buffer and is zeroed by the callee before it returns.
    bool unlock(char* passphrase) noexcept {
        return ffi_ != nullptr && pc_unlock(ffi_, passphrase) == 0;
    }

    // Tell the engine about a keystore written after startup (the in-app connect flow).
    bool set_keystore_path(const std::string& path) noexcept {
        return ffi_ != nullptr && pc_set_keystore_path(ffi_, path.c_str()) == 0;
    }

    // Deletes every local keystore and returns the engine to its fresh-install state, so the
    // next frame reports PC_AUTH_NO_KEYSTORE and the connect flow opens. Returns the number
    // of files removed, or -1 if refused (already unlocked) or a delete failed.
    //
    // Local only: the agent approval still stands at the venue until it lapses on its own.
    // parsec cannot revoke it, since revoking needs the master key it never stores.
    int reset_accounts() noexcept {
        return ffi_ != nullptr ? pc_reset_accounts(ffi_) : -1;
    }

    // Last FFI-side error string, for surfacing why a reset or unlock was refused.
    [[nodiscard]] std::string last_error() const noexcept {
        char buffer[256]{};
        if (ffi_ == nullptr || pc_last_error(ffi_, buffer, sizeof(buffer)) < 0)
            return {};
        return buffer;
    }

    // Path of the keystore this engine would unlock. Non-empty even when the file does not
    // exist yet, which is what lets the UI tell the user where setup will write it.
    [[nodiscard]] const std::string& keystore_path() const noexcept {
        return config_.keystore_path;
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
    // The account snapshot itself lives in `account_state_` below -- this only records that
    // one has arrived, which is what gates order entry and the account-scoped fetches.
    bool account_valid_{};
    pc_spot spot_{};
    bool spot_valid_{};
    uint64_t last_spot_fetch_ms_{};
    pc_asset_data asset_data_{};
    uint32_t asset_data_asset_{PC_ASSET_NONE};
    bool asset_data_valid_{};
    pc_fee_rates fee_rates_{};
    bool fee_rates_valid_{};
    exec::OrderStateBook orders_{};
    std::vector<pc_event> poll_buffer_{};
    char active_coin_[PC_COIN_LEN]{};
    bool universe_published_{};
    // Bitset of PC_IV_* timeframes already subscribed for the active coin; see
    // subscribe_interval().
    uint64_t subscribed_intervals_{};
    // The chart timeframe the UI is currently showing. Kept so a market-socket reconnect can
    // re-subscribe and re-backfill it -- subscribed_intervals_ alone only records what was
    // *asked for*, and an ask issued before the socket came up buys nothing.
    uint8_t active_interval_{};
    // Whether the market socket has been seen connected. A subscribe issued while it is down
    // is dropped by the venue transport, and subscribed_intervals_ would still latch it as
    // done -- which is how a cold start could leave the chart permanently empty.
    bool market_connected_{};

    // Last WebSocket ping/pong round trip per socket, microseconds. Fed by the pong-carried
    // PC_EV_CONN heartbeat, republished to the UI in every SafetySnapshot.
    uint32_t market_rtt_us_{};
    uint32_t user_rtt_us_{};

    // Local (this-process) latency accounting, republished in every SafetySnapshot. See
    // SafetySnapshot's own comments for what each figure means. EWMAs are kept in whole
    // microseconds with a 1/8 smoothing factor -- integer-only, since this runs on the hot
    // loop and a float here would buy nothing but a rounding question.
    uint32_t engine_tick_us_{};
    uint32_t engine_tick_max_us_{};  // reset on each publish, so it is a per-window worst case
    uint32_t engine_batch_events_{};
    uint32_t engine_cmd_us_{};
    // Set by drain_ui_commands() when it pops a command; folded into engine_cmd_us_ there.
    static constexpr uint32_t kLatencyEwmaShift = 3;  // alpha = 1/8
    static uint32_t ewma_us(uint32_t previous, uint64_t sample_us) noexcept {
        const int64_t sample = static_cast<int64_t>(
            sample_us > 0x7FFF'FFFFULL ? 0x7FFF'FFFFULL : sample_us);
        if (previous == 0)
            return static_cast<uint32_t>(sample);
        // Arithmetic shift on a signed difference, so the EWMA converges downward as well as
        // upward -- an unsigned >> on a negative delta would latch the maximum forever.
        const int64_t next =
            static_cast<int64_t>(previous) +
            ((sample - static_cast<int64_t>(previous)) >> kLatencyEwmaShift);
        return next < 0 ? 0U : static_cast<uint32_t>(next);
    }

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
    bool dms_unavailable_{};  // venue-level eligibility rejection; do not retry every heartbeat
    pc_req_id dms_req_id_{};  // correlates the venue's eligibility error to the heartbeat action

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
    uint64_t last_fee_rates_fetch_ms_{};
    // One-shot history backfill (fills, funding payments, terminal orders). Issued once the
    // first account snapshot proves the session is authenticated -- before that the venue has
    // no user to answer for. The three history tables are session logs fed by live streams;
    // this is what gives them anything to show from before the app was opened.
    bool history_backfilled_{};
    uint64_t last_asset_data_fetch_ms_{};
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
