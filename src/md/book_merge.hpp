#pragma once
#include "md/bbo.hpp"
#include "md/l2_book.hpp"
#include "parsec/parsec.h"

namespace pc::md {

// Composes the three order-book feeds Hyperliquid actually offers into one display book.
//
// Measured on mainnet BTC (30s capture, all three subscribed at once):
//
//     bbo                  10.13 msg/s   median gap   86 ms    1 level  / side
//     l2Book fast:true      1.87 msg/s   median gap  530 ms    5 levels / side
//     l2Book (default)      0.20 msg/s   median gap 5383 ms   20 levels / side
//
// Depth and freshness trade against each other on this venue, so no single subscription gives
// a ladder that is both deep and live: the default book's touch is up to ~5s stale, which is
// exactly the "why isn't the best bid moving?" symptom. Layering them -- freshest first, each
// deeper source contributing only the price region the fresher ones do not cover -- is the
// standard fix and is what every venue-tiered feed consumer ends up doing.
//
// The layering rule is a *price* rule, not an index rule, and that is what makes it safe:
// starting from the freshest touch and admitting a level from a slower feed only when it is
// strictly worse than everything already accepted means a stale level that has since been
// consumed can never re-enter the book. If the market moved down, the deep book's old top
// levels are simply never admitted; if it moved up, the deep levels that survive are the
// resting orders far from the touch, which are the ones least likely to have changed.
//
// The result is DISPLAY AND DEPTH ONLY, exactly like its inputs. It carries mixed-age levels
// by construction -- the touch is ~86ms old and the tail up to ~5s old -- so it must not
// become an execution price. `md::Bbo` remains execution truth (see its header); this type is
// for the ladder and for depth walks.
//
// `out` is replaced wholesale. Timestamps on the result are the freshest contributing source's,
// since that is what the staleness math should judge the touch by.
void merge_display_book(const L2Book& deep, const L2Book& fast, const Bbo& bbo,
                        L2Book& out) noexcept;

}  // namespace pc::md
