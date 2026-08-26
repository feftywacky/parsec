#include "ui/theme.hpp"

namespace pc::ui {

void apply_theme() {
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0F;
    style.FrameRounding = 3.0F;
    style.PopupRounding = 3.0F;
    style.ScrollbarRounding = 3.0F;
    style.GrabRounding = 3.0F;
    style.TabRounding = 3.0F;
    style.WindowBorderSize = 1.0F;
    style.FrameBorderSize = 1.0F;
    style.WindowPadding = ImVec2(8.0F, 8.0F);
    style.FramePadding = ImVec2(6.0F, 4.0F);
    style.ItemSpacing = ImVec2(6.0F, 4.0F);
    style.DockingSeparatorSize = 6.0F;

    // Interaction feedback is a translucent overlay on the single ground rather than a
    // lighter surface colour, so "no other background shades" survives hover/active states.
    constexpr ImVec4 kHover{1.0F, 1.0F, 1.0F, 0.06F};
    constexpr ImVec4 kActive{kColorAccent.x, kColorAccent.y, kColorAccent.z, 0.28F};
    constexpr ImVec4 kBorder{0.145F, 0.212F, 0.235F, 1.00F};

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = kColorTextPrimary;
    colors[ImGuiCol_TextDisabled] = kColorTextMuted;
    colors[ImGuiCol_WindowBg] = kColorBg;
    colors[ImGuiCol_ChildBg] = kColorBg;
    colors[ImGuiCol_PopupBg] = kColorBg;
    colors[ImGuiCol_Border] = kBorder;
    colors[ImGuiCol_FrameBg] = kColorBg;
    colors[ImGuiCol_FrameBgHovered] = kHover;
    colors[ImGuiCol_FrameBgActive] = kHover;
    colors[ImGuiCol_TitleBg] = kColorBg;
    colors[ImGuiCol_TitleBgActive] = kColorBg;
    colors[ImGuiCol_TitleBgCollapsed] = kColorBg;
    colors[ImGuiCol_MenuBarBg] = kColorBg;
    colors[ImGuiCol_ScrollbarBg] = kColorBg;
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(1.0F, 1.0F, 1.0F, 0.10F);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1.0F, 1.0F, 1.0F, 0.16F);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1.0F, 1.0F, 1.0F, 0.22F);
    colors[ImGuiCol_CheckMark] = kColorAccent;
    colors[ImGuiCol_SliderGrab] = kColorAccent;
    colors[ImGuiCol_SliderGrabActive] = kColorAccent;
    colors[ImGuiCol_Button] = kColorBg;
    colors[ImGuiCol_ButtonHovered] = kHover;
    colors[ImGuiCol_ButtonActive] = kActive;
    colors[ImGuiCol_Header] = kHover;
    colors[ImGuiCol_HeaderHovered] = ImVec4(1.0F, 1.0F, 1.0F, 0.10F);
    colors[ImGuiCol_HeaderActive] = kActive;
    colors[ImGuiCol_Separator] = kBorder;
    colors[ImGuiCol_SeparatorHovered] = kBorder;
    colors[ImGuiCol_SeparatorActive] = kColorAccent;
    colors[ImGuiCol_ResizeGrip] = ImVec4(1.0F, 1.0F, 1.0F, 0.08F);
    colors[ImGuiCol_ResizeGripHovered] = kHover;
    colors[ImGuiCol_ResizeGripActive] = kActive;
    colors[ImGuiCol_Tab] = kColorBg;
    colors[ImGuiCol_TabHovered] = kHover;
    colors[ImGuiCol_TabActive] = kColorBg;
    colors[ImGuiCol_TabUnfocused] = kColorBg;
    colors[ImGuiCol_TabUnfocusedActive] = kColorBg;
    colors[ImGuiCol_DockingPreview] = kActive;
    colors[ImGuiCol_DockingEmptyBg] = kColorBg;
    colors[ImGuiCol_PlotLines] = kColorAccent;
    colors[ImGuiCol_PlotHistogram] = kColorAccent;
    colors[ImGuiCol_TableHeaderBg] = kColorBg;
    colors[ImGuiCol_TableBorderStrong] = kBorder;
    colors[ImGuiCol_TableBorderLight] = kBorder;
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
}

}  // namespace pc::ui
