#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/units.hpp"
#include "parsec/parsec.h"

namespace pc::md {

// Bits describing which of docs/03-hyperliquid-api.md §W6's four (of six) client-observable
// staleness signals fired -- exposed as a bitmask, not a bool, so the status bar can show
// *why* a feed is flagged and the ticket can decide whether the specific failure mode matters
// for the action it's about to take (docs/02 §8: a stale book disables market orders; a stale
// header strip is just a visual warning). Signals 5 (periodic REST reconciliation of
// orders/positions) and 6 (missed-pong counter) are connection/session-level, not per-feed
// market-data concerns, and belong to app::Engine / the Rust WS manager, not md::.
enum : uint32_t {
    kStaleNone = 0,
    kStaleWallClock =
        1u << 0,  // signal 1: now - data.time exceeds N x the feed's own rolling cadence
    kStaleDuplicate =
        1u << 1,  // signal 2: byte-identical consecutive payload (scoped feeds only, see below)
    kStaleL1Clock = 1u << 2,  // signal 3: exchangeStatus L1 time diverges from local wall clock
    kStaleCrossFeed =
        1u << 3,  // signal 4: l2Book touch disagrees with bbo past the coalescing window
};

// Tunable knobs for signals 1/2/4, passed in at query time (not baked into the trackers below)
// so they stay simple POD and so the thresholds are a config value, not a compile-time
// constant -- required per the brief, because the two feeds' observed cadences differ by an
// order of magnitude and neither the doc's nor a single hardcoded number is right for both:
//
//   - `l2Book` default: measured 5.32s median / 5.43s p95 here, matching the doc's 5.25s
//     figure closely enough to trust the doc's 3x staleness multiple for this feed.
//   - `bbo`: measured 0.62/s (median gap 0.68s, p95 5.3s, max 14.4s) on *testnet* here, not
//     the mainnet-BTC 7.4/s in docs/03 §W3. A threshold tuned to 7.4/s would treat testnet's
//     normal gaps as stale continuously. Deriving the threshold from each feed's own rolling
//     cadence (FeedTracker::rolling_cadence_ms(), an EWMA of observed inter-message gaps on
//     the venue's block clock) rather than a hardcoded rate is what makes one config work on
//     both networks.
struct StalenessConfig {
    double cadence_stale_multiple = 3.0;  // signal 1 multiplier on the feed's own rolling cadence
    double cadence_ewma_alpha = 0.2;      // smoothing factor for the rolling cadence estimate
    uint64_t min_cadence_floor_ms =
        50;  // clamps a message burst from collapsing the threshold to ~0
    uint64_t cross_feed_window_ms =
        2000;  // signal 4 grace period before flagging l2Book/bbo divergence
    uint64_t l1_clock_max_age_ms =
        15000;  // signal 3: how long an exchangeStatus reading stays valid
    uint64_t l1_clock_max_skew_ms = 5000;  // signal 3: acceptable |local - L1| before flagging
};

// One instance per feed (l2Book, bbo, activeAssetCtx) per asset. Tracks arrival cadence via an
// EWMA of inter-message gaps measured on the venue's own block clock (`exch_time_ms`, which
// docs/03 §W6 confirms is monotonic and shared across subscriptions for a given block), and
// optionally last-payload equality for duplicate-payload detection.
//
// `check_duplicates_` exists because signal 2 is NOT meaningful for every feed:
// `activeAssetCtx` sends byte-identical consecutive payloads routinely (measured 57 of 233
// messages in a 4-minute testnet capture -- the venue doesn't suppress unchanged mark/oracle
// pushes the way it does for `l2Book`/`bbo`), so wiring duplicate detection to it fires
// constantly and trains users to ignore the indicator. `l2Book` and `bbo`, by contrast, are
// documented (03 §W6: "0/8 ... 0/295 consecutive-identical payloads") to never repeat, so a
// repeat there is real signal. Callers must set `check_duplicates=false` for activeAssetCtx.
class FeedTracker {
public:
    void configure(bool check_duplicates, double cadence_alpha) noexcept {
        check_duplicates_ = check_duplicates;
        alpha_ = cadence_alpha;
    }

