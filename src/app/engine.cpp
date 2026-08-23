#include "app/engine.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "core/log.hpp"
#include "core/time.hpp"
#include "exec/rounder.hpp"
#include "portfolio/pnl.hpp"

namespace pc::app {
namespace {

// pc_event is ~1.2 KB, so a 128-event batch is ~149 KB -- too much to sit on a worker thread's
// default stack. The buffer lives on the heap as an Engine member instead (see run()).
constexpr size_t kPollBatch = 128;

// pc_poll's blocking timeout. Short enough that the loop reacts well inside a 16 ms frame,
// long enough that an idle engine thread parks instead of spinning (docs/02 §4.2).
constexpr uint64_t kPollTimeoutNs = 500'000;  // 0.5 ms

// How often tick_reconciler() re-pulls clearinghouseState/openOrders over REST to drive the
// comparison (docs/02 §6.4, docs/07 Phase 5: "~4s cadence"). This is a floor, not the only
// source of position snapshots -- the venue's own WS push may also arrive on its own schedule;
// either way, every PC_EV_POSITION snapshot batch gets reconciled (see reconcile_positions()),
// this just guarantees one happens at least this often even if the WS side goes quiet.
constexpr uint64_t kReconcileIntervalMs = 4'000;
constexpr uint64_t kFeeRatesRefreshIntervalMs = 60'000;
// activeAssetData is pulled from three places (asset switch, reconciler tick, and every
// account snapshot). The reconciler's clearinghouseState fetch comes back as a PC_EV_ACCOUNT
// a few ms later, so without a floor each 4 s tick issued the same /info POST twice. Matching
// the reconciler cadence keeps the account-event path from adding anything the tick will not
// already have refreshed.
constexpr uint64_t kAssetDataRefreshIntervalMs = kReconcileIntervalMs;

// Divergence tolerance for the position reconciler, in Qty (1e8-scaled) units. Dust-level
// drift between an optimistic fill-projection and the next venue snapshot (e.g. from a
// partial fill landing right at snapshot boundary) is expected and not worth alarming on;
// anything larger is a real divergence. Deliberately not asset-aware (would need szDecimals
// per asset, which the engine does not cache today) -- 0.001 units is well below a dust
// threshold for every instrument in the current testnet universe.
constexpr Qty kReconcileSizeTolerance = kScale / 1000;

// The active instrument subscribes all three order-book feeds at once. They are not
// alternatives: measured on mainnet BTC, `bbo` pushes every ~86ms but carries one level,
// `l2Book fast:true` every ~530ms with five, and the default `l2Book` every ~5.4s with twenty.
// Subscribing only the default one (which is what this did previously, after the fast feed was
// dropped for being too shallow to fill the ladder) left the displayed touch up to five seconds
// stale. md::merge_display_book composes them into a single ladder that is both deep and live.
constexpr uint32_t kMarketStreams = PC_STREAM_BBO | PC_STREAM_L2 | PC_STREAM_L2_FAST |
                                    PC_STREAM_TRADES | PC_STREAM_ASSET_CTX;
constexpr uint8_t kDefaultInterval = PC_IV_1M;
constexpr const char* kDefaultCoin = "BTC";

}  // namespace

Engine::Engine(Config cfg) : config_(std::move(cfg)), mainnet_(config_.mainnet) {}

Engine::~Engine() {
    stop();
}

bool Engine::start() {
    if (running_.exchange(true))
        return false;

    active_asset_.store(PC_ASSET_NONE, std::memory_order_relaxed);
    universe_published_ = false;
    subscribed_intervals_ = 0;
    dms_deadline_ms_ = 0;
    dms_active_ = false;
    dms_unavailable_ = false;
    dms_req_id_ = 0;

    pc_config cfg{};
    cfg.abi_version = PC_ABI_VERSION;
    cfg.mainnet = mainnet_;
    cfg.event_queue_capacity = 8192;
    cfg.io_worker_threads = 2;
    // No passphrase here by design: Config never carries the keystore passphrase (it is a
    // secret, config.json is not). The engine therefore always starts unauthenticated --
    // read-only market data, no account/order events -- and the passphrase arrives later
    // through Engine::unlock()/pc_unlock from the UI's unlock dialog. That ordering is not
    // just a convenience: Argon2id at the SENSITIVE tier costs ~3.5s (docs/06 §3), so doing
    // it during construction would stall startup before a window ever appeared.
    //
    // Only pass the path down if a keystore actually exists there: config_.keystore_path always
    // has a value (defaults() sets it), even on a fresh install with no keystore, and handing
    // Rust a path with nothing behind it just produces a "cannot read keystore" log line on
    // every single startup for the common no-keystore-yet case.
    if (std::filesystem::exists(config_.keystore_path))
        std::snprintf(cfg.keystore_path, sizeof(cfg.keystore_path), "%s",
                      config_.keystore_path.c_str());

    ffi_ = pc_engine_create(&cfg);
    if (!ffi_) {
        running_ = false;
        return false;
    }

    // The asset universe has to land before any event can be attributed to a dense asset index.
    // The initial BTC selection is made by the engine after meta arrives; subscribing before
    // that point would race the registry and produce asset-less events.
    std::snprintf(active_coin_, sizeof(active_coin_), "%s", kDefaultCoin);
    pc_fetch(ffi_, PC_FETCH_META, nullptr);

    thread_ = std::thread(&Engine::run, this);
    return true;
}

void Engine::stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
    pc_engine_destroy(ffi_);
    ffi_ = nullptr;
}

