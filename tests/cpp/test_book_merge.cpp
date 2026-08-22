// md::merge_display_book composes bbo (~86ms, 1 level), the fast l2Book (~530ms, 5 levels) and
// the default l2Book (~5.4s, 20 levels) into one ladder. The cases that matter are the ones
// where the slower feeds are *wrong* rather than merely coarse -- a level the fast feed shows
// as consumed must not be resurrected by the stale deep book.
#include <doctest/doctest.h>

#include "md/bbo.hpp"
#include "md/book_merge.hpp"
#include "md/l2_book.hpp"

using namespace pc;
using namespace pc::md;

namespace {

pc_level lvl(double px, double sz) {
    pc_level l{};
    l.px = static_cast<Px>(px * static_cast<double>(kScale));
    l.sz = static_cast<Qty>(sz * static_cast<double>(kScale));
    return l;
}

// Builds a book whose bids descend and asks ascend from the given touches, `step` apart.
L2Book make_book(double bid_top, double ask_top, uint8_t levels, uint64_t exch_ms,
                 double step = 1.0) {
    pc_l2 l2{};
    for (uint8_t i = 0; i < levels; ++i) {
        l2.bids[i] = lvl(bid_top - i * step, 1.0);
        l2.asks[i] = lvl(ask_top + i * step, 1.0);
    }
    l2.n_bid = levels;
    l2.n_ask = levels;
    L2Book book;
    book.apply(l2, exch_ms, exch_ms * 1000000);
    return book;
}

Bbo make_bbo(double bid, double ask, uint64_t exch_ms) {
    pc_bbo b{};
    b.bid = lvl(bid, 2.0);
    b.ask = lvl(ask, 2.0);
    b.has_bid = 1;
    b.has_ask = 1;
    Bbo bbo;
    bbo.apply(b, exch_ms, exch_ms * 1000000);
    return bbo;
}

double px_of(const pc_level& l) {
    return static_cast<double>(l.px) / static_cast<double>(kScale);
}

}  // namespace

TEST_CASE("merge keeps the fresh touch and the deep tail in one ladder") {
    const L2Book deep = make_book(100.0, 101.0, 20, 1000);
    const L2Book fast = make_book(100.0, 101.0, 5, 5000);
    const Bbo bbo = make_bbo(100.0, 101.0, 5500);

    L2Book out;
    merge_display_book(deep, fast, bbo, out);

    // bbo's touch, then fast levels 2..5, then the deep book's levels 6..20 -- 20 in total,
    // with no duplicated price.
    CHECK(out.bid_count() == 20);
    CHECK(out.ask_count() == 20);
    CHECK(px_of(out.bids()[0]) == doctest::Approx(100.0));
    CHECK(out.bids()[0].sz == bbo.execution_bid_sz());  // bbo's size wins at the touch
    for (uint8_t i = 1; i < out.bid_count(); ++i)
        CHECK(out.bids()[i].px < out.bids()[i - 1].px);
    for (uint8_t i = 1; i < out.ask_count(); ++i)
        CHECK(out.asks()[i].px > out.asks()[i - 1].px);
}

TEST_CASE("a stale deep level that has since been consumed is never resurrected") {
    // The deep book is 5s old and still shows bids up to 100; the market has since traded down
    // to 97. The merged ladder must start at 97 and contain nothing above it.
    const L2Book deep = make_book(100.0, 101.0, 20, 1000);
    const L2Book fast = make_book(97.0, 98.0, 5, 5000);
    const Bbo bbo = make_bbo(97.0, 98.0, 5500);

    L2Book out;
    merge_display_book(deep, fast, bbo, out);

    CHECK(px_of(out.bids()[0]) == doctest::Approx(97.0));
    for (uint8_t i = 0; i < out.bid_count(); ++i)
        CHECK(px_of(out.bids()[i]) <= 97.0);
    for (uint8_t i = 0; i < out.ask_count(); ++i)
        CHECK(px_of(out.asks()[i]) >= 98.0);
    CHECK(out.best_bid() < out.best_ask());
}

TEST_CASE("merge degrades to deep + bbo when the fast feed is absent") {
    const L2Book deep = make_book(100.0, 101.0, 20, 5000);
    const L2Book fast;  // never received anything
    const Bbo bbo = make_bbo(100.0, 101.0, 5500);

    L2Book out;
    merge_display_book(deep, fast, bbo, out);

    CHECK(out.bid_count() == 20);
    CHECK(px_of(out.bids()[0]) == doctest::Approx(100.0));
    CHECK(px_of(out.bids()[19]) == doctest::Approx(81.0));
}

