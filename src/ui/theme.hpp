#pragma once

#include <imgui.h>

namespace pc::ui {

// Applies the parsec dark theme to the current ImGui context. Call once after
// ImGui::CreateContext() and before the first frame.
void apply_theme();

// Named accent colours other panels should reuse rather than hardcoding, so bid/ask/text
// colouring stays consistent across the book, chart, ticket, and positions panels
// (docs/05-ui.md, docs/02-architecture.md §7). Palette is the Hyperliquid-style dark teal
// scheme: teal-green longs, rose-red shorts, on one flat deep-teal ground.
inline constexpr ImVec4 kColorBid{0.318F, 0.824F, 0.757F, 1.00F};  // long / bid  #51D2C1
inline constexpr ImVec4 kColorAsk{0.933F, 0.439F, 0.533F, 1.00F};  // short / ask #EE7088

// Filled areas that stand in for a quantity (depth-bar volume, row highlights). These are
// opaque tints of the ground rather than alpha-blended accent, so they read the same whatever
// they are painted over.
inline constexpr ImVec4 kColorBidMuted{0.090F, 0.224F, 0.212F, 1.00F};  // #173936
inline constexpr ImVec4 kColorAskMuted{0.196F, 0.157F, 0.180F, 1.00F};  // #32282E

inline constexpr ImVec4 kColorTextPrimary{0.90F, 0.94F, 0.94F, 1.00F};
inline constexpr ImVec4 kColorTextMuted{0.44F, 0.53F, 0.55F, 1.00F};
inline constexpr ImVec4 kColorWarning{0.98F, 0.76F, 0.35F, 1.00F};  // stale / reconnecting

// One ground for everything. Panels, popups, frames, tabs, and the dockspace all sit on the
// same #0E1A1F -- separation comes from borders and hover overlays, not from a stack of
// near-identical greys, which only ever read as noise at this contrast.
inline constexpr ImVec4 kColorBg{0.055F, 0.102F, 0.122F, 1.00F};  // #0E1A1F
inline constexpr ImVec4 kColorBgWindow = kColorBg;
inline constexpr ImVec4 kColorBgPanel = kColorBg;
inline constexpr ImVec4 kColorBgRaised = kColorBg;
inline constexpr ImVec4 kColorAccent = kColorBid;

}  // namespace pc::ui