// Resolves the built-in initial coin to its dense asset index once `meta` has landed. Subsequent
// coin changes come from the UI's asset selector through UiCommand::SetActiveAsset.
void Engine::resolve_active_asset() noexcept {
    const int32_t count = pc_asset_count(ffi_);
    if (count <= 0)
        return;

    if (!universe_published_)
        publish_asset_universe();

    if (active_asset_.load(std::memory_order_relaxed) != PC_ASSET_NONE)
        return;

    uint32_t first_asset = PC_ASSET_NONE;
    uint32_t default_asset = PC_ASSET_NONE;
    for (int32_t i = 0; i < count; ++i) {
        char name[PC_COIN_LEN]{};
        uint8_t sz_decimals{};
        uint32_t max_leverage{};
        uint8_t only_isolated{};
        if (pc_asset_info(ffi_, static_cast<uint32_t>(i), name, sizeof(name), &sz_decimals,
                          &max_leverage, &only_isolated) < 0)
            continue;
        if (first_asset == PC_ASSET_NONE)
            first_asset = static_cast<uint32_t>(i);
        if (std::strncmp(name, kDefaultCoin, PC_COIN_LEN) == 0)
            default_asset = static_cast<uint32_t>(i);
    }

    const uint32_t initial = default_asset != PC_ASSET_NONE ? default_asset : first_asset;
    if (initial != PC_ASSET_NONE)
        select_asset(initial, kDefaultInterval);
}

void Engine::publish_asset_universe() noexcept {
    const int32_t count = pc_asset_count(ffi_);
    if (count <= 0)
        return;

    app::AssetUniverseSnapshot snapshot{};
    for (int32_t i = 0; i < count && snapshot.count < snapshot.kMaxAssets; ++i) {
        char name[PC_COIN_LEN]{};
        uint8_t sz_decimals{};
        uint32_t max_leverage{};
        uint8_t only_isolated{};
        if (pc_asset_info(ffi_, static_cast<uint32_t>(i), name, sizeof(name), &sz_decimals,
                          &max_leverage, &only_isolated) < 0)
            continue;
        auto& option = snapshot.assets[snapshot.count++];
        option.asset = static_cast<uint32_t>(i);
        option.sz_decimals = sz_decimals;
        option.max_leverage = max_leverage;
        option.only_isolated = only_isolated;
        std::snprintf(option.name, sizeof(option.name), "%s", name);
    }
    bridge_.publish_universe(snapshot);
    universe_published_ = snapshot.count != 0;
}

