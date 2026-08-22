#include "md/market_store.hpp"

namespace pc::md {

AssetMarket& MarketStore::get_or_create(uint32_t asset) noexcept {
    auto& owner = owners_[asset];
    if (!owner) {
        owner = std::make_unique<AssetMarket>();  // subscribe-time allocation only, see header
        // Publish only after the object is fully constructed: the release pairs with the
        // acquire in find(), so a UI thread either sees nullptr or a complete AssetMarket.
        slots_[asset].store(owner.get(), std::memory_order_release);
    }
    return *owner;
}

const AssetMarket* MarketStore::find(uint32_t asset) const noexcept {
    if (asset >= kMaxAssets)  // also catches PC_ASSET_NONE (0xFFFFFFFF)
        return nullptr;
    return slots_[asset].load(std::memory_order_acquire);
}

AssetMarket* MarketStore::find_mut(uint32_t asset) noexcept {
    if (asset >= kMaxAssets)
        return nullptr;
    return slots_[asset].load(std::memory_order_acquire);
}

void MarketStore::apply(const pc_event& e) noexcept {
    if (e.asset >= kMaxAssets)
        return;
    auto& a = get_or_create(e.asset);
    switch (e.kind) {
        case PC_EV_L2_BOOK:
            // Two l2Book subscriptions land here, told apart by PC_F_L2_FAST (set on the
            // Rust side by depth -- see rust/src/transport/ws.rs). They are kept as separate
            // books and composed at snapshot time by merge_display_book(); overwriting one
            // with the other would make the ladder alternate between 5 and 20 levels.
            if ((e.flags & PC_F_L2_FAST) != 0) {
                a.fast_book.apply(e.u.l2, e.exch_time_ms, e.recv_time_ns);
                a.staleness.on_l2_fast(e.u.l2, e.exch_time_ms);
            } else {
                a.book.apply(e.u.l2, e.exch_time_ms, e.recv_time_ns);
                a.staleness.on_l2(e.u.l2, e.exch_time_ms);
            }
            break;
        case PC_EV_BBO:
            a.bbo.apply(e.u.bbo, e.exch_time_ms, e.recv_time_ns);
            a.staleness.on_bbo(e.u.bbo, e.exch_time_ms);
            break;
        case PC_EV_TRADE:
            a.trades.apply(e.u.trade);
            // The venue serves no candles below 1m, so the sub-minute timeframes are folded
            // from this same print (see CandleSeries::fold_trade). Doing it for every
            // sub-minute series on every trade -- rather than only the one the chart happens
            // to be showing -- is what lets switching between 1s and 30s be instant instead of
            // starting each series from scratch at the moment it is first viewed.
            for (uint8_t iv = 0; iv < PC_IV_FIRST_VENUE; ++iv)
                a.candles[iv].fold_trade(e.u.trade.px, e.u.trade.sz, e.u.trade.time_ms,
                                         kIntervalMs[iv], iv);
            break;
        case PC_EV_CANDLE:
            if (e.u.candle.interval >= kIntervalCount)
                break;
            if ((e.flags & PC_F_SNAPSHOT) == 0) {
                a.candles[e.u.candle.interval].apply(e.u.candle);
                break;
            }
            // Historical run: buffer, then prepend the whole batch at once (see the
            // candle_snapshot_ comment in the header for why apply() is wrong here).
            if ((e.flags & PC_F_SNAPSHOT_BEGIN) != 0 ||
                candle_snapshot_asset_ != e.asset ||
                candle_snapshot_interval_ != e.u.candle.interval) {
                candle_snapshot_.clear();
                candle_snapshot_.reserve(CandleSeries::kCapacity);
                candle_snapshot_asset_ = e.asset;
                candle_snapshot_interval_ = e.u.candle.interval;
            }
            if (candle_snapshot_.size() >= CandleSeries::kCapacity)
                candle_snapshot_.erase(candle_snapshot_.begin());  // keep the newest kCapacity
            candle_snapshot_.push_back(e.u.candle);
            if ((e.flags & PC_F_SNAPSHOT_END) != 0) {
                a.candles[e.u.candle.interval].backfill(candle_snapshot_.data(),
                                                        candle_snapshot_.size());
                candle_snapshot_.clear();
                candle_snapshot_asset_ = PC_ASSET_NONE;
                candle_snapshot_interval_ = 0xFF;
            }
            break;
        case PC_EV_ASSET_CTX:
            a.ctx.apply(e.u.asset_ctx, e.exch_time_ms, e.recv_time_ns);
            a.staleness.on_asset_ctx(e.u.asset_ctx, e.exch_time_ms);
            break;
        default:
            break;
    }
}

bool MarketStore::snapshot(uint32_t asset, uint64_t now_ms, const StalenessConfig& cfg,
                           InstrumentSnapshot& out) const noexcept {
    const auto* a = find(asset);
    if (!a) {
        out = InstrumentSnapshot{};
        return false;
    }
    out.asset = asset;
    // The published book is the composed one: bbo's touch over the fast book's five levels
    // over the default book's tail (md/book_merge.hpp documents the measured cadences and why
    // the layering is safe). Display and depth only -- md::Bbo stays execution truth.
    merge_display_book(a->book, a->fast_book, a->bbo, out.book);
    out.bbo = a->bbo;
    out.ctx = a->ctx;
    out.staleness = a->staleness.state(now_ms, cfg);
    out.staleness.l1_signals = l1_clock_.signals(now_ms, cfg);
    return true;
}

}  // namespace pc::md