    // Call once per message. `payload_hash` is a cheap fingerprint of the message body
    // excluding `exch_time_ms` (see AssetStaleness::hash_payload) -- including the timestamp
    // would defeat duplicate detection, since it legitimately changes every message.
    void on_message(uint64_t exch_time_ms, uint64_t payload_hash) noexcept {
        if (seen_ && exch_time_ms > last_exch_time_ms_) {
            const double gap = static_cast<double>(exch_time_ms - last_exch_time_ms_);
            cadence_ewma_ms_ = seen_gap_ ? (alpha_ * gap + (1.0 - alpha_) * cadence_ewma_ms_) : gap;
            seen_gap_ = true;
        }
        duplicate_ = seen_ && payload_hash == last_payload_hash_;
        last_exch_time_ms_ = exch_time_ms;
        last_payload_hash_ = payload_hash;
        seen_ = true;
    }

    [[nodiscard]] bool has_data() const noexcept { return seen_; }
    [[nodiscard]] uint64_t last_exch_time_ms() const noexcept { return last_exch_time_ms_; }
    [[nodiscard]] double rolling_cadence_ms() const noexcept { return cadence_ewma_ms_; }

    // `now_ms` is the caller's local wall clock (docs/03 §W6 signal 1 compares local time
    // against the venue's block clock to catch a silently wedged connection).
    [[nodiscard]] uint32_t signals(uint64_t now_ms, const StalenessConfig& cfg) const noexcept {
        if (!seen_)
            return kStaleNone;
        uint32_t out = kStaleNone;
        const double cadence = cadence_ewma_ms_ > 0.0
                                   ? cadence_ewma_ms_
                                   : static_cast<double>(cfg.min_cadence_floor_ms);
        const double threshold = std::max(cadence, static_cast<double>(cfg.min_cadence_floor_ms)) *
                                 cfg.cadence_stale_multiple;
        if (now_ms > last_exch_time_ms_ &&
            static_cast<double>(now_ms - last_exch_time_ms_) > threshold)
            out |= kStaleWallClock;
        if (check_duplicates_ && duplicate_)
            out |= kStaleDuplicate;
        return out;
    }

private:
    bool seen_{};
    bool seen_gap_{};
    bool check_duplicates_{};
    bool duplicate_{};
    double alpha_{0.2};
    uint64_t last_exch_time_ms_{};
    uint64_t last_payload_hash_{};
    double cadence_ewma_ms_{};
};

// Signal 4: cross-feed consistency. Compares `l2Book`'s touch against `bbo`'s, which updates
// far more often in practice and is therefore the reference (docs/03 §W6: "l2Book.levels[0][0]
// diverging from bbo[0] for longer than the coalescing window means the book feed is
// lagging"). Only ever flags via `kStaleCrossFeed` against the pairing as a whole -- callers
// that need to know which side is suspect should prefer trusting `bbo` (md::Bbo is the
// execution truth per docs/02 §6.1) and treat `l2Book`/display as the stale one.
//
// Time here is measured on the venue's shared block clock (`exch_time_ms`), not local wall
// time, since it is comparing two feeds against each other rather than against local time --
// using the same clock both sides already share sidesteps local clock skew entirely.
class CrossFeedTracker {
public:
    void update_l2_touch(Px bid, Px ask, uint64_t exch_time_ms) noexcept {
        l2_bid_ = bid;
        l2_ask_ = ask;
        have_l2_ = true;
        recheck(exch_time_ms);
    }
    void update_bbo_touch(Px bid, Px ask, uint64_t exch_time_ms) noexcept {
        bbo_bid_ = bid;
        bbo_ask_ = ask;
        have_bbo_ = true;
        recheck(exch_time_ms);
    }
    [[nodiscard]] uint32_t signals(const StalenessConfig& cfg) const noexcept {
        if (mismatch_since_ms_ == 0 || latest_exch_ms_ < mismatch_since_ms_)
            return kStaleNone;
        return (latest_exch_ms_ - mismatch_since_ms_ > cfg.cross_feed_window_ms) ? kStaleCrossFeed
                                                                                 : kStaleNone;
    }

private:
    void recheck(uint64_t exch_time_ms) noexcept {
        if (exch_time_ms > latest_exch_ms_)
            latest_exch_ms_ = exch_time_ms;
        if (!have_l2_ || !have_bbo_)
            return;
        const bool matches = l2_bid_ == bbo_bid_ && l2_ask_ == bbo_ask_;
        if (matches)
            mismatch_since_ms_ = 0;
        else if (mismatch_since_ms_ == 0)
            mismatch_since_ms_ = exch_time_ms;
    }
    Px l2_bid_{}, l2_ask_{}, bbo_bid_{}, bbo_ask_{};
    bool have_l2_{}, have_bbo_{};
    uint64_t latest_exch_ms_{};
    uint64_t mismatch_since_ms_{};  // 0 == currently matching
};

// Signal 3: `exchangeStatus` as a heartbeat oracle (docs/03 §W6: cheap, weight 2, returns L1
// time; compare against local wall clock and ignore server info when it's too stale). md/ does
// not do networking itself -- the engine issues the `exchangeStatus` request (there is no
// PC_FETCH_* entry for it yet; see the ui_bridge report note) and feeds the reply here via
// note(). One tracker is shared account-wide (MarketStore::note_l1_clock), not per-asset,
// since exchangeStatus isn't scoped to a coin.
class L1ClockTracker {
public:
    void note(uint64_t l1_time_ms, uint64_t local_now_ms) noexcept {
        skew_ms_ = local_now_ms >= l1_time_ms ? static_cast<int64_t>(local_now_ms - l1_time_ms)
                                              : -static_cast<int64_t>(l1_time_ms - local_now_ms);
        last_seen_local_ms_ = local_now_ms;
        seen_ = true;
    }
    [[nodiscard]] uint32_t signals(uint64_t now_ms, const StalenessConfig& cfg) const noexcept {
        if (!seen_)
            return kStaleNone;
        const uint64_t age = now_ms >= last_seen_local_ms_ ? now_ms - last_seen_local_ms_ : 0;
        if (age > cfg.l1_clock_max_age_ms)
            return kStaleNone;  // reading itself expired; signal 1 covers a wedged connection
        const uint64_t abs_skew =
            skew_ms_ < 0 ? static_cast<uint64_t>(-skew_ms_) : static_cast<uint64_t>(skew_ms_);
        return abs_skew > cfg.l1_clock_max_skew_ms ? kStaleL1Clock : kStaleNone;
    }

private:
    bool seen_{};
    int64_t skew_ms_{};
    uint64_t last_seen_local_ms_{};
};

// Query-time result: which signals are currently firing for one asset. Trivially copyable and
// small (5 x uint32_t) so it belongs inside md::InstrumentSnapshot, the type that actually
// crosses the engine/UI seqlock (src/app/ui_bridge.hpp) -- unlike the trackers above, which
// stay engine-side.
struct AssetStalenessState {
    uint32_t l2_signals{};
    uint32_t bbo_signals{};
    uint32_t asset_ctx_signals{};
    uint32_t cross_feed_signals{};
    uint32_t l1_signals{};  // account-wide, mixed in by MarketStore::snapshot()

