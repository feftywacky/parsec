#include "ui/app_window.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>  // ImGui::DockBuilder* — internal API, needed for the default layout
#include <implot.h>

#include <cstdio>

#include "core/time.hpp"
#include "ui/panels.hpp"
#include "ui/theme.hpp"

namespace pc::ui {

namespace {

// Idle/active redraw budget (docs/05-ui.md §4): idle at ~10 fps, burst to ~60 fps for a short
// window after activity so idle CPU stays well under the <2% acceptance bar in docs/07 Phase 0,
// while input and data updates still feel immediate — glfwWaitEventsTimeout wakes immediately
// on any real GLFW event regardless of the timeout used.
constexpr double kIdleFps = 10.0;
constexpr double kActiveFps = 60.0;
constexpr int kActiveFrameHold = 30;  // frames to keep rendering active after last activity

// Logical point size for the UI font. Trading panels are dense -- this is a readability/density
// tradeoff, not a rendering one; sharpness comes from rasterising at DPI scale (load_ui_font).
constexpr float kUiFontSizePt = 14.0F;

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

}  // namespace

void AppWindow::build_default_layout(unsigned int dockspace_id_in) {
    const ImGuiID dockspace_id = dockspace_id_in;

    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, ImGui::GetMainViewport()->Size);

    // Top instrument strip. Sized for its actual content: the header strip is two text lines
    // tall (label over value) plus window padding and the dock tab bar, so the 0.06 that fit
    // the old single-line table clipped the value row off the bottom.
    ImGuiID top{};
    ImGuiID main{};
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.085F, &top, &main);

    // Bottom tab group for account/history panels.
    ImGuiID bottom{};
    ImGuiID upper{};
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.28F, &bottom, &upper);

    // Ticket on the far right.
    ImGuiID ticket{};
    ImGuiID rest{};
    ImGui::DockBuilderSplitNode(upper, ImGuiDir_Right, 0.20F, &ticket, &rest);

    // Order book to the right of the chart.
    ImGuiID book{};
    ImGuiID chart{};
    ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.28F, &book, &chart);

    ImGui::DockBuilderDockWindow(kWindowInstruments, top);
    ImGui::DockBuilderDockWindow(kWindowChart, chart);
    ImGui::DockBuilderDockWindow(kWindowOrderBook, book);
    ImGui::DockBuilderDockWindow(kWindowTrades, book);
    ImGui::DockBuilderDockWindow(kWindowTicket, ticket);
    ImGui::DockBuilderDockWindow(kWindowPositions, bottom);
    ImGui::DockBuilderDockWindow(kWindowOpenOrders, bottom);
    ImGui::DockBuilderDockWindow(kWindowBalances, bottom);
    ImGui::DockBuilderDockWindow(kWindowFills, bottom);
    ImGui::DockBuilderDockWindow(kWindowFunding, bottom);
    ImGui::DockBuilderDockWindow(kWindowOrderHistory, bottom);
    ImGui::DockBuilderDockWindow(kWindowStatus, bottom);

    ImGui::DockBuilderFinish(dockspace_id);
}

void AppWindow::draw_panels(PanelContext& ctx) {
    // Order matters only for input handling: the book runs before the ticket so a level click
    // lands in the same frame it is consumed (ViewState::price_pick_pending).
    draw_instruments(ctx);
    draw_chart(ctx);
    draw_book(ctx);
    draw_trades(ctx);
    draw_ticket(ctx);
    draw_positions(ctx);
    draw_open_orders(ctx);
    draw_balances(ctx);
    draw_fills(ctx);
    draw_funding(ctx);
    draw_order_history(ctx);
    draw_status_bar(ctx);
}


namespace {

// Loads the first available system monospace face, sized for the display's pixel density.
//
// Two separate problems are being solved here, and both showed up as "the text looks low
// resolution". The first is that ImGui's default font is ProggyClean, a 13px BITMAP face --
// there is no such thing as a crisp rendering of it at any other size. The second is HiDPI: on
// a Retina display the framebuffer is 2x the window's logical size, so a font rasterised at
// logical size gets upscaled by the GPU and blurs. The fix for that is to rasterise at
// `size * scale` and then divide back down with FontGlobalScale, so glyphs are built at native
// framebuffer resolution while every layout coordinate in the app stays in logical pixels.
//
// Monospace rather than a proportional UI face on purpose: every panel here aligns its columns
// with fixed-pixel SameLine() offsets, and digits that do not share an advance width make a
// price ladder visibly ragged. Returns false if no candidate could be loaded, in which case the
// caller keeps the built-in font rather than rendering nothing.
bool load_ui_font(ImGuiIO& io, float scale) {
    // Ordered by preference: SF Mono is macOS's own terminal face, Menlo its predecessor, then
    // the usual Linux monospaces. A .ttc collection loads its first face, which is Regular for
    // all of these.
    static const char* kCandidates[] = {
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    };

    ImFontConfig config;
    config.OversampleH = 2;  // horizontal oversampling sharpens stems at small sizes
    config.OversampleV = 1;
    config.PixelSnapH = true;

    const float pixel_size = kUiFontSizePt * scale;
    for (const char* path : kCandidates) {
        if (io.Fonts->AddFontFromFileTTF(path, pixel_size, &config) != nullptr)
            return true;
    }
    return false;
}

}  // namespace