void Engine::select_asset(uint32_t asset, uint8_t interval) noexcept {
    if (asset == PC_ASSET_NONE || interval >= PC_IV_COUNT)
        return;

    char name[PC_COIN_LEN]{};
    uint8_t sz_decimals{};
    uint32_t max_leverage{};
    uint8_t only_isolated{};
    if (pc_asset_info(ffi_, asset, name, sizeof(name), &sz_decimals, &max_leverage,
                      &only_isolated) < 0 || name[0] == '\0')
        return;

    const uint32_t previous = active_asset_.load(std::memory_order_relaxed);
    if (previous != asset) {
        if (previous != PC_ASSET_NONE) {
            pc_unsubscribe_book(ffi_, active_coin_, kMarketStreams, book_n_sig_figs_,
                                book_mantissa_);
            for (uint8_t iv = PC_IV_FIRST_VENUE; iv < PC_IV_COUNT; ++iv) {
                if (subscribed_intervals_ & (1ull << iv))
                    pc_unsubscribe(ffi_, active_coin_, PC_STREAM_CANDLE, iv);
            }
        }
        std::snprintf(active_coin_, sizeof(active_coin_), "%s", name);
        subscribed_intervals_ = 0;
        active_asset_.store(asset, std::memory_order_relaxed);
        asset_data_asset_ = PC_ASSET_NONE;
        asset_data_valid_ = false;
        bridge_.publish_instrument(InstrumentSnapshot{});
        // Carries the granularity currently selected in the book panel, so switching coins
        // does not silently reset the ladder to the venue's native step.
        pc_subscribe_book(ffi_, active_coin_, kMarketStreams, book_n_sig_figs_, book_mantissa_);
    }

    // Unthrottled on purpose: a just-selected coin has no activeAssetData at all, so this
    // must not wait on the refresh floor. Stamping it keeps the next account snapshot from
    // immediately duplicating the request.
    if (account_valid_ && active_coin_[0] != '\0') {
        pc_fetch(ffi_, PC_FETCH_ACTIVE_ASSET_DATA, active_coin_);
        last_asset_data_fetch_ms_ = unix_ms();
    }
    subscribe_interval(asset, interval);
}