    // Observed feed latency, for the header strip's readout. `*_cadence_ms` is each feed's own
    // rolling inter-message gap (the same EWMA the staleness thresholds are derived from), and
    // `*_age_ms` is how long it has been since its last message. Together they answer "is the
    // book slow, or is the venue just not pushing?" -- which is otherwise invisible.
    uint32_t l2_cadence_ms{};
    uint32_t l2_fast_cadence_ms{};
    uint32_t l2_fast_age_ms{};
    uint32_t bbo_cadence_ms{};
    uint32_t l2_age_ms{};
    uint32_t bbo_age_ms{};

    [[nodiscard]] bool any() const noexcept {
        return (l2_signals | bbo_signals | asset_ctx_signals | cross_feed_signals | l1_signals) !=
               kStaleNone;
    }
    // What should actually gate the ticket (docs/02 §8: "Book stale ... disables market
    // orders, since the slippage estimate is no longer trustworthy"). l2Book/asset_ctx
    // staleness on its own is a display-only concern -- bbo is execution truth, and a cross-
    // feed or L1-clock flag means the whole connection's trustworthiness is in question.
    [[nodiscard]] bool blocks_market_orders() const noexcept {
        return (bbo_signals | cross_feed_signals | l1_signals) != kStaleNone;
    }
};
static_assert(std::is_trivially_copyable_v<AssetStalenessState>);

// Aggregates the per-feed trackers for one asset. Lives inside the engine-owned per-asset
// store, fed directly from MarketStore::apply(); never crosses a thread boundary itself (see
// AssetStalenessState for the part that does).
class AssetStaleness {
public:
    AssetStaleness() noexcept {
        l2_.configure(/*check_duplicates=*/true, /*cadence_alpha=*/0.2);
        l2_fast_.configure(/*check_duplicates=*/true, /*cadence_alpha=*/0.2);
        bbo_.configure(/*check_duplicates=*/true, /*cadence_alpha=*/0.2);
        // activeAssetCtx repeats identical payloads routinely -- duplicate detection must stay
        // off here. See the FeedTracker class comment for the measurement.
        asset_ctx_.configure(/*check_duplicates=*/false, /*cadence_alpha=*/0.2);
    }

