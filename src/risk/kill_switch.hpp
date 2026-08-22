#pragma once
namespace pc::risk {

// Manual global stop (docs/02 SS6.5, SS7 UI): one action -> cancel every resting order,
// optionally flatten every position, and refuse new orders until explicitly re-armed. The
// switch itself is a pure latch; it does not perform the cancel/flatten -- it tells the
// caller what to do via the Action it returns from trigger(), and the app layer executes that
// against pc_cancel_all / position-closing orders.
class KillSwitch {
public:
    struct Action {
        bool cancel_all{};
        bool flatten{};
    };

    // Trips the switch. `flatten` should only ever be true behind a UI confirmation (docs/07
    // Phase 5: "Confirm-on-flatten, no confirm on cancel-all") -- this class does not gate
    // that itself, since confirmation is a UI concern, not a risk-state concern.
    Action trigger(bool flatten) noexcept {
        triggered_ = true;
        return {true, flatten};
    }

    // Re-arms the switch, allowing new orders again. Deliberately a distinct, explicit call
    // from trigger() so "re-arm" can never happen as a side effect of anything else.
    void rearm() noexcept { triggered_ = false; }

    // True once triggered until rearm() is called. pre_trade::check_order reads this and
    // rejects every new order while it holds.
    bool blocks_new_orders() const noexcept { return triggered_; }

private:
    bool triggered_{};
};

}  // namespace pc::risk
