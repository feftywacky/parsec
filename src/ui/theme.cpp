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
    style.FrameBorderSize = 0.0F;
    style.WindowPadding = ImVec2(8.0F, 8.0F);
    style.FramePadding = ImVec2(6.0F, 4.0F);
    style.ItemSpacing = ImVec2(6.0F, 4.0F);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = kColorTextPrimary;
    colors[ImGuiCol_TextDisabled] = kColorTextMuted;
    colors[ImGuiCol_WindowBg] = kColorBgWindow;
    colors[ImGuiCol_ChildBg] = kColorBgPanel;
    colors[ImGuiCol_PopupBg] = ImVec4(0.075F, 0.075F, 0.078F, 0.98F);
    colors[ImGuiCol_Border] = ImVec4(0.145F, 0.145F, 0.150F, 1.00F);
    colors[ImGuiCol_FrameBg] = ImVec4(0.090F, 0.090F, 0.094F, 1.00F);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.125F, 0.125F, 0.130F, 1.00F);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.160F, 0.160F, 0.166F, 1.00F);
    colors[ImGuiCol_TitleBg] = ImVec4(0.043F, 0.043F, 0.046F, 1.00F);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.062F, 0.062F, 0.066F, 1.00F);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.062F, 0.062F, 0.066F, 1.00F);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.043F, 0.043F, 0.046F, 1.00F);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.165F, 0.165F, 0.170F, 1.00F);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.225F, 0.225F, 0.232F, 1.00F);
    colors[ImGuiCol_CheckMark] = kColorAccent;
    colors[ImGuiCol_SliderGrab] = kColorAccent;
    colors[ImGuiCol_SliderGrabActive] = kColorAccent;
    colors[ImGuiCol_Button] = ImVec4(0.125F, 0.125F, 0.130F, 1.00F);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.178F, 0.178F, 0.184F, 1.00F);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.31F, 0.82F, 0.76F, 0.35F);
    colors[ImGuiCol_Header] = ImVec4(0.125F, 0.125F, 0.130F, 1.00F);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.178F, 0.178F, 0.184F, 1.00F);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.31F, 0.82F, 0.76F, 0.30F);
    colors[ImGuiCol_Separator] = colors[ImGuiCol_Border];
    colors[ImGuiCol_Tab] = ImVec4(0.062F, 0.062F, 0.066F, 1.00F);
    colors[ImGuiCol_TabHovered] = ImVec4(0.178F, 0.178F, 0.184F, 1.00F);
    colors[ImGuiCol_TabActive] = ImVec4(0.098F, 0.098F, 0.102F, 1.00F);
    colors[ImGuiCol_TabUnfocused] = colors[ImGuiCol_Tab];
    colors[ImGuiCol_TabUnfocusedActive] = colors[ImGuiCol_TabActive];
    colors[ImGuiCol_DockingPreview] = ImVec4(0.31F, 0.82F, 0.76F, 0.40F);
    colors[ImGuiCol_DockingEmptyBg] = kColorBgWindow;
    colors[ImGuiCol_PlotLines] = kColorAccent;
    colors[ImGuiCol_PlotHistogram] = kColorAccent;
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.075F, 0.075F, 0.078F, 1.00F);
    colors[ImGuiCol_TableBorderStrong] = colors[ImGuiCol_Border];
    colors[ImGuiCol_TableBorderLight] = ImVec4(0.115F, 0.115F, 0.120F, 1.00F);
    colors[ImGuiCol_TableRowBg] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0F, 1.0F, 1.0F, 0.02F);
}

}  // namespace pc::ui
