#include "ui/app_window.hpp"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>  // ImGui::DockBuilder* — internal API, needed for the default layout
#include <implot.h>

#include <cstdio>

#include "core/time.hpp"
#include "ui/dialog_connect.hpp"
#include "ui/dialog_reset.hpp"
#include "ui/dialog_unlock.hpp"
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
    // the old single-line table clipped the value row off the bottom. 0.085 fit the text but
    // left it flush against the Chart tab underneath; the extra height is deliberate breathing
    // room below the value row, not space for more content.
    ImGuiID top{};
    ImGuiID main{};
    ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Up, 0.095F, &top, &main);

    // Bottom tab group for account/history panels. Sized so the positions table shows several
    // rows plus its summary line without scrolling -- at 0.28 an account with more than two
    // positions had to be scrolled to be read, which is the wrong tradeoff against chart height.
    ImGuiID bottom{};
    ImGuiID upper{};
    ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.22F, &bottom, &upper);

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

    // After every panel, so each tab bar's layout is settled (see theme.cpp).
    draw_tab_underlines();
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

    // Open filling the screen, but as an ordinary resizable window rather than a real
    // fullscreen one. Passing a monitor to glfwCreateWindow would take exclusive fullscreen --
    // its own display mode, no menu bar, and on macOS its own Space -- which is wrong for a
    // trading terminal that gets tabbed away from constantly. Sizing to the monitor's WORK AREA
    // instead gives the whole screen minus whatever the OS reserves (menu bar and Dock here,
    // taskbar/panels elsewhere), so nothing lands underneath a system bar. Falls back to a
    // fixed size only if the monitor cannot be queried, which is the headless/odd-setup case.
    const char* window_title = "Parsec";
    int win_x = 0, win_y = 0, win_w = 1280, win_h = 800;
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        int area_x = 0, area_y = 0, area_w = 0, area_h = 0;
        glfwGetMonitorWorkarea(monitor, &area_x, &area_y, &area_w, &area_h);
        if (area_w > 0 && area_h > 0) {
            win_x = area_x;
            win_y = area_y;
            win_w = area_w;
            win_h = area_h;
        }
    }
    window_ = glfwCreateWindow(win_w, win_h, window_title, nullptr, nullptr);
    if (window_ == nullptr) {
        glfwTerminate();
        return 1;
    }
    // Position explicitly: the window manager places a new window on its own otherwise, which
    // on a multi-monitor setup routinely lands it half off the primary display.
    if (win_w != 1280 || win_h != 800)
        glfwSetWindowPos(window_, win_x, win_y);
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

    // Unlocking the keystore is what turns parsec from a market-data viewer into a trading
    // terminal (docs/06 §2). It is deliberately *not* part of startup: market data must come
    // up regardless of whether an account is connected, so the dialog is drawn over a running
    // UI rather than gating it. `unlock` submits to pc_unlock (which returns immediately and
    // zeroes the buffer) and polls pc_auth_status -- Argon2id's ~3.5s runs on a Rust blocking
    // task, so frames keep rendering throughout.
    UnlockDialog unlock;
    unlock.configure([this](char* passphrase) { return engine_.unlock(passphrase); },
                     [this] { return engine_.auth_status(); });

    // Shown instead of the unlock dialog when there is no keystore at all -- a first run.
    // On success it has just written one, so the engine has to be told it exists (it was
    // created before the file did) and can then be unlocked with the passphrase the user
    // already chose, rather than asking for it twice.
    ConnectDialog connect;

    // The way out of both dead ends the other two dialogs cannot resolve: an agent approval
    // that has lapsed (no passphrase opens it) and a passphrase that is genuinely lost. Both
    // end in the same place -- delete the local keystores, then onboard again.
    ResetDialog reset;
    reset.configure([this] { return engine_.reset_accounts(); },
                    [this] { return engine_.last_error(); });

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

        // Built before the host window so the menu bar -- which now carries the network badge
        // and the latency strip -- can read the same frame's snapshots as the panels do.
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
            draw_menu_bar_status(ctx);
            ImGui::EndMenuBar();
        }

        const ImGuiID dockspace_id = ImGui::GetID("ParsecDockSpace");
        if (!layout_built_) {
            build_default_layout(dockspace_id);
            layout_built_ = true;
        }
        ImGui::DockSpace(dockspace_id, ImVec2(0.0F, 0.0F), ImGuiDockNodeFlags_None);
        ImGui::End();

        draw_panels(ctx);

        // Drawn last so it lands on top of the docked panels. PC_AUTH_NO_KEYSTORE means no
        // keystore file exists at all -- a fresh install that has not run `parsec setup` --
        // which is not a passphrase problem and must not produce a passphrase prompt.
        const int auth = engine_.auth_status();

        // PC_AUTH_EXPIRED is raised from the keystore's cleartext header before any
        // passphrase is asked for, so the lapsed case never reaches the unlock dialog. It
        // used to: the unlock would run, fail deep inside pc_unlock's expiry check, and come
        // back as the generic PC_AUTH_FAILED the dialog renders as "wrong passphrase" -- a
        // dead end that blamed the user for a deadline they could do nothing about.
        if (auth == PC_AUTH_EXPIRED && !reset.is_open() && !unlock.dismissed())
            reset.open(ResetDialog::Reason::AgentExpired);

        if (reset.is_open()) {
            if (reset.draw(engine_.mainnet())) {
                // The keystores are gone, so the engine reports NO_KEYSTORE from here and the
                // branch below opens onboarding on the next frame. Re-arm the connect dialog
                // in case it was dismissed or already used earlier in this session.
                connect.reopen(engine_.keystore_path());
                // The unlock dialog cached the deleted keystore's header and may be holding
                // an earlier dismissal; both refer to an account that no longer exists.
                unlock.rearm();
            }
            active_hold = kActiveFrameHold;
        } else if (auth == PC_AUTH_NO_KEYSTORE && !connect.dismissed()) {
            if (connect.draw(engine_.mainnet(), engine_.keystore_path())) {
                engine_.set_keystore_path(connect.keystore_path());
                engine_.unlock(connect.passphrase_for_unlock());
                connect.done_with_passphrase();
            }
            active_hold = kActiveFrameHold;
        } else if (auth != PC_AUTH_NO_KEYSTORE && auth != PC_AUTH_UNLOCKED &&
                   !unlock.dismissed()) {
            unlock.draw(engine_.mainnet(), engine_.keystore_path());
            if (unlock.take_reset_request())
                reset.open(ResetDialog::Reason::UserRequested);
            // The dialog is interactive every frame it is up, so hold the active frame rate
            // rather than dropping to the 10 fps idle budget mid-keystroke.
            active_hold = kActiveFrameHold;
        }

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
