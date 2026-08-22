#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "exec/rounder.hpp"
#include "md/candle_series.hpp"
#include "md/l2_book.hpp"

TEST_CASE("book replaces snapshots and calculates sweeps") {
    pc_l2 update{};
    update.n_bid = 2;
    update.n_ask = 2;
    update.bids[0] = {10000000000LL, 200000000, 1, 0};
    update.bids[1] = {9900000000LL, 300000000, 1, 0};
    update.asks[0] = {10100000000LL, 100000000, 1, 0};
    update.asks[1] = {10200000000LL, 300000000, 1, 0};
    pc::md::L2Book book;
    book.apply(update, 1, 2);
    CHECK(book.mid() == 10050000000LL);
    auto fill = book.sweep(pc::Side::Buy, 200000000);
    CHECK(fill.filled == 200000000);
    CHECK(fill.avg_px == 10150000000LL);
}

TEST_CASE("candles overwrite the live bucket") {
    pc::md::CandleSeries s;
    pc_candle a{};
    a.open_ms = 1;
    a.c = 1;
    s.apply(a);
    a.c = 2;
    s.apply(a);
    CHECK(s.size() == 1);
    CHECK(s.at(0).c == 2);
    a.open_ms = 2;
    s.apply(a);
    CHECK(s.size() == 2);
}

TEST_CASE("rounder floors sizes and keeps passive orders passive") {
    using namespace pc;
    using namespace pc::exec;
    CHECK(Rounder::round_sz(123456789, {2}) == 123000000);
    const auto buy = Rounder::round_px(123456789123LL, Side::Buy, {3});
    CHECK(buy <= 123456789123LL);
    const auto sell = Rounder::round_px(123456789123LL, Side::Sell, {3});
    CHECK(sell >= 123456789123LL);
}
