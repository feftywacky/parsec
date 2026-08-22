#include "md/book_merge.hpp"

#include <algorithm>

namespace pc::md {
namespace {

// True if `px` is strictly better than `reference` on `side` -- i.e. closer to the touch.
bool strictly_better(Px px, Px reference, Side side) noexcept {
    return side == Side::Buy ? px > reference : px < reference;
}

// The price grid the venue is currently aggregating at, inferred as the smallest gap between
// adjacent levels. At native granularity this is the asset's tick; under `nSigFigs` it is the
// coarser step the venue bucketed to. Inferring it beats plumbing the subscription parameter
// down here: it is a property of the data actually in hand, so it stays correct across the
// window where a granularity change has been sent but its first snapshot has not arrived.
Px infer_step(const pc_level* levels, uint8_t count) noexcept {
    Px step = 0;
    for (uint8_t i = 1; i < count; ++i) {
        const Px gap = levels[i].px > levels[i - 1].px ? levels[i].px - levels[i - 1].px
                                                       : levels[i - 1].px - levels[i].px;
        if (gap > 0 && (step == 0 || gap < step))
            step = gap;
    }
    return step;
}

// Snaps `px` onto the grid of step `step` anchored at `anchor`, rounding away from the touch
// so the result never claims a better price than the underlying data supports.
Px snap_to_grid(Px px, Px anchor, Px step, Side side) noexcept {
    if (step <= 0)
        return px;
    const Px delta = px - anchor;
    Px k = delta / step;
    const Px rem = delta % step;
    if (rem != 0) {
        // Integer division truncates toward zero; bids floor, asks ceil.
        if (side == Side::Buy && rem < 0)
            --k;
        else if (side == Side::Sell && rem > 0)
            ++k;
    }
    return anchor + k * step;
}

// Appends `level` to `out` only if it sits strictly beyond everything already accepted on this
// side. This single rule is what discards stale levels: anything a slower feed still shows at
// or better than a fresher feed's worst accepted price has already been superseded.
void push_if_deeper(pc_level* out, uint8_t& n, const pc_level& level, Side side) noexcept {
    if (n >= PC_MAX_LEVELS || level.sz <= 0)
        return;
    if (n > 0 && !strictly_better(out[n - 1].px, level.px, side))
        return;  // not strictly deeper than the last accepted level
    out[n++] = level;
}

// Builds one side of the merged book: bbo's touch (freshest), then the fast book's levels,
// then the default book's tail.
//
// `bbo_level` is at the venue's NATIVE price precision, while the books may be aggregated to a
// coarser grid, so it is snapped onto that grid before it is allowed to influence anything.
// Two distinct jobs come out of that:
//
//   * eviction -- levels the slower feeds still show better than the snapped bbo touch have
//     been consumed, and are dropped. This is the part that matters at every granularity.
//   * injection -- the bbo level is prepended as a row only when the books have not caught up
//     to it yet. When a book level already sits at the snapped price, that level's size wins:
//     under aggregation it is the sum of a whole bucket, whereas bbo carries only the touch,
//     so injecting bbo's size there would under-report the row. At native granularity the two
//     describe the same single level, and bbo's fresher size is preferred.
void merge_side(pc_level* out, uint8_t& n, Side side, bool has_bbo, const pc_level& bbo_level,
                const pc_level* fast, uint8_t n_fast, const pc_level* deep, uint8_t n_deep,
                Px grid_step) noexcept {
    n = 0;

    const pc_level* anchor_levels = n_fast > 0 ? fast : deep;
    const uint8_t anchor_count = n_fast > 0 ? n_fast : n_deep;
    const bool have_books = anchor_count > 0;

    Px touch = 0;
    bool touch_is_native = false;
    if (has_bbo && bbo_level.sz > 0) {
        touch = have_books ? snap_to_grid(bbo_level.px, anchor_levels[0].px, grid_step, side)
                           : bbo_level.px;
        touch_is_native = touch == bbo_level.px;
    }
    const bool evict = touch != 0;

    // Injection is decided before anything is appended: if the freshest book already carries
    // the touch price, that row comes from the book instead.
    if (evict) {
        bool book_has_touch = false;
        for (uint8_t i = 0; i < anchor_count && !book_has_touch; ++i)
            book_has_touch = anchor_levels[i].px == touch;
        if (!book_has_touch || touch_is_native) {
            pc_level level = bbo_level;
            level.px = touch;
            out[n++] = level;
        }
    }

    for (uint8_t i = 0; i < n_fast; ++i) {
        if (evict && strictly_better(fast[i].px, touch, side))
            continue;
        push_if_deeper(out, n, fast[i], side);
    }
    for (uint8_t i = 0; i < n_deep; ++i) {
        if (evict && strictly_better(deep[i].px, touch, side))
            continue;
        push_if_deeper(out, n, deep[i], side);
    }
}

}  // namespace

void merge_display_book(const L2Book& deep, const L2Book& fast, const Bbo& bbo,
                        L2Book& out) noexcept {
    pc_l2 merged{};

    // The fast book only contributes while it is at least as fresh as the deep one. After a
    // reconnect that resubscribes them in either order, or if the fast subscription is dropped
    // entirely, this degrades cleanly to "deep book plus bbo touch" rather than pinning the
    // ladder's top levels to a snapshot that has stopped updating.
    const bool use_fast = fast.bid_count() + fast.ask_count() > 0 &&
                          fast.exch_time_ms() >= deep.exch_time_ms();
    const uint8_t n_fast_bid = use_fast ? fast.bid_count() : 0;
    const uint8_t n_fast_ask = use_fast ? fast.ask_count() : 0;
    const uint64_t newest_book_ms = std::max(deep.exch_time_ms(), fast.exch_time_ms());
    const bool use_bbo = bbo.exch_time_ms() >= newest_book_ms;

    // One grid for both sides: the two l2Book subscriptions always run at the same
    // granularity, and taking the finest evidence available avoids over-snapping a side whose
    // levels happen to be sparse.
    Px grid_step = 0;
    for (const auto& candidate : {infer_step(fast.bids().data(), n_fast_bid),
                                  infer_step(fast.asks().data(), n_fast_ask),
                                  infer_step(deep.bids().data(), deep.bid_count()),
                                  infer_step(deep.asks().data(), deep.ask_count())}) {
        if (candidate > 0 && (grid_step == 0 || candidate < grid_step))
            grid_step = candidate;
    }

    merge_side(merged.bids, merged.n_bid, Side::Buy, use_bbo && bbo.has_execution_bid(),
               bbo.value.bid, fast.bids().data(), n_fast_bid, deep.bids().data(),
               deep.bid_count(), grid_step);
    merge_side(merged.asks, merged.n_ask, Side::Sell, use_bbo && bbo.has_execution_ask(),
               bbo.value.ask, fast.asks().data(), n_fast_ask, deep.asks().data(),
               deep.ask_count(), grid_step);

    // Cross guard. Each side is internally consistent by construction, but the two sides are
    // built independently, so a side whose touch came from a slower feed (bbo missing that
    // side, say) can still overlap the other side's fresher touch. Trim whichever levels cross
    // the opposing touch rather than publishing a crossed book, which would make the spread
    // row read as negative and any depth walk nonsense.
    if (merged.n_bid > 0 && merged.n_ask > 0) {
        const Px best_bid = merged.bids[0].px;
        const Px best_ask = merged.asks[0].px;
        if (best_bid >= best_ask) {
            // Keep the side whose touch is fresher: bbo's, when it supplied one.
            const bool bid_is_fresh = use_bbo && bbo.has_execution_bid();
            if (bid_is_fresh) {
                uint8_t keep = 0;
                for (uint8_t i = 0; i < merged.n_ask; ++i)
                    if (merged.asks[i].px > best_bid)
                        merged.asks[keep++] = merged.asks[i];
                merged.n_ask = keep;
            } else {
                uint8_t keep = 0;
                for (uint8_t i = 0; i < merged.n_bid; ++i)
                    if (merged.bids[i].px < best_ask)
                        merged.bids[keep++] = merged.bids[i];
                merged.n_bid = keep;
            }
        }
    }

    const uint64_t exch_ms = std::max(newest_book_ms, bbo.exch_time_ms());
    const uint64_t recv_ns =
        std::max(std::max(deep.recv_time_ns(), fast.recv_time_ns()), bbo.recv_time_ns());
    out.apply(merged, exch_ms, recv_ns);
}

}  // namespace pc::md
