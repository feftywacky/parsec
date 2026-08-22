#pragma once
#include <imgui.h>

#include <cstdint>

namespace pc::ui {

// Which edge of the row the cumulative-depth fill grows from. Bids fill left-to-right (fill
// grows away from the spread, which sits at the row's right edge in the reference layout);
// asks fill right-to-left. See docs/02 §7's book sketch.
enum class DepthBarAnchor : uint8_t { Left, Right };

// Draws one cumulative-depth bar behind a book row: a filled rectangle spanning `fraction`
// (already normalised by the caller -- see panel_book.cpp's normalisation-to-the-visible-
// window comment, docs/02 §7) of [box_min, box_max]'s width, anchored to `anchor`'s edge, full
// height. `fraction` is clamped to [0, 1]; a fraction of 0 draws nothing. Call this into the
// current window's draw list *before* the row's text so the text paints on top.
void draw_depth_bar(ImDrawList* draw_list, ImVec2 box_min, ImVec2 box_max, float fraction,
                    ImU32 color, DepthBarAnchor anchor);

}  // namespace pc::ui