void Engine::apply_event(const pc_event& event) noexcept {
    // Market data is keyed by asset and handled wholesale by the store.
    markets_.apply(event);

    switch (event.kind) {
        case PC_EV_POSITION:
            // Positions arrive bracketed by PC_F_SNAPSHOT_BEGIN/END so the portfolio swaps
            // atomically rather than merging partial updates (docs/02 §5.4). Every such batch
            // is also a fresh authoritative venue snapshot, so it doubles as reconciler input
            // (docs/02 §6.4) -- reconcile_batch_ tracks which assets this batch touched, and
            // reconcile_positions() runs the comparison once the batch closes.
            if (event.flags & PC_F_SNAPSHOT_BEGIN) {
                positions_.clear();
                reconcile_batch_.clear();
            }
            positions_.apply(event.asset, event.u.position);
            if (event.asset < portfolio::PositionBook::kMaxAssets)
                reconcile_batch_.push_back(event.asset);
            if (event.flags & PC_F_SNAPSHOT_END)
                reconcile_positions();
            break;

        case PC_EV_ACCOUNT:
            account_ = event.u.account;
            account_valid_ = true;
            // Re-bases both AccountState's authoritative and optimistic views (docs/02 §6.4).
            account_state_.apply_authoritative(event.u.account);
            if (active_coin_[0] != '\0' &&
                unix_ms() - last_asset_data_fetch_ms_ >= kAssetDataRefreshIntervalMs) {
                pc_fetch(ffi_, PC_FETCH_ACTIVE_ASSET_DATA, active_coin_);
                last_asset_data_fetch_ms_ = unix_ms();
            }
            if (unix_ms() - last_fee_rates_fetch_ms_ >= kFeeRatesRefreshIntervalMs) {
                pc_fetch(ffi_, PC_FETCH_USER_FEES, nullptr);
                last_fee_rates_fetch_ms_ = unix_ms();
            }
            break;

        case PC_EV_ASSET_DATA:
            asset_data_ = event.u.asset_data;
            asset_data_asset_ = event.asset;
            asset_data_valid_ = event.asset != PC_ASSET_NONE;
            break;

        case PC_EV_FEE_RATES:
            fee_rates_ = event.u.fee_rates;
            fee_rates_valid_ = true;
            break;

        case PC_EV_ORDER_ACK: {
            // Correlate on req_id -- pc_order_ack carries no cloid, and matching "whichever
            // order is still PendingNew" attaches acks to the wrong order whenever two are in
            // flight at once (docs/02 §6.2).
            orders_.apply_ack(event.req_id, event.u.ack);
            UiEvent ui{};
            ui.kind = UiEventKind::OrderAck;
            ui.asset = event.asset;
            ui.recv_time_ns = event.recv_time_ns;
            ui.u.ack = event.u.ack;
            bridge_.push_event(ui);
            break;
        }

        case PC_EV_ORDER_UPDATE: {
            orders_.apply_update(event.u.order_update);
            UiEvent ui{};
            ui.kind = UiEventKind::OrderUpdate;
            ui.asset = event.asset;
            ui.recv_time_ns = event.recv_time_ns;
            ui.u.order_update = event.u.order_update;
            bridge_.push_event(ui);
            break;
        }

        case PC_EV_FILL: {
            orders_.apply_fill(event.u.fill);

            // Nudge the optimistic position projection so the reconciler has a "what we think
            // we hold, updated the instant the fill lands" value to compare against the next
            // venue snapshot (docs/02 §6.4) -- without this every fill would look identical to
            // a divergence at the next reconcile pass. Falls back to the current venue-known
            // position if this is the first fill seen for the asset (no prior optimistic value
            // to nudge from).
            if (event.asset < portfolio::PositionBook::kMaxAssets) {
                const portfolio::Position* opt = optimistic_positions_.find(event.asset);
                const portfolio::Position* venue = positions_.find(event.asset);
                pc_position p = opt ? opt->value : (venue ? venue->value : pc_position{});
                p.szi += event.u.fill.is_buy ? event.u.fill.qty : -event.u.fill.qty;
                optimistic_positions_.apply(event.asset, p);
            }
            account_state_.apply_fill_delta(portfolio::realized_pnl_from_fill(event.u.fill),
                                            event.u.fill.fee);

            UiEvent ui{};
            ui.kind = UiEventKind::Fill;
            ui.asset = event.asset;
            ui.recv_time_ns = event.recv_time_ns;
            ui.u.fill = event.u.fill;
            bridge_.push_event(ui);
            break;
        }

        case PC_EV_RATE: {
            rate_budget_.update(event.u.rate.remaining, event.u.rate.reset_ms);

            UiEvent ui{};
            ui.kind = UiEventKind::Rate;
            ui.asset = event.asset;
            ui.recv_time_ns = event.recv_time_ns;
            ui.u.rate = event.u.rate;
            bridge_.push_event(ui);
            break;
        }

        case PC_EV_CONN: {
            // A pong-carried heartbeat only updates the latency readout. Pushing it as a UI
            // event would put a "connected" line in the event log every 5 seconds and bury
            // the state transitions the log exists to record.
            if (event.u.conn.rtt_us != 0) {
                if (event.u.conn.socket == PC_SOCK_MARKET)
                    market_rtt_us_ = event.u.conn.rtt_us;
                else
                    user_rtt_us_ = event.u.conn.rtt_us;
                if (event.u.conn.state == PC_CONN_CONNECTED)
                    break;
            }
            UiEvent ui{};
            ui.kind = UiEventKind::ConnState;
            ui.asset = event.asset;
            ui.recv_time_ns = event.recv_time_ns;
            ui.u.conn = event.u.conn;
            bridge_.push_event(ui);
            break;
        }

        case PC_EV_ERROR:
            if (event.u.error.code < 0) {
                if (event.req_id != 0 && event.req_id == dms_req_id_ &&
                    std::strstr(event.u.error.msg, "enough volume traded") != nullptr) {
                    // Hyperliquid permanently rejects scheduleCancel for low-volume accounts.
                    // Stop the automatic heartbeat until the next process/session start rather
                    // than spending another signed request and repeating the same warning.
                    dms_unavailable_ = true;
                    dms_active_ = false;
                    dms_deadline_ms_ = 0;
                }
                PC_LOG_WARN("parsec: %s", event.u.error.msg);
                bridge_.push_event(make_toast(event.u.error.msg, 2, event.recv_time_ns));
            }
            break;

        default:
            break;
    }
}