TEST_CASE("merge with no bbo still layers fast over deep") {
    const L2Book deep = make_book(100.0, 101.0, 20, 1000);
    const L2Book fast = make_book(99.0, 100.0, 5, 5000);
    const Bbo bbo;  // no bbo yet

    L2Book out;
    merge_display_book(deep, fast, bbo, out);

    CHECK(px_of(out.bids()[0]) == doctest::Approx(99.0));
    CHECK(out.best_bid() < out.best_ask());
}

TEST_CASE("merge never publishes a crossed book") {
    // bbo says the bid is 102 while the deep book's asks still start at 101: the stale asks
    // that the fresh bid has crossed must be trimmed, not published.
    const L2Book deep = make_book(100.0, 101.0, 20, 1000);
    const L2Book fast;
    const Bbo bbo = make_bbo(102.0, 103.0, 5500);

    L2Book out;
    merge_display_book(deep, fast, bbo, out);

    REQUIRE(out.bid_count() > 0);
    REQUIRE(out.ask_count() > 0);
    CHECK(out.best_bid() < out.best_ask());
    CHECK(px_of(out.asks()[0]) == doctest::Approx(103.0));
}

TEST_CASE("merge stamps the freshest contributing timestamp") {
    const L2Book deep = make_book(100.0, 101.0, 20, 1000);
    const L2Book fast = make_book(100.0, 101.0, 5, 5000);
    const Bbo bbo = make_bbo(100.0, 101.0, 5500);

    L2Book out;
    merge_display_book(deep, fast, bbo, out);
    CHECK(out.exch_time_ms() == 5500);
}

// --- venue-aggregated granularity (nSigFigs / mantissa) ----------------------------------
//
// Under aggregation the books arrive on a coarse grid ($10 here) while bbo still reports the
// native touch ($1). The merged ladder must stay on the grid: an off-grid row would break the
// ladder's alignment, and bbo's size describes one price level rather than the whole bucket.

TEST_CASE("aggregated book keeps bbo's off-grid touch from breaking the grid") {
    const L2Book deep = make_book(78580.0, 78590.0, 20, 1000, 10.0);
    const L2Book fast = make_book(78580.0, 78590.0, 5, 5000, 10.0);
    const Bbo bbo = make_bbo(78585.0, 78587.0, 5500);  // native $1 precision, off the $10 grid

    L2Book out;
    merge_display_book(deep, fast, bbo, out);

    // 78585 snaps down to the 78580 bucket, which the book already carries -- so the bucket's
    // own (complete) size is kept rather than bbo's single-level size.
    CHECK(px_of(out.bids()[0]) == doctest::Approx(78580.0));
    CHECK(out.bids()[0].sz == fast.bids()[0].sz);
    CHECK(px_of(out.asks()[0]) == doctest::Approx(78590.0));
    CHECK(out.asks()[0].sz == fast.asks()[0].sz);
    for (uint8_t i = 0; i < out.bid_count(); ++i) {
        const auto cents = static_cast<int64_t>(px_of(out.bids()[i]) * 100.0 + 0.5);
        CHECK(cents % 1000 == 0);  // every row still lands on the $10 grid
    }
    CHECK(out.bid_count() == 20);
}

TEST_CASE("aggregated book still evicts buckets the fresh touch has passed") {
    // Books are 5s stale showing bids up to 78580; bbo says the market is now at 78545.
    const L2Book deep = make_book(78580.0, 78590.0, 20, 1000, 10.0);
    const L2Book fast = make_book(78580.0, 78590.0, 5, 1000, 10.0);
    const Bbo bbo = make_bbo(78545.0, 78555.0, 5500);

    L2Book out;
    merge_display_book(deep, fast, bbo, out);

    // 78545 snaps down to 78540; every stale bucket above it is dropped.
    CHECK(px_of(out.bids()[0]) == doctest::Approx(78540.0));
    for (uint8_t i = 0; i < out.bid_count(); ++i)
        CHECK(px_of(out.bids()[i]) <= 78540.0);
    CHECK(out.best_bid() < out.best_ask());
}

TEST_CASE("native granularity still takes bbo's size at the touch") {
    // The regression guard for the case above: when the grid IS the native tick, bbo and the
    // book describe the same single level and bbo's fresher size must win.
    const L2Book deep = make_book(100.0, 101.0, 20, 1000);
    const L2Book fast = make_book(100.0, 101.0, 5, 5000);
    const Bbo bbo = make_bbo(100.0, 101.0, 5500);

    L2Book out;
    merge_display_book(deep, fast, bbo, out);
    CHECK(out.bids()[0].sz == bbo.execution_bid_sz());
    CHECK(out.bids()[0].sz != fast.bids()[0].sz);
}
