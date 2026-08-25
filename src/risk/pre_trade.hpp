#pragma once
#include <cstdint>

#include "core/units.hpp"
namespace pc::risk {

// What the order-entry gate needs to know about the world. All of this is engine-local state
// read on the engine thread (docs/02 SS4.2) -- no I/O here. It is deliberately about the
// CLIENT, not about the order: nothing here describes size, price or leverage, because the
// venue is the authority on whether an order is acceptable.
struct RiskContext {
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

// Order-entry gate (docs/02 SS6.5).
//
// This deliberately holds NO opinion about size, leverage or price. Hyperliquid enforces its
// own margin, tick, lot, minimum-notional and oracle-band rules on every order and rejects
// with a reason that reaches the UI as a toast, so a second local set of thresholds only ever
// duplicated the venue or contradicted it -- and contradicting it meant blocking orders the
// account could well afford, with a message about a config file rather than about trading.
//
// What survives is the two gates the venue cannot enforce for us, because they are about this
// client's own state rather than the order's contents: the kill switch (a local emergency
// stop) and the address action budget (exhausting it gets the address rate-limited, which the
// venue punishes rather than prevents).
inline CheckOutcome check_order(const RiskContext& ctx) noexcept {
    if (ctx.kill_switch_armed)
        return CheckOutcome::fail("kill switch armed");

    if (ctx.rate_budget_bps < 1'000)  // <10% of address action budget remaining (docs/02 SS8)
        return CheckOutcome::fail("rate budget", LimitUnit::Bps, 1'000 - ctx.rate_budget_bps);

    return CheckOutcome::pass();
}

}  // namespace pc::risk
