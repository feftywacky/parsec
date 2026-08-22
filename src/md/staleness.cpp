#include "md/staleness.hpp"

#include <cstdio>

namespace pc::md {

// Renders which signals fired into a short, fixed-size string for the status bar tooltip /
// ticket warning banner (docs/07 Phase 5: "a real indicator", not just a red dot with no
// explanation of why). No allocation: writes into the caller's buffer with snprintf, silently
// truncating if `cap` is too small for every fired signal (that's a display detail, not a
// correctness one -- the bitmask itself, not this string, is what gates trading logic).
void describe_staleness(uint32_t signals, char* out, size_t cap) noexcept {
    if (cap == 0)
        return;
    out[0] = '\0';
    if (signals == kStaleNone) {
        std::snprintf(out, cap, "live");
        return;
    }
    size_t used = 0;
    auto append = [&](const char* tag) {
        if (used >= cap)
            return;
        const int written = std::snprintf(out + used, cap - used, "%s%s", used ? "+" : "", tag);
        if (written > 0)
            used += static_cast<size_t>(written);
    };
    if (signals & kStaleWallClock)
        append("wedged");
    if (signals & kStaleDuplicate)
        append("repeat");
    if (signals & kStaleL1Clock)
        append("clock-skew");
    if (signals & kStaleCrossFeed)
        append("book-lag");
}

}  // namespace pc::md
