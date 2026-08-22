#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>
namespace pc {

// Single-producer/single-consumer lock-free ring buffer (docs/05-ui.md §5.2 "SPSC ring --
// fills, acks, toasts"): for streams where *every* item matters, unlike SnapshotSlot where
// only the latest value matters. Capacity is one larger than the usable slot count so
// head == tail unambiguously means empty (the classic SPSC ring trick -- no separate "full"
// flag or size counter to keep in sync). try_push() returning false is backpressure the
// producer must handle (drop-and-count, per docs/02 §4.1), never a signal to grow the buffer:
// every ring is preallocated at startup so steady-state push/pop never allocates.
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity > 1);
    static_assert(std::is_trivially_copyable_v<T>);

public:
    [[nodiscard]] bool try_push(const T& value) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = increment(head);
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        values_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }
    [[nodiscard]] bool try_pop(T& value) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        value = values_[tail];
        tail_.store(increment(tail), std::memory_order_release);
        return true;
    }
    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t increment(std::size_t i) noexcept { return (i + 1) % Capacity; }
    alignas(128) std::array<T, Capacity> values_{};
    alignas(128) std::atomic<std::size_t> head_{0};
    alignas(128) std::atomic<std::size_t> tail_{0};
};
}  // namespace pc
