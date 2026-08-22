#pragma once
#include <cstdint>
namespace pc::risk {

// scheduleCancel heartbeat (docs/03-hyperliquid-api.md, docs/07 Phase 5): while parsec is
// alive it periodically pushes the venue's cancel-everything deadline further into the
// future; if the process dies, that deadline arrives and the venue cancels every resting
// order without parsec's involvement. This is the last line of defense against "parsec
// crashed with orders resting."
//
// Two constraints are real venue rules, not preferences, and both are load-bearing:
//   - `time` must be >= 5s in the future (kMinLeadMs) -- a scheduleCancel closer than that is
//     rejected outright.
//   - Only 10 triggers per UTC calendar day are allowed. "Trigger" here means an *emergency*
//     manual/immediate schedule (trigger_now), not the routine heartbeat refresh(): refreshing
//     the deadline every ~10s to a point 30s out never touches that budget, so the routine
//     heartbeat cannot itself burn the daily allowance no matter how long the session runs.
class DeadMansSwitch {
public:
    static constexpr uint64_t kRefreshIntervalMs = 10'000;
    static constexpr uint64_t kDeadlineOffsetMs = 30'000;
    static constexpr uint64_t kMinLeadMs = 5'000;
    static constexpr uint32_t kMaxTriggersPerUtcDay = 10;
    static constexpr uint64_t kMsPerUtcDay = 86'400'000;

    // Whether the routine heartbeat is due (called from tick_timers(), docs/02 SS4.2).
    bool needs_refresh(uint64_t now_ms) const noexcept {
        return now_ms - last_refresh_ms_ >= kRefreshIntervalMs;
    }

    // Records the refresh and returns the deadline to send via pc_schedule_cancel: now + 30s,
    // always comfortably clear of the 5s minimum lead. Does not consume the daily trigger
    // budget -- this is the routine path, not the emergency one.
    uint64_t refresh(uint64_t now_ms) noexcept {
        last_refresh_ms_ = now_ms;
        return now_ms + kDeadlineOffsetMs;
    }

    // Emergency/manual trigger: schedule the soonest deadline the venue allows (now + 5s).
    // Budget-limited to kMaxTriggersPerUtcDay per UTC calendar day, tracked internally, so a
    // bug that calls this in a loop cannot silently exhaust the day's allotment -- once
    // exhausted it refuses (returns false) rather than sending a request the venue would
    // reject anyway, leaving the caller free to fall back to a manual cancel-all instead.
    // On success, `*deadline_out` receives the deadline that was scheduled.
    bool trigger_now(uint64_t now_ms, uint64_t* deadline_out) noexcept;

    uint32_t triggers_remaining_today(uint64_t now_ms) noexcept;

private:
    void roll_day_if_needed(uint64_t now_ms) noexcept;

    uint64_t last_refresh_ms_{};
    uint64_t current_day_{UINT64_MAX};
    uint32_t triggers_today_{};
};

}  // namespace pc::risk