// Translates UI intents into FFI commands. This is the only place the engine calls a pc_*
// mutating command, and the only place an order enters the state book -- keeping the two
// together is what makes the req_id correlation above sound.
void Engine::drain_ui_commands() noexcept {
    UiCommand cmd{};
    while (bridge_.try_pop_command(cmd)) {
        // How long this command sat in the ring. Measured before it is acted on, so an order's
        // own submission cost is not folded into the handoff figure the status bar reports.
        if (cmd.enqueue_mono_ns != 0) {
            const uint64_t now_ns = monotonic_ns();
            if (now_ns > cmd.enqueue_mono_ns)
                engine_cmd_us_ = ewma_us(engine_cmd_us_, (now_ns - cmd.enqueue_mono_ns) / 1000);
        }
        switch (cmd.kind) {
            case UiCommandKind::PlaceOrder: {
                // The safety-net gate (docs/02 §6.5, docs/07 Phase 5): an armed kill switch or
                // an exhausted rate budget refuses the order here, before it ever reaches
                // pc_place_order. See app::blocks_new_orders() -- factored out as a free
                // function specifically so this gate is unit-testable without an Engine.
                if (blocks_new_orders(kill_switch_, rate_budget_)) {
                    bridge_.push_event(
                        make_toast(kill_switch_.blocks_new_orders()
                                       ? "order blocked: kill switch is armed"
                                       : "order blocked: address rate budget exhausted",
                                   2, monotonic_ns()));
                    break;
                }
                const pc_req_id id = pc_place_order(ffi_, &cmd.order);
                if (id == 0) {
                    bridge_.push_event(
                        make_toast("order rejected before submission", 2, monotonic_ns()));
                    break;
                }
                const uint64_t deadline = unix_ms() + config_.order_ack_timeout_ms;
                for (uint8_t i = 0; i < cmd.order.n_orders && i < 4; ++i)
                    orders_.create(id, cmd.order.orders[i], deadline);
                break;
            }
            case UiCommandKind::CancelOrder:
                pc_cancel_order(ffi_, cmd.asset, cmd.oid);
                break;
            case UiCommandKind::CancelByCloid:
                pc_cancel_by_cloid(ffi_, cmd.asset, cmd.cloid);
                break;
            case UiCommandKind::CancelAll:
                pc_cancel_all(ffi_);
                break;
            case UiCommandKind::SetLeverage:
                pc_set_leverage(ffi_, cmd.asset, cmd.is_cross, cmd.leverage);
                break;
            case UiCommandKind::SetMarginMode:
                // The venue models margin mode as part of updateLeverage, so re-send the
                // current leverage with the new isCross flag rather than inventing an action.
                pc_set_leverage(ffi_, cmd.asset, cmd.is_cross, cmd.leverage);
                break;
            case UiCommandKind::SetInterval:
                subscribe_interval(cmd.asset, cmd.interval);
                break;
            case UiCommandKind::SetBookAggregation:
                set_book_aggregation(cmd.n_sig_figs, cmd.mantissa);
                break;
            case UiCommandKind::SetActiveAsset:
                select_asset(cmd.asset,
                             cmd.interval < PC_IV_COUNT ? cmd.interval : kDefaultInterval);
                break;
            case UiCommandKind::KillSwitchTrigger: {
                const risk::KillSwitch::Action action = kill_switch_.trigger(cmd.flatten);
                if (action.cancel_all)
                    pc_cancel_all(ffi_);
                if (action.flatten)
                    flatten_all_positions();
                bridge_.push_event(
                    make_toast(action.flatten ? "kill switch: cancelling all orders and flattening"
                                              : "kill switch: cancelling all orders",
                               2, monotonic_ns()));
                break;
            }
            case UiCommandKind::KillSwitchRearm:
                kill_switch_.rearm();
                bridge_.push_event(
                    make_toast("kill switch re-armed: orders allowed again", 0, monotonic_ns()));
                break;
        }
    }
}

