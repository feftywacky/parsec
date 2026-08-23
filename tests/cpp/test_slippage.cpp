#include <doctest/doctest.h>

#include "exec/slippage.hpp"
#include "md/l2_book.hpp"
#include "parsec/parsec.h"

using namespace pc;

namespace {

// Asks at 100/101/102, one unit each; bids at 99/98/97, one unit each.
md::L2Book make_book() {
    pc_l2 update{};
    update.n_ask = 3;
    update.asks[0] = {100 * kScale, kScale, 1, 0};
    update.asks[1] = {101 * kScale, kScale, 1, 0};
    update.asks[2] = {102 * kScale, kScale, 1, 0};
    update.n_bid = 3;
    update.bids[0] = {99 * kScale, kScale, 1, 0};
    update.bids[1] = {98 * kScale, kScale, 1, 0};
    update.bids[2] = {97 * kScale, kScale, 1, 0};
    md::L2Book book;
    book.apply(update, 1, 2);
    return book;
}

}  // namespace

TEST_CASE("limit fill estimate never prices an order through its own limit") {
    const md::L2Book book = make_book();

    // A buy limit BELOW the market takes nothing: the whole size rests. The sweep model this
    // replaced reported the ask (100) as the estimated fill for an order capped at 95.
    const auto resting = exec::SlippageModel::estimate_limit(book, Side::Buy, 2 * kScale,
                                                             95 * kScale, 100 * kScale);
    CHECK(resting.immediate == 0);
    CHECK(resting.avg_px == 0);
    CHECK(resting.resting == 2 * kScale);

    // A buy limit at 101 can take the 100 and the 101 levels but not the 102 one, so 2 of the
    // 3 units fill (VWAP 100.5) and the last unit rests at 101.
    const auto partial = exec::SlippageModel::estimate_limit(book, Side::Buy, 3 * kScale,
                                                             101 * kScale, 100 * kScale);
    CHECK(partial.immediate == 2 * kScale);
    CHECK(partial.avg_px == 100 * kScale + kScale / 2);
    CHECK(partial.resting == kScale);
    CHECK(partial.avg_px <= 101 * kScale);
    CHECK(partial.slippage_bps == 50);  // 100.5 vs a 100 reference

    // Fully marketable: nothing rests.
    const auto full = exec::SlippageModel::estimate_limit(book, Side::Buy, kScale, 102 * kScale,
                                                          100 * kScale);
    CHECK(full.immediate == kScale);
    CHECK(full.avg_px == 100 * kScale);
    CHECK(full.resting == 0);
}

TEST_CASE("limit fill estimate mirrors correctly for a sell") {
    const md::L2Book book = make_book();

    const auto resting = exec::SlippageModel::estimate_limit(book, Side::Sell, 2 * kScale,
                                                             105 * kScale, 99 * kScale);
    CHECK(resting.immediate == 0);
    CHECK(resting.resting == 2 * kScale);

    // A sell limit at 98 hits the 99 and 98 bids only.
    const auto partial = exec::SlippageModel::estimate_limit(book, Side::Sell, 3 * kScale,
                                                             98 * kScale, 99 * kScale);
    CHECK(partial.immediate == 2 * kScale);
    CHECK(partial.avg_px == 98 * kScale + kScale / 2);
    CHECK(partial.avg_px >= 98 * kScale);
    CHECK(partial.resting == kScale);
    CHECK(partial.slippage_bps == 50);  // sold 0.5 below the 99 reference
}

TEST_CASE("limit fill estimate refuses nonsense inputs") {
    const md::L2Book book = make_book();
    CHECK(exec::SlippageModel::estimate_limit(book, Side::Buy, 0, 100 * kScale, 100 * kScale)
              .immediate == 0);
    CHECK(exec::SlippageModel::estimate_limit(book, Side::Buy, kScale, 0, 100 * kScale)
              .immediate == 0);
}
