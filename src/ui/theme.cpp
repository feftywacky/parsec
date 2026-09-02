#include <imgui_internal.h>

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
    // The selected tab is marked by a green rule under the label (drawn in
    // draw_tab_underlines), not by ImGui's default overline across the top edge.
    colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.0F, 0.0F, 0.0F, 0.0F);
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


// Marks the selected tab of every visible tab bar with a green rule along its bottom edge.
//
// ImGui has no underline style for tabs -- its own selected-tab marker is the overline across
// the top edge, which is switched off above. Run this once per frame after all panels have
// been submitted: by then each tab bar's layout is final, so the selected tab's rect can be
// read straight off it and the line appended to the host window's draw list (which already
// holds the tab bar itself, so the line lands on top of it and under any popup).
void draw_tab_underlines() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ImU32 col = ImGui::GetColorU32(kColorAccent);

    auto underline = [&](ImGuiTabBar* tab_bar) {
        if (tab_bar == nullptr || tab_bar->CurrFrameVisible != g.FrameCount ||
            tab_bar->Window == nullptr)
            return;
        const ImGuiTabItem* tab = ImGui::TabBarFindTabByID(tab_bar, tab_bar->SelectedTabId);
        if (tab == nullptr || tab->Width <= 0.0F)
            return;

        const float x0 = tab_bar->BarRect.Min.x + tab->Offset - tab_bar->ScrollingAnim;
        const float x1 = x0 + tab->Width;
        const float y = tab_bar->BarRect.Max.y - 1.0F;

        ImDrawList* dl = tab_bar->Window->DrawList;
        dl->PushClipRect(ImVec2(tab_bar->BarRect.Min.x, tab_bar->BarRect.Min.y),
                         ImVec2(tab_bar->BarRect.Max.x, tab_bar->BarRect.Max.y + 2.0F), true);
        dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), col, 1.0F);
        dl->PopClipRect();
    };

    // Plain tab bars (BeginTabBar) live in the context pool...
    for (int n = 0; n < g.TabBars.GetMapSize(); n++)
        underline(g.TabBars.TryGetMapData(n));

    // ...while a docked panel's tabs belong to its dock node, which owns its tab bar
    // privately and never registers it in that pool.
    for (int n = 0; n < g.DockContext.Nodes.Data.Size; n++)
        if (auto* node = static_cast<ImGuiDockNode*>(g.DockContext.Nodes.Data[n].val_p))
            underline(node->TabBar);
}

}  // namespace pc::ui
