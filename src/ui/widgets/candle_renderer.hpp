#pragma once
#include <imgui.h>

#include <cstddef>
#include <cstdint>

#include "parsec/parsec.h"

namespace pc::ui {

// Hand-rolled ImPlot candlestick renderer (docs/05-ui.md §2.1-2.4). `PlotCandlestick` is not
// ImPlot API -- it is sample code in `implot_demo.cpp`'s `MyImPlot` namespace that this file
// is lifted from, with its O(n) render loop replaced by the viewport-clipped, LOD version
// below so cost is bounded by plot pixel width rather than candle count (§2.4, "the
// highest-leverage thing in the chart").
//
// Must be called between ImPlot::BeginPlot()/EndPlot() (it calls ImPlot::BeginItem/EndItem,
// ImPlot::GetPlotLimits(), etc. itself). `candles` must be sorted ascending by `open_ms` --
// CandleSeries::copy_recent() already guarantees this -- but need NOT be uniformly spaced:
// unlike the doc's O(1) index-arithmetic clip (which assumes a fixed `dt` and breaks across a
// backfill/live-stream boundary or a missed candle), this uses binary search on `open_ms` to
// find the visible index range, which stays correct across gaps at the cost of O(log n)
// instead of O(1) -- about 13 comparisons for a full 5000-candle series, negligible next to
// the per-candle draw work it's guarding.
void plot_candlesticks(const char* label_id, const pc_candle* candles, size_t count,
                       uint64_t interval_ms, const ImVec4& bull_color, const ImVec4& bear_color);

}  // namespace pc::ui
