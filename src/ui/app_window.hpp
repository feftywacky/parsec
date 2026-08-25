#pragma once

#include "app/engine.hpp"
#include "ui/panels.hpp"

struct GLFWwindow;

namespace pc::ui {

// The UI boundary intentionally only consumes engine snapshots. A GLFW/ImGui frontend can
// be enabled without giving rendering code access to networking or order state.
//
// Window names below are the panel dock targets built by AppWindow's default layout
// (docs/02-architecture.md §7). Panel authors should `ImGui::Begin(kWindow...)` using these
// exact names so their panels land in the right dock node.
inline constexpr const char* kWindowInstruments = "Instruments";
inline constexpr const char* kWindowChart = "Chart";
inline constexpr const char* kWindowOrderBook = "Order Book";
inline constexpr const char* kWindowTrades = "Trades";
inline constexpr const char* kWindowTicket = "Ticket";
inline constexpr const char* kWindowPositions = "Positions";
inline constexpr const char* kWindowOpenOrders = "Open Orders";
inline constexpr const char* kWindowBalances = "Balances";
inline constexpr const char* kWindowFills = "Trade History";
inline constexpr const char* kWindowFunding = "Funding History";
inline constexpr const char* kWindowOrderHistory = "Order History";
inline constexpr const char* kWindowStatus = "Status";

class AppWindow {
public:
    explicit AppWindow(app::Engine& engine) : engine_(engine) {}

    int run();

private:
    void build_default_layout(unsigned int dockspace_id);
    void draw_panels(PanelContext& ctx);

    app::Engine& engine_;
    GLFWwindow* window_{};
    bool layout_built_{false};
    ViewState view_{};
};

}  // namespace pc::ui
