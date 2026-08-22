#pragma once

#include <imgui.h>

namespace pc::ui {

// Applies the parsec dark theme to the current ImGui context. Call once after
// ImGui::CreateContext() and before the first frame.
void apply_theme();

// Named accent colours other panels should reuse rather than hardcoding, so bid/ask/text
// colouring stays consistent across the book, chart, ticket, and positions panels
// (docs/05-ui.md, docs/02-architecture.md §7). Palette is the Hyperliquid-style dark teal
// scheme: teal-green longs, rose-red shorts, near-black teal backgrounds.
inline constexpr ImVec4 kColorBid{0.31F, 0.82F, 0.76F, 1.00F};       // long / bid  #50D2C1
inline constexpr ImVec4 kColorAsk{0.93F, 0.44F, 0.53F, 1.00F};       // short / ask #ED7088
inline constexpr ImVec4 kColorBidMuted{0.31F, 0.82F, 0.76F, 0.18F};  // depth bar fill
inline constexpr ImVec4 kColorAskMuted{0.93F, 0.44F, 0.53F, 0.18F};  // depth bar fill
inline constexpr ImVec4 kColorTextPrimary{0.90F, 0.94F, 0.94F, 1.00F};
inline constexpr ImVec4 kColorTextMuted{0.44F, 0.53F, 0.55F, 1.00F};
inline constexpr ImVec4 kColorWarning{0.98F, 0.76F, 0.35F, 1.00F};  // stale / reconnecting

// Surfaces, exposed so panels that paint their own backgrounds (book ladder rows, chart
// gutters) stay in step with the window chrome. Neutral muted grey rather than the teal-tinted
// near-black these started as -- bid/ask/accent still carry the teal identity, but a tinted
// background fought with them instead of setting them off.
inline constexpr ImVec4 kColorBgWindow{0.055F, 0.055F, 0.058F, 1.00F};  // #0E0E0F
inline constexpr ImVec4 kColorBgPanel{0.086F, 0.086F, 0.090F, 1.00F};   // #161617
inline constexpr ImVec4 kColorBgRaised{0.129F, 0.129F, 0.133F, 1.00F};  // #212122
inline constexpr ImVec4 kColorAccent{0.31F, 0.82F, 0.76F, 1.00F};

}  // namespace pc::ui
