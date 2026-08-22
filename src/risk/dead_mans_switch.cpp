#include "risk/dead_mans_switch.hpp"

namespace pc::risk {

void DeadMansSwitch::roll_day_if_needed(uint64_t now_ms) noexcept {
    const uint64_t day = now_ms / kMsPerUtcDay;
    if (day != current_day_) {
        current_day_ = day;
        triggers_today_ = 0;
    }
}

bool DeadMansSwitch::trigger_now(uint64_t now_ms, uint64_t* deadline_out) noexcept {
    roll_day_if_needed(now_ms);
    if (triggers_today_ >= kMaxTriggersPerUtcDay)
        return false;
    ++triggers_today_;
    if (deadline_out)
        *deadline_out = now_ms + kMinLeadMs;
    return true;
}

uint32_t DeadMansSwitch::triggers_remaining_today(uint64_t now_ms) noexcept {
    roll_day_if_needed(now_ms);
    return kMaxTriggersPerUtcDay - triggers_today_;
}

}  // namespace pc::risk