    void on_l2(const pc_l2& l2, uint64_t exch_time_ms) noexcept {
        l2_.on_message(exch_time_ms, hash_payload(l2));
        const Px bid = l2.n_bid ? l2.bids[0].px : 0;
        const Px ask = l2.n_ask ? l2.asks[0].px : 0;
        cross_.update_l2_touch(bid, ask, exch_time_ms);
    }
    // The `fast:true` l2Book subscription. Tracked separately from the default book so the
    // header strip can show both cadences: they differ by an order of magnitude, and a single
    // averaged number would hide exactly the difference that matters.
    void on_l2_fast(const pc_l2& l2, uint64_t exch_time_ms) noexcept {
        l2_fast_.on_message(exch_time_ms, hash_payload(l2));
        const Px bid = l2.n_bid ? l2.bids[0].px : 0;
        const Px ask = l2.n_ask ? l2.asks[0].px : 0;
        cross_.update_l2_touch(bid, ask, exch_time_ms);
    }
    void on_bbo(const pc_bbo& bbo, uint64_t exch_time_ms) noexcept {
        bbo_.on_message(exch_time_ms, hash_payload(bbo));
        const Px bid = bbo.has_bid ? bbo.bid.px : 0;
        const Px ask = bbo.has_ask ? bbo.ask.px : 0;
        cross_.update_bbo_touch(bid, ask, exch_time_ms);
    }
    void on_asset_ctx(const pc_asset_ctx& ctx, uint64_t exch_time_ms) noexcept {
        asset_ctx_.on_message(exch_time_ms, hash_payload(ctx));
    }

    // `now_ms` is the caller's local wall clock; l1_signals is left at kStaleNone here since
    // the L1 clock is account-wide, not per-asset -- MarketStore::snapshot() fills it in.
    [[nodiscard]] AssetStalenessState state(uint64_t now_ms,
                                            const StalenessConfig& cfg) const noexcept {
        AssetStalenessState out;
        out.l2_signals = l2_.signals(now_ms, cfg);
        out.bbo_signals = bbo_.signals(now_ms, cfg);
        out.asset_ctx_signals = asset_ctx_.signals(now_ms, cfg);
        out.cross_feed_signals = cross_.signals(cfg);
        out.l2_cadence_ms = to_ms_u32(l2_.rolling_cadence_ms());
        out.l2_fast_cadence_ms = to_ms_u32(l2_fast_.rolling_cadence_ms());
        out.l2_fast_age_ms = age_ms(l2_fast_, now_ms);
        out.bbo_cadence_ms = to_ms_u32(bbo_.rolling_cadence_ms());
        out.l2_age_ms = age_ms(l2_, now_ms);
        out.bbo_age_ms = age_ms(bbo_, now_ms);
        return out;
    }

private:
    static uint32_t to_ms_u32(double ms) noexcept {
        if (!(ms > 0.0))
            return 0;
        return ms > 4294967295.0 ? 4294967295u : static_cast<uint32_t>(ms);
    }
    // Age is measured against the venue's block clock, so a local clock running ahead of the
    // venue shows as a small age rather than a negative one.
    static uint32_t age_ms(const FeedTracker& feed, uint64_t now_ms) noexcept {
        if (!feed.has_data() || now_ms <= feed.last_exch_time_ms())
            return 0;
        const uint64_t age = now_ms - feed.last_exch_time_ms();
        return age > 4294967295ull ? 4294967295u : static_cast<uint32_t>(age);
    }

    template <typename T>
    static uint64_t hash_payload(const T& value) noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        // FNV-1a over the raw struct bytes. Not a security hash -- a collision just means a
        // rare missed duplicate flag, which is an acceptable failure mode for a UI indicator.
        uint64_t h = 1469598103934665603ull;
        const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
        for (size_t i = 0; i < sizeof(T); ++i) {
            h ^= bytes[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    FeedTracker l2_{};
    FeedTracker l2_fast_{};
    FeedTracker bbo_{};
    FeedTracker asset_ctx_{};
    CrossFeedTracker cross_{};
};

// Renders which of `signals` (an OR of the kStale* bits above) fired into a short
// human-readable string, e.g. "wedged+book-lag", for a status bar tooltip. See staleness.cpp.
void describe_staleness(uint32_t signals, char* out, size_t cap) noexcept;

}  // namespace pc::md