// Sends a reduce-only IOC order against every currently-held position, opposite side, full
// size -- the "flatten" half of the kill switch (docs/07 Phase 5). Iterates the whole
// PositionBook index space rather than a live asset list because PositionBook (owned by
// portfolio/, not this file) exposes no enumeration API beyond find()-by-index; at
// kMaxAssets=512 flat scalar comparisons this is negligible next to the network round trip
// each resulting order takes, and flatten is an emergency path, not a hot one.
void Engine::flatten_all_positions() noexcept {
    for (uint32_t asset = 0; asset < portfolio::PositionBook::kMaxAssets; ++asset) {
        const portfolio::Position* pos = positions_.find(asset);
        if (!pos || pos->value.szi == 0)
            continue;

        const md::AssetMarket* market = markets_.find(asset);
        const Px mark = market ? market->ctx.mark_px() : 0;
        if (mark <= 0) {
            // No price to flatten against -- skip rather than guess. Silent skip is defensible
            // here: cancel-all already ran, so this asset's resting orders are gone regardless;
            // a stale/unknown mark just means the flatten leg is deferred, not lost.
            PC_LOG_WARN("kill switch: skipping flatten for asset %u, no mark price", asset);
            continue;
        }

        char name[PC_COIN_LEN]{};
        uint8_t sz_decimals{};
        uint32_t max_leverage{};
        uint8_t only_isolated{};
        pc_asset_info(ffi_, asset, name, sizeof(name), &sz_decimals, &max_leverage, &only_isolated);
        const exec::AssetPrecision precision{sz_decimals};

        const bool is_buy = pos->value.szi < 0;  // buy to close a short, sell to close a long
        const Side side = is_buy ? Side::Buy : Side::Sell;

        pc_order_req req{};
        req.grouping = PC_GROUP_NA;
        req.n_orders = 1;
        pc_order& o = req.orders[0];
        o.asset = asset;
        o.is_buy = is_buy;
        o.reduce_only = 1;
        o.tif = PC_TIF_IOC;
        o.sz = pos->value.szi < 0 ? -pos->value.szi : pos->value.szi;
        o.limit_px = exec::Rounder::marketable_px(
            mark, side, static_cast<uint32_t>(config_.default_slippage_bps), precision);

        const pc_req_id id = pc_place_order(ffi_, &req);
        if (id != 0)
            orders_.create(id, o, unix_ms() + config_.order_ack_timeout_ms);
    }
}

// Subscribes a chart timeframe and backfills it over REST. Timeframes are subscribed lazily on
// first open and never unsubscribed, so switching back to one already seen costs nothing and
// keeps its cached CandleSeries (docs/07 Phase 2).
void Engine::subscribe_interval(uint32_t asset, uint8_t interval) noexcept {
    if (asset == PC_ASSET_NONE || asset != active_asset_.load(std::memory_order_relaxed) ||
        interval >= PC_IV_COUNT)
        return;
    // Sub-minute timeframes have no venue feed to subscribe or backfill -- `candleSnapshot`
    // rejects "1s".."30s" -- so they are already being folded from the `trades` stream this
    // asset is subscribed to (md::MarketStore::apply). Issuing a `candle` subscribe for one
    // would silently ask the venue for 1m bars and file them under the wrong interval id.
    if (interval < PC_IV_FIRST_VENUE)
        return;
    const uint64_t bit = 1ull << interval;
    if (subscribed_intervals_ & bit)
        return;
    subscribed_intervals_ |= bit;
    pc_subscribe(ffi_, active_coin_, PC_STREAM_CANDLE, interval);
    pc_fetch_candle_snapshot(ffi_, active_coin_, interval);
}

// Only the two l2Book streams are cycled -- bbo, trades and activeAssetCtx have no granularity
// parameter, and dropping them here would blank the header strip and the tape for a round trip
// every time the ladder's step changed.
void Engine::set_book_aggregation(int8_t n_sig_figs, uint8_t mantissa) noexcept {
    if (n_sig_figs == book_n_sig_figs_ && mantissa == book_mantissa_)
        return;
    constexpr uint32_t kBookStreams = PC_STREAM_L2 | PC_STREAM_L2_FAST;
    if (active_coin_[0] != '\0')
        pc_unsubscribe_book(ffi_, active_coin_, kBookStreams, book_n_sig_figs_, book_mantissa_);
    book_n_sig_figs_ = n_sig_figs;
    book_mantissa_ = mantissa;
    if (active_coin_[0] != '\0')
        pc_subscribe_book(ffi_, active_coin_, kBookStreams, book_n_sig_figs_, book_mantissa_);
}

void Engine::publish(uint64_t now_ms) noexcept {
    const uint32_t asset = active_asset_.load(std::memory_order_relaxed);
    InstrumentSnapshot instrument{};
    if (asset != PC_ASSET_NONE)
        markets_.snapshot(asset, now_ms, staleness_cfg_, instrument);
    bridge_.publish_instrument(instrument);
    auto portfolio = make_portfolio_snapshot(account_, account_valid_, positions_);
    if (asset_data_valid_ && asset_data_asset_ == asset) {
        portfolio.asset_data = asset_data_;
        portfolio.asset_data_asset = asset_data_asset_;
        portfolio.asset_data_valid = true;
    }
    portfolio.fee_rates = fee_rates_;
    portfolio.fee_rates_valid = fee_rates_valid_;
    bridge_.publish_portfolio(portfolio);
}

