// Unit-level tests for the pieces app::Engine::tick_timers() wires together (docs/02 §4.2,
// docs/07 Phase 5). Deliberately does not construct pc::app::Engine itself: Engine::start()
// calls pc_engine_create(), which spins up the Rust I/O layer and a real network-capable
// engine thread -- exactly what this suite must not require. Instead this exercises the same
// risk::/portfolio:: primitives tick_timers() drives, plus app::blocks_new_orders(), the free
// function factored out of Engine::drain_ui_commands() specifically so the kill-switch gate is
// testable without an Engine (see src/app/engine.hpp).
#include <doctest/doctest.h>

#include "app/engine.hpp"
#include "core/units.hpp"
#include "portfolio/reconciler.hpp"
#include "risk/dead_mans_switch.hpp"
#include "risk/kill_switch.hpp"
#include "risk/rate_budget.hpp"

using namespace pc;
using namespace pc::risk;

// --- risk::DeadMansSwitch: heartbeat cadence and venue constraints -------------------------

TEST_CASE("DeadMansSwitch: needs_refresh is false until the refresh interval elapses") {
    DeadMansSwitch dms;
    // Never refreshed yet (last_refresh_ms_ default-initializes to 0), so the first heartbeat
    // becomes due kRefreshIntervalMs after the epoch, same as after any other refresh() call --
    // there is nothing special about a fresh instance that should make it due at time 0.
    CHECK_FALSE(dms.needs_refresh(0));
    CHECK(dms.needs_refresh(DeadMansSwitch::kRefreshIntervalMs));

    const uint64_t deadline = dms.refresh(1'000);
    CHECK(deadline == 1'000 + DeadMansSwitch::kDeadlineOffsetMs);
    CHECK_FALSE(dms.needs_refresh(1'000 + DeadMansSwitch::kRefreshIntervalMs - 1));
    CHECK(dms.needs_refresh(1'000 + DeadMansSwitch::kRefreshIntervalMs));
}

TEST_CASE("DeadMansSwitch: routine refresh always schedules >= 5s lead, comfortably") {
    DeadMansSwitch dms;
    const uint64_t now = 10'000;
    const uint64_t deadline = dms.refresh(now);
    // 30s out, which is well clear of the venue's 5s minimum lead (kMinLeadMs) -- the routine
    // heartbeat should never be anywhere near the floor that would make it marginal.
    CHECK(deadline - now == DeadMansSwitch::kDeadlineOffsetMs);
    CHECK(deadline - now >= DeadMansSwitch::kMinLeadMs);
}

TEST_CASE("DeadMansSwitch: routine refresh never consumes the daily trigger budget") {
    DeadMansSwitch dms;
    uint64_t now = 0;
    // Run far more than kMaxTriggersPerUtcDay routine refreshes across the same UTC day; none
    // of them may touch the emergency trigger budget.
    for (int i = 0; i < 50; ++i) {
        now += DeadMansSwitch::kRefreshIntervalMs;
        REQUIRE(dms.needs_refresh(now));
        dms.refresh(now);
    }
    CHECK(dms.triggers_remaining_today(now) == DeadMansSwitch::kMaxTriggersPerUtcDay);
}

TEST_CASE("DeadMansSwitch: trigger_now schedules the soonest allowed deadline (now + 5s)") {
    DeadMansSwitch dms;
    uint64_t deadline = 0;
    REQUIRE(dms.trigger_now(1'000, &deadline));
    CHECK(deadline == 1'000 + DeadMansSwitch::kMinLeadMs);
}

TEST_CASE("DeadMansSwitch: exactly 10 emergency triggers per UTC day, then refuses") {
    DeadMansSwitch dms;
    const uint64_t day_start = 3 * DeadMansSwitch::kMsPerUtcDay;  // any day boundary
    for (uint32_t i = 0; i < DeadMansSwitch::kMaxTriggersPerUtcDay; ++i) {
        CAPTURE(i);
        CHECK(dms.trigger_now(day_start + i * 1'000, nullptr));
    }
    CHECK(dms.triggers_remaining_today(day_start) == 0);
    // The 11th trigger the same UTC day must be refused, not silently sent -- the venue would
    // reject it anyway, and this is exactly the guard that keeps a buggy caller from hammering
    // that rejection in a loop.
    CHECK_FALSE(dms.trigger_now(day_start + 999'000, nullptr));
}

TEST_CASE("DeadMansSwitch: the trigger budget resets on a new UTC day") {
    DeadMansSwitch dms;
    const uint64_t day1 = 5 * DeadMansSwitch::kMsPerUtcDay;
    for (uint32_t i = 0; i < DeadMansSwitch::kMaxTriggersPerUtcDay; ++i)
        REQUIRE(dms.trigger_now(day1 + i, nullptr));
    CHECK_FALSE(dms.trigger_now(day1 + 500, nullptr));

    const uint64_t day2 = day1 + DeadMansSwitch::kMsPerUtcDay;
    CHECK(dms.triggers_remaining_today(day2) == DeadMansSwitch::kMaxTriggersPerUtcDay);
    CHECK(dms.trigger_now(day2, nullptr));
}

// --- portfolio::Reconciler: the second-consecutive-divergence alarm ------------------------
// (test_portfolio.cpp already covers Reconciler's own unit behaviour in detail; this covers
// the specific acceptance criterion this pass was asked to verify -- the alarm fires on the
// second consecutive divergence for an asset, not the first, and not a later unrelated one.)

TEST_CASE("Reconciler: alarm fires on the second consecutive divergence, not the first") {
    portfolio::Reconciler r;
    pc_position local{};
    local.szi = 0;
    pc_position venue{};
    venue.szi = 10 * kScale;

    const auto first = r.reconcile(/*asset=*/7, local, venue, /*tolerance=*/kScale / 10);
    REQUIRE(first.has_value());
    CHECK_FALSE(first->alarm);
    CHECK(local.szi == venue.szi);  // snapped to venue truth

    // Local drifts again before the next snapshot.
    local.szi = venue.szi + 5 * kScale;
    const auto second = r.reconcile(7, local, venue, kScale / 10);
    REQUIRE(second.has_value());
    CHECK(second->alarm);
}

// --- app::blocks_new_orders: the kill-switch/rate-budget gate ------------------------------
// Engine::drain_ui_commands() calls this exact function before every PlaceOrder reaches
// pc_place_order (docs/07 Phase 5 acceptance: "[kill switch] must actually block new orders").

TEST_CASE("blocks_new_orders: armed kill switch blocks submission") {
    KillSwitch ks;
    RateBudget rb;
    CHECK_FALSE(app::blocks_new_orders(ks, rb));  // neither armed nor exhausted yet

    ks.trigger(/*flatten=*/false);
    CHECK(app::blocks_new_orders(ks, rb));
}

TEST_CASE("blocks_new_orders: re-arming the kill switch unblocks submission") {
    KillSwitch ks;
    RateBudget rb;
    ks.trigger(true);
    REQUIRE(app::blocks_new_orders(ks, rb));
    ks.rearm();
    CHECK_FALSE(app::blocks_new_orders(ks, rb));
}

TEST_CASE("blocks_new_orders: an exhausted rate budget blocks submission independently") {
    KillSwitch ks;  // never triggered
    RateBudget rb;
    rb.update(/*remaining=*/50, /*reset_ms=*/0);  // high-water mark = 50
    rb.update(/*remaining=*/4, /*reset_ms=*/0);   // 4/50 = 8% < the 10% reject threshold
    CHECK(rb.should_reject_new_orders());
    CHECK(app::blocks_new_orders(ks, rb));
}

TEST_CASE("blocks_new_orders: comfortable rate budget and disarmed kill switch allow orders") {
    KillSwitch ks;
    RateBudget rb;
    rb.update(/*remaining=*/1'000, /*reset_ms=*/0);
    CHECK_FALSE(app::blocks_new_orders(ks, rb));
}
