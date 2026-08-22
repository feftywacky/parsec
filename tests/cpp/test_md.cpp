#include <doctest/doctest.h>

#include "md/candle_series.hpp"
#include "md/l2_book.hpp"
#include "md/market_store.hpp"
#include "md/staleness.hpp"
#include "parsec/parsec.h"

TEST_CASE("L2Book::sweep reports unfilled remainder when the book is too thin") {
    pc_l2 update{};
    update.n_bid = 0;
    update.n_ask = 1;
    update.asks[0] = {10000000000LL, 100000000, 1, 0};  // only 1.0 unit offered
    pc::md::L2Book book;
    book.apply(update, 1, 2);

    const auto fill = book.sweep(pc::Side::Buy, 300000000);  // ask for 3.0
    CHECK(fill.filled == 100000000);
    CHECK(fill.unfilled == 200000000);
}

TEST_CASE("L2Book exposes exch_time_ms for staleness checks") {
    pc_l2 update{};
    pc::md::L2Book book;
    book.apply(update, 12345, 0);
    CHECK(book.exch_time_ms() == 12345);
}

TEST_CASE("CandleSeries::backfill prepends only strictly-older history and keeps live data") {
    pc::md::CandleSeries series;
    pc_candle live{};
    live.open_ms = 300;
    live.c = 42;
    series.apply(live);  // simulates a live tick that already arrived

    pc_candle history[3]{};
    history[0].open_ms = 100;
    history[1].open_ms = 200;
    history[2].open_ms = 300;  // overlaps the live candle -- must be ignored, not overwrite it
    history[2].c = 999;
    series.backfill(history, 3);

    REQUIRE(series.size() == 3);
    CHECK(series.at(0).open_ms == 100);
    CHECK(series.at(1).open_ms == 200);
    CHECK(series.at(2).open_ms == 300);
    CHECK(series.at(2).c == 42);  // untouched by the overlapping backfill entry
}

TEST_CASE("CandleSeries::copy_recent returns the newest window without tearing") {
    pc::md::CandleSeries series;
    for (uint64_t i = 0; i < 10; ++i) {
        pc_candle c{};
        c.open_ms = i;
        c.c = static_cast<pc::Px>(i);
        series.apply(c);
    }
    pc_candle out[4]{};
    const auto n = series.copy_recent(out, 4);
    REQUIRE(n == 4);
    CHECK(out[0].open_ms == 6);
    CHECK(out[3].open_ms == 9);
}

TEST_CASE("MarketStore handles PC_ASSET_NONE and out-of-range indices without UB") {
    pc::md::MarketStore store;
    CHECK(store.find(PC_ASSET_NONE) == nullptr);
    CHECK(store.find(pc::md::MarketStore::kMaxAssets) == nullptr);
    CHECK(store.find(pc::md::MarketStore::kMaxAssets + 1000) == nullptr);

    pc_event bad{};
    bad.kind = PC_EV_BBO;
    bad.asset = PC_ASSET_NONE;
    store.apply(bad);  // must be a no-op, not UB

    pc::md::InstrumentSnapshot snap{};
    CHECK_FALSE(store.snapshot(PC_ASSET_NONE, 0, {}, snap));
    CHECK_FALSE(snap.valid());
}

TEST_CASE("MarketStore lazily allocates only touched assets and builds a small snapshot") {
    pc::md::MarketStore store;
    CHECK(store.find(7) == nullptr);

    pc_event ev{};
    ev.kind = PC_EV_BBO;
    ev.asset = 7;
    ev.exch_time_ms = 1000;
    ev.u.bbo.has_bid = true;
    ev.u.bbo.bid = {10000000000LL, 100000000, 1, 0};
    ev.u.bbo.has_ask = true;
    ev.u.bbo.ask = {10010000000LL, 100000000, 1, 0};
    store.apply(ev);

    REQUIRE(store.find(7) != nullptr);
    pc::md::InstrumentSnapshot snap{};
    REQUIRE(store.snapshot(7, 1000, {}, snap));
    CHECK(snap.asset == 7);
    CHECK(snap.bbo.execution_bid_px() == 10000000000LL);
    CHECK(snap.bbo.execution_ask_px() == 10010000000LL);
    static_assert(sizeof(pc::md::InstrumentSnapshot) <= 64 * 1024);
}

TEST_CASE("staleness: wall-clock signal fires once now exceeds N x rolling cadence") {
    using namespace pc::md;
    FeedTracker tracker;
    tracker.configure(/*check_duplicates=*/false, /*cadence_alpha=*/0.2);
    StalenessConfig cfg;
    // Establish a ~1s cadence.
    tracker.on_message(1000, 1);
    tracker.on_message(2000, 2);
    tracker.on_message(3000, 3);
    CHECK((tracker.signals(3500, cfg) & kStaleWallClock) == 0);         // within 3x cadence
    CHECK((tracker.signals(3000 + 4000, cfg) & kStaleWallClock) != 0);  // well past 3x cadence
}