// docs/02 §4.2's tick_timers step: dead man's switch, reconciliation cadence, order timeouts
// (the last of those stays in run() via orders_.expire(), which predates this method and is
// already correct). Called every loop iteration -- each sub-timer below is itself cadence
// gated, so an idle call here costs a handful of integer comparisons, not real work.
void Engine::tick_timers(uint64_t now_ms) noexcept {
    tick_dead_mans_switch(now_ms);
    tick_reconciler(now_ms);

    // Published every call (not gated behind `n > 0` like publish()) so the status bar's kill
    // switch / rate budget readout reacts within one poll tick of a UI command, not one tick
    // per incoming network event. The snapshot itself is a few dozen bytes -- cheap to store
    // every ~0.5ms regardless.
    SafetySnapshot safety{};
    safety.dms_active = dms_active_;
    safety.dms_unavailable = dms_unavailable_;
    safety.dms_deadline_ms = dms_deadline_ms_;
    safety.dms_triggers_remaining_today = dms_.triggers_remaining_today(now_ms);
    safety.market_rtt_us = market_rtt_us_;
    safety.user_rtt_us = user_rtt_us_;
    safety.engine_tick_us = engine_tick_us_;
    safety.engine_tick_max_us = engine_tick_max_us_;
    safety.engine_batch_events = engine_batch_events_;
    safety.engine_cmd_us = engine_cmd_us_;
    safety.publish_mono_ns = monotonic_ns();
    // Per-window worst case: the UI reads "the slowest tick since you last looked", not an
    // all-time high that never decays once one scheduler hiccup has set it.
    engine_tick_max_us_ = 0;
    safety.rate_budget_bps = rate_budget_.remaining_bps();
    safety.rate_budget_remaining = rate_budget_.remaining();
    safety.kill_switch_armed = kill_switch_.blocks_new_orders();
    safety.reconcile_divergence_count = reconcile_divergence_count_;
    safety.reconcile_alarm = reconcile_alarm_;
    bridge_.publish_safety(safety);
}

// scheduleCancel heartbeat (risk::DeadMansSwitch, docs/07 Phase 5). Only runs once a keystore
// is unlocked and orders are actually possible -- `account_valid_` (set the moment the first
// PC_EV_ACCOUNT lands, which only happens for an authenticated session) is the proxy for that;
// the engine has no separate "keystore unlocked" flag today since unlocking happens once, at
// pc_engine_create, not as a later FFI call (see the comment in start()). A read-only session
// with no keystore therefore never sends scheduleCancel, which is correct -- there is nothing
// resting on the venue for it to protect.
void Engine::tick_dead_mans_switch(uint64_t now_ms) noexcept {
    if (!account_valid_ || dms_unavailable_)
        return;
    if (!dms_.needs_refresh(now_ms))
        return;
    const uint64_t deadline = dms_.refresh(now_ms);
    dms_req_id_ = pc_schedule_cancel(ffi_, deadline);
    dms_deadline_ms_ = deadline;
    dms_active_ = true;
}

// Drives the ~4s clearinghouseState/openOrders comparison (docs/02 §6.4, docs/07 Phase 5).
// pc_fetch is async -- this only issues the REST pull; the actual comparison happens in
// reconcile_positions() once the resulting PC_EV_POSITION snapshot batch closes with
// PC_F_SNAPSHOT_END (see apply_event). Also re-pulls open orders so exec::OrderStateBook's
// view of resting orders does not go stale between fills/acks, though (see this pass's report)
// there is no reconciler-equivalent divergence check on the order side yet -- Reconciler's API
// is position-shaped (pc_position in, pc_position out) and adding an order-book analogue would
// mean designing new portfolio/exec surface, which is out of this file's ownership.
void Engine::tick_reconciler(uint64_t now_ms) noexcept {
    if (!account_valid_)
        return;
    if (now_ms - last_reconcile_fetch_ms_ < kReconcileIntervalMs)
        return;
    last_reconcile_fetch_ms_ = now_ms;
    pc_fetch(ffi_, PC_FETCH_CLEARINGHOUSE_STATE, nullptr);
    pc_fetch(ffi_, PC_FETCH_OPEN_ORDERS, nullptr);
    if (active_coin_[0] != '\0') {
        pc_fetch(ffi_, PC_FETCH_ACTIVE_ASSET_DATA, active_coin_);
        last_asset_data_fetch_ms_ = now_ms;
    }
    if (now_ms - last_fee_rates_fetch_ms_ >= kFeeRatesRefreshIntervalMs) {
        pc_fetch(ffi_, PC_FETCH_USER_FEES, nullptr);
        last_fee_rates_fetch_ms_ = now_ms;
    }
}

