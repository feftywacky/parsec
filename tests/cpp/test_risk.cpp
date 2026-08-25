#include <doctest/doctest.h>

#include <cstring>
#include <string_view>

#include "core/units.hpp"
#include "risk/dead_mans_switch.hpp"
#include "risk/kill_switch.hpp"
#include "risk/pre_trade.hpp"
#include "risk/rate_budget.hpp"

using namespace pc;
using namespace pc::risk;

TEST_CASE("RateBudget: no data yet is optimistic, does not block") {
    RateBudget rb;
    CHECK(rb.remaining_bps() == 10'000);
    CHECK_FALSE(rb.should_reject_new_orders());
}

TEST_CASE("RateBudget: drops below 10% of the observed high-water mark rejects") {
    RateBudget rb;
    rb.update(10'000, 0);  // establishes the high-water mark
    CHECK_FALSE(rb.should_reject_new_orders());
    rb.update(500, 0);  // 5% of 10000
    CHECK(rb.should_reject_new_orders());
    CHECK(rb.remaining_bps() == 500);
}

TEST_CASE("RateBudget: a later higher remaining raises the high-water mark") {
    RateBudget rb;
    rb.update(1'000, 0);
    rb.update(2'000, 0);  // budget grew (more volume traded) -> new high-water mark
    CHECK(rb.remaining_bps() == 10'000);
}

TEST_CASE("KillSwitch: trigger blocks new orders until rearm") {
    KillSwitch ks;
    CHECK_FALSE(ks.blocks_new_orders());
    const auto action = ks.trigger(/*flatten=*/true);
    CHECK(action.cancel_all);
    CHECK(action.flatten);
    CHECK(ks.blocks_new_orders());
    ks.rearm();
    CHECK_FALSE(ks.blocks_new_orders());
}

TEST_CASE("DeadMansSwitch: routine refresh never consumes the daily trigger budget") {
    DeadMansSwitch dms;
    uint64_t now = 0;
    for (int i = 0; i < 500; ++i) {  // far more than 10 refreshes over the simulated session
        if (dms.needs_refresh(now)) {
            const uint64_t deadline = dms.refresh(now);
            CHECK(deadline == now + DeadMansSwitch::kDeadlineOffsetMs);
            CHECK(deadline >= now + DeadMansSwitch::kMinLeadMs);
        }
        now += DeadMansSwitch::kRefreshIntervalMs;
    }
    CHECK(dms.triggers_remaining_today(now) == DeadMansSwitch::kMaxTriggersPerUtcDay);
}

TEST_CASE("DeadMansSwitch: trigger_now is capped at 10 per UTC day") {
    DeadMansSwitch dms;
    const uint64_t start_of_day = 0;  // epoch 0 is a UTC day boundary
    uint64_t deadline{};
    for (uint32_t i = 0; i < DeadMansSwitch::kMaxTriggersPerUtcDay; ++i) {
        CHECK(dms.trigger_now(start_of_day + i, &deadline));
        CHECK(deadline == start_of_day + i + DeadMansSwitch::kMinLeadMs);
    }
    // The 11th trigger the same day is refused outright -- never silently allowed through.
    CHECK_FALSE(dms.trigger_now(start_of_day + 100, &deadline));
    CHECK(dms.triggers_remaining_today(start_of_day + 100) == 0);
}

TEST_CASE("DeadMansSwitch: the budget resets on the next UTC day") {
    DeadMansSwitch dms;
    uint64_t deadline{};
    for (uint32_t i = 0; i < DeadMansSwitch::kMaxTriggersPerUtcDay; ++i)
        CHECK(dms.trigger_now(0, &deadline));
    CHECK_FALSE(dms.trigger_now(1, &deadline));  // still day 0, budget exhausted

    const uint64_t next_day = DeadMansSwitch::kMsPerUtcDay + 1;
    CHECK(dms.trigger_now(next_day, &deadline));  // fresh day, fresh budget
    CHECK(dms.triggers_remaining_today(next_day) == DeadMansSwitch::kMaxTriggersPerUtcDay - 1);
}


TEST_CASE("order gate passes when no client-side condition blocks trading") {
    // No notional, leverage or price-band opinion lives here any more: the venue owns those.
    CHECK(check_order(RiskContext{}).ok);
}

TEST_CASE("order gate refuses while the kill switch is armed") {
    RiskContext ctx{};
    ctx.kill_switch_armed = true;
    const auto out = check_order(ctx);
    CHECK_FALSE(out.ok);
    CHECK(std::strcmp(out.check, "kill switch armed") == 0);
}

TEST_CASE("order gate refuses when the address action budget is nearly spent") {
    RiskContext ctx{};
    ctx.rate_budget_bps = 400;  // 4% left, under the 10% floor
    const auto out = check_order(ctx);
    CHECK_FALSE(out.ok);
    CHECK(std::strcmp(out.check, "rate budget") == 0);
    CHECK(out.unit == LimitUnit::Bps);
    CHECK(out.excess == 600);

    ctx.rate_budget_bps = 1'000;  // exactly at the floor is still allowed
    CHECK(check_order(ctx).ok);
}
