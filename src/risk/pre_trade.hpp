#pragma once
#include <cstdint>

#include "core/units.hpp"
namespace pc::risk {

struct Limits {
    Usd max_order_notional{25'000 * kScale};
    Usd max_position_notional{100'000 * kScale};
    uint32_t max_leverage{10};
    uint32_t max_price_band_bps{1000};
    Usd min_notional{10 * kScale};
};

// What the order-entry gate needs to know about the world to evaluate a single intent. All of
// this is engine-local state read on the engine thread (docs/02 SS4.2) -- no I/O here.
struct OrderIntent {
    Px px{};
    Qty qty{};
    uint32_t leverage{};
};
struct RiskContext {
    Px mark{};                         // 0 if unknown; disables the price-band check
    Usd existing_position_notional{};  // this asset's current position, pre-trade
    bool kill_switch_armed{};
    int32_t rate_budget_bps{10'000};  // remaining address-action budget, 0..10000
};

// Every rejection is self-describing: which check failed, in which unit, and by how much it
// was exceeded -- never a single overloaded "margin" field whose meaning depends on which
// branch fired (docs/07 Phase 4 acceptance: "every rejection produces a toast that says which
// check failed and by how much").
enum class LimitUnit : uint8_t { None, Usd, Bps };

struct CheckOutcome {
    bool ok{};
    const char* check{"ok"};
    LimitUnit unit{LimitUnit::None};
    int64_t excess{};  // magnitude the value exceeded the limit by, in `unit`; 0 if not ok but
                       // the check is a hard boolean (e.g. kill switch) rather than a margin

    // NB: `{true}` and not `{}` -- `ok` defaults to false so that a default-constructed
    // CheckOutcome fails closed, which means the pass path has to say so explicitly.
    static CheckOutcome pass() noexcept { return {true}; }
    static CheckOutcome fail(const char* check, LimitUnit unit = LimitUnit::None,
                             int64_t excess = 0) noexcept {
        return {false, check, unit, excess};
    }
};

// Order-entry gate: rejects on the first violated limit, cheapest/most-latency-sensitive
// checks first (kill switch and rate budget are pure state reads; the notional/leverage/band
// checks all need `qty`/`px`/`mark`). Every order the engine considers sending passes this
// before it can reach pc_place_order (docs/02 SS6.5).
inline CheckOutcome check_order(const OrderIntent& intent, const RiskContext& ctx,
                                const Limits& limits) noexcept {
    if (ctx.kill_switch_armed)
        return CheckOutcome::fail("kill switch armed");

    if (ctx.rate_budget_bps < 1'000)  // <10% of address action budget remaining (docs/02 SS8)
        return CheckOutcome::fail("rate budget", LimitUnit::Bps, 1'000 - ctx.rate_budget_bps);

    const Usd order_value = notional(intent.px, intent.qty);

    if (order_value < limits.min_notional)
        return CheckOutcome::fail("minimum notional", LimitUnit::Usd,
                                  limits.min_notional - order_value);

    if (order_value > limits.max_order_notional)
        return CheckOutcome::fail("max order notional", LimitUnit::Usd,
                                  order_value - limits.max_order_notional);

    const Usd resulting_position = abs_px(ctx.existing_position_notional) + order_value;
    if (resulting_position > limits.max_position_notional)
        return CheckOutcome::fail("max position notional", LimitUnit::Usd,
                                  resulting_position - limits.max_position_notional);

    if (intent.leverage > limits.max_leverage)
        return CheckOutcome::fail(
            "max leverage", LimitUnit::Bps,
            static_cast<int64_t>(intent.leverage - limits.max_leverage) * 10'000);

    if (ctx.mark > 0) {
        const int64_t band_bps =
            static_cast<int64_t>(abs_px(intent.px - ctx.mark)) * 10'000 / ctx.mark;
        if (band_bps > limits.max_price_band_bps)
            return CheckOutcome::fail("mark price band", LimitUnit::Bps,
                                      band_bps - limits.max_price_band_bps);
    }

    return CheckOutcome::pass();
}

}  // namespace pc::risk