int AppWindow::run() {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        std::fputs("GLFW initialization failed.\n", stderr);
        return 1;
    }

#if defined(__APPLE__)
    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    const char* window_title =
        engine_.mainnet() ? "Parsec — Hyperliquid mainnet" : "Parsec — Hyperliquid testnet";
    window_ = glfwCreateWindow(1280, 800, window_title, nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;  // deterministic default layout every run, not a stale .ini

    // Content scale, not framebuffer/window ratio: GLFW reports the display's DPI scaling
    // directly, and it is what the font needs to be rasterised against.
    float dpi_scale = 1.0F;
    float dpi_scale_y = 1.0F;
    glfwGetWindowContentScale(window_, &dpi_scale, &dpi_scale_y);
    if (!(dpi_scale > 0.0F))
        dpi_scale = 1.0F;

    if (load_ui_font(io, dpi_scale)) {
        // Glyphs were rasterised at framebuffer resolution; scale the whole font back down so
        // widget layout keeps working in logical pixels.
        io.FontGlobalScale = 1.0F / dpi_scale;
    }

    apply_theme();

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    int active_hold = kActiveFrameHold;  // render active for the first few frames on startup

    // Cheap staleness check to decide whether the market data actually changed since the last
    // frame, so an idle book doesn't force active-fps redraws. This polls rather than reacting
    // to a cross-thread dirty flag/glfwPostEmptyEvent from the engine (docs/05-ui.md §4) because
    // AppWindow only owns UI files here; wiring that push-based signal is for the engine owner.
    bool have_prev_bbo = false;
    pc_bbo prev_bbo{};
    app::InstrumentSnapshot instrument{};
    app::AssetUniverseSnapshot universe{};
    app::PortfolioSnapshot portfolio{};

    while (!glfwWindowShouldClose(window_)) {
        glfwWaitEventsTimeout(1.0 / (active_hold > 0 ? kActiveFps : kIdleFps));

        // Read the small per-instrument snapshot across the seqlock. Note this fills a
        // caller-owned buffer rather than returning by value -- the snapshot must never
        // transit the stack as a temporary (see core/seqlock.hpp's size cap).
        engine_.bridge().load_instrument(instrument);
        bool data_changed = false;
        if (instrument.asset != PC_ASSET_NONE) {
            const pc_bbo& bbo = instrument.bbo.value;
            if (!have_prev_bbo || bbo.bid.px != prev_bbo.bid.px || bbo.ask.px != prev_bbo.ask.px ||
                bbo.bid.sz != prev_bbo.bid.sz || bbo.ask.sz != prev_bbo.ask.sz ||
                bbo.has_bid != prev_bbo.has_bid || bbo.has_ask != prev_bbo.has_ask) {
                data_changed = true;
                prev_bbo = bbo;
                have_prev_bbo = true;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        constexpr ImGuiWindowFlags kHostFlags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_MenuBar;
        ImGui::Begin("ParsecDockSpaceHost", nullptr, kHostFlags);
        ImGui::PopStyleVar(3);

        if (ImGui::BeginMenuBar()) {
            const bool live =
                instrument.asset != PC_ASSET_NONE &&
                (instrument.bbo.has_execution_bid() || instrument.bbo.has_execution_ask());
            ImGui::TextDisabled("%s live", live ? "market" : "connecting");
            ImGui::EndMenuBar();
        }

        const ImGuiID dockspace_id = ImGui::GetID("ParsecDockSpace");
        if (!layout_built_) {
            build_default_layout(dockspace_id);
            layout_built_ = true;
        }
        ImGui::DockSpace(dockspace_id, ImVec2(0.0F, 0.0F), ImGuiDockNodeFlags_None);
        ImGui::End();

        const uint32_t active = engine_.active_asset();
        view_.active_asset = active;
        engine_.bridge().load_universe(universe);
        engine_.bridge().load_portfolio(portfolio);
        uint8_t sz_decimals = 0;
        for (uint32_t i = 0; i < universe.count; ++i) {
            if (universe.assets[i].asset == active) {
                sz_decimals = universe.assets[i].sz_decimals;
                break;
            }
        }
        PanelContext ctx{instrument,
                         universe,
                         portfolio,
                         engine_.ui_market(active),
                         engine_.bridge(),
                         view_,
                         unix_ms(),
                         sz_decimals,
                         engine_.mainnet()};
        draw_panels(ctx);

        const bool ui_active =
            io.WantCaptureMouse &&
            (ImGui::IsAnyItemActive() || io.MouseDelta.x != 0.0F || io.MouseDelta.y != 0.0F);
        active_hold = (data_changed || ui_active || io.WantTextInput)
                          ? kActiveFrameHold
                          : (active_hold > 0 ? active_hold - 1 : 0);

        ImGui::Render();
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(window_, &width, &height);
        glViewport(0, 0, width, height);
        glClearColor(0.035F, 0.045F, 0.065F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window_);
    glfwTerminate();
    return 0;
}

}  // namespace pc::ui