TEST_CASE("staleness: duplicate detection is scoped off for activeAssetCtx-style feeds") {
    using namespace pc::md;
    AssetStaleness staleness;
    pc_asset_ctx ctx{};
    ctx.mark = 100;
    // Same payload twice in a row -- must NOT flag kStaleDuplicate for asset_ctx.
    staleness.on_asset_ctx(ctx, 1000);
    staleness.on_asset_ctx(ctx, 2000);
    const auto state = staleness.state(2000, StalenessConfig{});
    CHECK((state.asset_ctx_signals & kStaleDuplicate) == 0);
}

TEST_CASE("staleness: duplicate detection does apply to l2Book/bbo") {
    using namespace pc::md;
    AssetStaleness staleness;
    pc_l2 l2{};
    l2.n_bid = 1;
    l2.bids[0] = {100, 1, 1, 0};
    staleness.on_l2(l2, 1000);
    staleness.on_l2(l2, 2000);  // byte-identical payload repeated
    const auto state = staleness.state(2000, StalenessConfig{});
    CHECK((state.l2_signals & kStaleDuplicate) != 0);
}

TEST_CASE(
    "staleness: cross-feed signal fires when l2Book and bbo touches disagree past the window") {
    using namespace pc::md;
    AssetStaleness staleness;
    StalenessConfig cfg;
    cfg.cross_feed_window_ms = 500;

    pc_bbo bbo{};
    bbo.has_bid = true;
    bbo.bid = {100, 1, 1, 0};
    bbo.has_ask = true;
    bbo.ask = {101, 1, 1, 0};
    staleness.on_bbo(bbo, 1000);

    pc_l2 l2{};
    l2.n_bid = 1;
    l2.bids[0] = {90, 1, 1, 0};  // deliberately disagrees with bbo's touch
    l2.n_ask = 1;
    l2.asks[0] = {101, 1, 1, 0};
    staleness.on_l2(l2, 1000);
    CHECK((staleness.state(1000, cfg).cross_feed_signals & kStaleCrossFeed) == 0);  // just started

    staleness.on_l2(l2, 1000 + 600);  // still disagreeing, past the 500ms window
    CHECK((staleness.state(1000 + 600, cfg).cross_feed_signals & kStaleCrossFeed) != 0);
}

// --- locally folded sub-minute candles ---------------------------------------------------
//
// The venue serves no candles below 1m (`candleSnapshot` rejects "1s".."30s"), so
// PC_IV_1S..PC_IV_30S are folded from the trades stream. These cover the bucketing itself,
// which is the part that has no venue snapshot to fall back on if it is wrong.

TEST_CASE("CandleSeries::fold_trade builds OHLCV buckets from prints") {
    pc::md::CandleSeries series;
    const uint64_t sec = 1000;

    series.fold_trade(100 * pc::kScale, 2 * pc::kScale, 10'000, sec, PC_IV_1S);
    series.fold_trade(105 * pc::kScale, 1 * pc::kScale, 10'400, sec, PC_IV_1S);
    series.fold_trade(95 * pc::kScale, 3 * pc::kScale, 10'900, sec, PC_IV_1S);

    REQUIRE(series.size() == 1);
    const pc_candle& bar = series.at(0);
    CHECK(bar.open_ms == 10'000);
    CHECK(bar.close_ms == 10'999);
    CHECK(bar.o == 100 * pc::kScale);
    CHECK(bar.h == 105 * pc::kScale);
    CHECK(bar.l == 95 * pc::kScale);
    CHECK(bar.c == 95 * pc::kScale);
    CHECK(bar.v == 6 * pc::kScale);
    CHECK(bar.n == 3);
    CHECK(bar.interval == PC_IV_1S);
}

TEST_CASE("CandleSeries::fold_trade opens a new bucket at the interval boundary") {
    pc::md::CandleSeries series;
    series.fold_trade(100 * pc::kScale, 1 * pc::kScale, 10'999, 1000, PC_IV_1S);
    series.fold_trade(101 * pc::kScale, 1 * pc::kScale, 11'000, 1000, PC_IV_1S);
    REQUIRE(series.size() == 2);
    CHECK(series.at(0).open_ms == 10'000);
    CHECK(series.at(1).open_ms == 11'000);
    CHECK(series.at(1).o == 101 * pc::kScale);
}

TEST_CASE("CandleSeries::fold_trade skips empty buckets rather than carrying them forward") {
    // A minute with no prints produces no bar at all: there is no source of truth for a candle
    // that never traded, and inventing a flat one would misreport volume as real.
    pc::md::CandleSeries series;
    series.fold_trade(100 * pc::kScale, 1 * pc::kScale, 10'000, 1000, PC_IV_1S);
    series.fold_trade(102 * pc::kScale, 1 * pc::kScale, 45'000, 1000, PC_IV_1S);
    REQUIRE(series.size() == 2);
    CHECK(series.at(0).open_ms == 10'000);
    CHECK(series.at(1).open_ms == 45'000);
}

TEST_CASE("CandleSeries::fold_trade buckets a 30s interval on its own boundaries") {
    pc::md::CandleSeries series;
    series.fold_trade(100 * pc::kScale, 1 * pc::kScale, 1'000'000, 30'000, PC_IV_30S);
    series.fold_trade(101 * pc::kScale, 1 * pc::kScale, 1'019'999, 30'000, PC_IV_30S);
    REQUIRE(series.size() == 1);
    CHECK(series.at(0).open_ms == 990'000);
    CHECK(series.at(0).c == 101 * pc::kScale);
}
