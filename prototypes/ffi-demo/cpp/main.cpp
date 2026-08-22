#include <cstdint>
#include <cstdio>
#include <atomic>
#include <thread>
#include <chrono>

extern "C" {
struct HlBookLevel { double px; double sz; uint32_t n; uint32_t _pad; };
struct HlBookUpdate {
    char coin[16];
    uint64_t time_ms;
    uint32_t n_bids, n_asks;
    HlBookLevel levels[40];
};
struct HlClient;
HlClient* hl_client_new();
void hl_client_free(HlClient*);
int hl_subscribe_book(HlClient*, void (*cb)(void*, const HlBookUpdate*), void*, uint32_t);
}

static std::atomic<uint64_t> g_count{0};

// Invoked from a tokio worker thread: must be reentrancy-safe / no locks held.
static void on_book(void* user, const HlBookUpdate* ev) {
    (void)user;
    g_count.fetch_add(1, std::memory_order_relaxed);
    if (ev->time_ms < 2) {
        std::printf("[cpp] coin=%s t=%llu bid=%.2f x %.2f ask=%.2f x %.2f\n",
            ev->coin, (unsigned long long)ev->time_ms,
            ev->levels[0].px, ev->levels[0].sz,
            ev->levels[1].px, ev->levels[1].sz);
    }
}

int main() {
    HlClient* c = hl_client_new();
    if (!c) { std::printf("FAILED to create client\n"); return 1; }
    hl_subscribe_book(c, &on_book, nullptr, 20);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::printf("[cpp] callbacks received: %llu\n", (unsigned long long)g_count.load());
    hl_client_free(c);
    std::printf("[cpp] clean shutdown\n");
    return 0;
}
