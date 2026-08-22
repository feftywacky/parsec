#include "ui/widgets/depth_bar.hpp"

#include <algorithm>

namespace pc::ui {

void draw_depth_bar(ImDrawList* draw_list, ImVec2 box_min, ImVec2 box_max, float fraction,
                    ImU32 color, DepthBarAnchor anchor) {
    fraction = std::clamp(fraction, 0.0F, 1.0F);
    if (fraction <= 0.0F || draw_list == nullptr)
        return;

    const float width = (box_max.x - box_min.x) * fraction;
    ImVec2 fill_min = box_min;
    ImVec2 fill_max = box_max;
    if (anchor == DepthBarAnchor::Left) {
        fill_max.x = box_min.x + width;
    } else {
        fill_min.x = box_max.x - width;
    }
    draw_list->AddRectFilled(fill_min, fill_max, color);
}

}  // namespace pc::ui