// Compares the fill-nudged optimistic projection against the venue snapshot that just landed
// in `positions_`, for every asset the just-closed batch touched. docs/02 §6.4: divergence
// beyond tolerance snaps local state to venue truth, logs both values, and raises a visible
// alarm on the second consecutive occurrence for that asset.
void Engine::reconcile_positions() noexcept {
    for (const uint32_t asset : reconcile_batch_) {
        const portfolio::Position* venue = positions_.find(asset);
        if (!venue)
            continue;
        const portfolio::Position* opt = optimistic_positions_.find(asset);
        // No prior optimistic value (first snapshot ever seen for this asset, or the very
        // first fill hasn't landed yet) -- start it at venue truth rather than at a
        // default-constructed zero position, which would otherwise report a false divergence
        // for every newly-observed asset with a non-zero venue position.
        pc_position local = opt ? opt->value : venue->value;

        const auto log = reconciler_.reconcile(asset, local, venue->value, kReconcileSizeTolerance);
        optimistic_positions_.apply(asset, local);

        if (log) {
            PC_LOG_WARN("RECONCILE_DIVERGENCE asset=%u local_szi=%lld venue_szi=%lld diff=%lld%s",
                        asset, static_cast<long long>(log->local_szi),
                        static_cast<long long>(log->venue_szi),
                        static_cast<long long>(log->size_diff), log->alarm ? " ALARM" : "");
            ++reconcile_divergence_count_;
            if (log->alarm) {
                reconcile_alarm_ = true;
                bridge_.push_event(make_toast(
                    "reconciliation alarm: position diverged twice in a row, snapped to venue", 2,
                    monotonic_ns()));
            }
        }
    }
}

// Blocking-with-timeout drain loop: sleeps when idle, wakes within microseconds of an event
// (docs/02 §4.2). Market/portfolio snapshots are republished only when something actually
// changed (`n > 0`); tick_timers() runs every iteration regardless, since the safety
// net's own cadences (10s DMS heartbeat, 4s reconcile fetch) must not depend on market activity.
void Engine::run() {
    poll_buffer_.assign(kPollBatch, pc_event{});

    while (running_.load(std::memory_order_relaxed)) {
        const int32_t n = pc_poll(ffi_, poll_buffer_.data(),
                                  static_cast<uint32_t>(poll_buffer_.size()), kPollTimeoutNs);

        // Timed from *after* pc_poll returns: the poll wait is a blocking sleep on an idle
        // socket, not work, and including it would report a quiet market as the slowest loop.
        const uint64_t work_start_ns = monotonic_ns();
        for (int32_t i = 0; i < n; ++i)
            apply_event(poll_buffer_[static_cast<size_t>(i)]);

        const uint64_t now = unix_ms();
        resolve_active_asset();
        drain_ui_commands();
        // tick_timers publishes the SafetySnapshot, so it reads the previous iteration's
        // figures -- one loop of lag on a number that is itself an EWMA, which is fine and
        // avoids either measuring the publish inside itself or publishing twice per loop.
        tick_timers(now);
        orders_.expire(now);
        if (n > 0)
            publish(now);

        const uint64_t work_end_ns = monotonic_ns();
        const uint64_t work_us =
            work_end_ns > work_start_ns ? (work_end_ns - work_start_ns) / 1000 : 0;
        engine_tick_us_ = ewma_us(engine_tick_us_, work_us);
        if (work_us > engine_tick_max_us_)
            engine_tick_max_us_ = work_us > 0xFFFF'FFFFULL ? 0xFFFF'FFFFU
                                                           : static_cast<uint32_t>(work_us);
        if (n > 0)
            engine_batch_events_ = static_cast<uint32_t>(n);
    }
}

}  // namespace pc::app
