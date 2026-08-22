#include "app/ui_bridge.hpp"

#include <cstdio>
#include <cstring>

namespace pc::app {

UiEvent make_toast(const char* text, uint8_t severity, uint64_t recv_time_ns) noexcept {
    UiEvent event{};
    event.kind = UiEventKind::Toast;
    event.recv_time_ns = recv_time_ns;
    event.u.toast.severity = severity;
    std::strncpy(event.u.toast.text, text, sizeof(event.u.toast.text) - 1);
    event.u.toast.text[sizeof(event.u.toast.text) - 1] = '\0';
    return event;
}

PortfolioSnapshot make_portfolio_snapshot(const pc_account& account, bool account_valid,
                                          const portfolio::PositionBook& positions) noexcept {
    PortfolioSnapshot out{};
    out.account = account;
    out.account_valid = account_valid;
    for (uint32_t asset = 0; asset < portfolio::PositionBook::kMaxAssets &&
                             out.position_count < PortfolioSnapshot::kMaxPositions;
         ++asset) {
        if (const auto* p = positions.find(asset))
            out.positions[out.position_count++] = *p;
    }
    return out;
}

}  // namespace pc::app
