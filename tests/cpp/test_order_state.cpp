#include <doctest/doctest.h>

#include "exec/order_state.hpp"

using namespace pc::exec;

namespace {
pc_order make_order(uint32_t asset = 1) {
    pc_order o{};
    o.asset = asset;
    o.is_buy = 1;
    o.limit_px = 100;
    o.sz = 1;
    return o;
}
}  // namespace

TEST_CASE("create registers a PendingNew record under its req_id") {
    OrderStateBook book;
    auto* r = book.create(/*req_id=*/42, make_order(), /*deadline_ms=*/1'000);
    REQUIRE(r != nullptr);
    CHECK(r->state == OrderState::PendingNew);
    CHECK(r->req_id == 42);
    CHECK(book.find_req_id(42) == r);
}

TEST_CASE("apply_ack correlates strictly by req_id, even with two orders in flight") {
    // This is the bug the rewrite fixes: with two PendingNew orders, an ack for one must never
    // attach to the other just because it happens to still be PendingNew.
    OrderStateBook book;
    auto* a = book.create(/*req_id=*/1, make_order(1), 10'000);
    auto* b = book.create(/*req_id=*/2, make_order(2), 10'000);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    pc_order_ack ack{};
    ack.status = PC_ACK_RESTING;
    ack.oid = 555;
    book.apply_ack(/*req_id=*/2, ack);

    CHECK(b->state == OrderState::Resting);
    CHECK(b->oid == 555);
    // `a` must be untouched -- old array-order-fallback behaviour would have bound this ack to
    // `a` instead, since `a` was also still PendingNew.
    CHECK(a->state == OrderState::PendingNew);
    CHECK(a->oid == 0);
}

TEST_CASE("apply_ack for an unrecognized req_id is dropped, not guessed onto some record") {
    OrderStateBook book;
    auto* a = book.create(/*req_id=*/1, make_order(), 10'000);
    REQUIRE(a != nullptr);

    pc_order_ack ack{};
    ack.status = PC_ACK_RESTING;
    ack.oid = 999;
    book.apply_ack(/*req_id=*/777, ack);  // no record was ever submitted under 777

    CHECK(a->state == OrderState::PendingNew);
    CHECK(a->oid == 0);
    CHECK(book.find_oid(999) == nullptr);
}

TEST_CASE("apply_ack with PC_ACK_ERR rejects the order") {
    OrderStateBook book;
    book.create(1, make_order(), 10'000);
    pc_order_ack ack{};
    ack.status = PC_ACK_ERR;
    book.apply_ack(1, ack);
    CHECK(book.find_req_id(1)->state == OrderState::Rejected);
}

TEST_CASE("PendingNew deadline expiry moves the order to Unknown, and never auto-resends") {
    OrderStateBook book;
    auto* r = book.create(1, make_order(), /*deadline_ms=*/1'000);
    REQUIRE(r != nullptr);

    book.expire(999);
    CHECK(r->state == OrderState::PendingNew);  // deadline not reached yet

    book.expire(1'000);
    CHECK(r->state == OrderState::Unknown);  // deadline reached -> Unknown, not resent

    // A late ack for an already-Unknown order should not silently resurrect it into Resting
    // without the caller reconciling first is a policy choice left to callers; what this test
    // guarantees is that expire() itself never touches oid or re-sends anything -- there is no
    // resend API on this class at all.
}

TEST_CASE("apply_update transitions on oid and never un-terminates a terminal state") {
    OrderStateBook book;
    book.create(1, make_order(), 10'000);
    pc_order_ack ack{};
    ack.status = PC_ACK_RESTING;
    ack.oid = 42;
    book.apply_ack(1, ack);

    pc_order_update upd{};
    upd.oid = 42;
    upd.status = PC_ORD_CANCELED;
    book.apply_update(upd);
    CHECK(book.find_oid(42)->state == OrderState::Canceled);

    // A stale/late update after a terminal state must not resurrect it.
    upd.status = PC_ORD_OPEN;
    book.apply_update(upd);
    CHECK(book.find_oid(42)->state == OrderState::Canceled);
}

TEST_CASE("apply_fill marks PartiallyFilled without needing an orderUpdate first") {
    OrderStateBook book;
    book.create(1, make_order(), 10'000);
    pc_order_ack ack{};
    ack.status = PC_ACK_RESTING;
    ack.oid = 7;
    book.apply_ack(1, ack);

    pc_fill fill{};
    fill.oid = 7;
    fill.qty = 1;
    book.apply_fill(fill);
    CHECK(book.find_oid(7)->state == OrderState::PartiallyFilled);
}
