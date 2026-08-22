#include <doctest/doctest.h>

#include <atomic>
#include <limits>
#include <thread>

#include "core/seqlock.hpp"
#include "core/spsc_ring.hpp"
#include "core/units.hpp"

namespace {

struct Small {
    int64_t a{};
    int64_t b{};
};

}  // namespace

TEST_CASE("SnapshotSlot publishes the latest value and never blocks the reader") {
    pc::SnapshotSlot<Small> slot;
    slot.store({1, 2});
    Small out{};
    slot.load(out);
    CHECK(out.a == 1);
    CHECK(out.b == 2);

    slot.store({3, 4});
    out = slot.load();  // by-value convenience overload
    CHECK(out.a == 3);
    CHECK(out.b == 4);
}

TEST_CASE("SnapshotSlot readers always observe a self-consistent value under concurrent writes") {
    pc::SnapshotSlot<Small> slot;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        int64_t i = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            slot.store({i, i});  // a==b is the invariant a torn read would break
            ++i;
        }
    });
    for (int i = 0; i < 100000; ++i) {
        const auto v = slot.load();
        CHECK(v.a == v.b);
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
}

TEST_CASE("SpscRing preserves FIFO order and reports full/empty correctly") {
    pc::SpscRing<int, 4> ring;  // 3 usable slots
    CHECK(ring.empty());
    CHECK(ring.try_push(1));
    CHECK(ring.try_push(2));
    CHECK(ring.try_push(3));
    CHECK_FALSE(ring.try_push(4));  // full

    int out = 0;
    CHECK(ring.try_pop(out));
    CHECK(out == 1);
    CHECK(ring.try_push(4));  // room again after one pop

    CHECK(ring.try_pop(out));
    CHECK(out == 2);
    CHECK(ring.try_pop(out));
    CHECK(out == 3);
    CHECK(ring.try_pop(out));
    CHECK(out == 4);
    CHECK(ring.empty());
    CHECK_FALSE(ring.try_pop(out));
}

TEST_CASE("notional() computes px*qty/kScale and saturates instead of wrapping") {
    using namespace pc;
    CHECK(notional(10000000000LL, 200000000LL) == 20000000000LL);  // 100.0 * 2.0 = 200.0
    // px * 2.0 overflows int64 after the /kScale division; must saturate, not wrap negative.
    CHECK(notional(std::numeric_limits<Px>::max(), 2 * kScale) == std::numeric_limits<Usd>::max());
}
