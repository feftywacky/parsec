// The C++ half of the FFI layout contract (docs/02 §5.5). `rust/src/ffi/types.rs` carries the
// mirror image of these numbers; if either side's struct drifts, one of the two builds fails
// loudly instead of the two halves silently misreading each other's memory.
//
// These are static_asserts, so they are checked at compile time -- the runtime TEST_CASE below
// exists only so the failure shows up as a named test rather than as a bare compile error, and
// so `ctest` reports the contract as something that was actually verified.
#include <doctest/doctest.h>

#include <cstddef>
#include <type_traits>

#include "parsec/parsec.h"

// ---- scalar and header invariants -------------------------------------------------------
static_assert(sizeof(pc_px) == 8, "scaled integers are int64 at 1e8");
static_assert(sizeof(pc_qty) == 8);
static_assert(sizeof(pc_usd) == 8);
static_assert(sizeof(pc_req_id) == 8);

// The event header is 32 bytes: kind+flags+asset (8) | req_id (8) | exch_time_ms (8) |
// recv_time_ns (8). The union starts immediately after it.
static_assert(offsetof(pc_event, u) == 32, "event header must stay 32 bytes");
static_assert(offsetof(pc_event, kind) == 0);
static_assert(offsetof(pc_event, flags) == 2);
static_assert(offsetof(pc_event, asset) == 4);
static_assert(offsetof(pc_event, req_id) == 8);
static_assert(offsetof(pc_event, exch_time_ms) == 16);
static_assert(offsetof(pc_event, recv_time_ns) == 24);
static_assert(alignof(pc_event) == 8);

// ---- payload sizes ----------------------------------------------------------------------
static_assert(sizeof(pc_level) == 24, "px + sz + n + pad");
static_assert(sizeof(pc_l2) == 1160);
static_assert(sizeof(pc_bbo) == 56);
static_assert(sizeof(pc_trade) == 40);
static_assert(sizeof(pc_candle) == 64);
static_assert(sizeof(pc_asset_ctx) == 56);
static_assert(sizeof(pc_asset_data) == 48);
static_assert(sizeof(pc_fee_rates) == 16);
static_assert(sizeof(pc_order_update) == 72);
static_assert(offsetof(pc_order_update, trigger_px) == 56,
              "trigger_px must sit after orig_sz, before the flag bytes");
static_assert(sizeof(pc_fill) == 72);
static_assert(sizeof(pc_position) == 72);
static_assert(sizeof(pc_account) == 48);
static_assert(sizeof(pc_spot) == 16);
static_assert(sizeof(pc_order_ack) == 224);
static_assert(sizeof(pc_conn) == 12);
static_assert(sizeof(pc_rate) == 16);
static_assert(sizeof(pc_error) == 196);
static_assert(sizeof(pc_funding) == 32);

// pc_l2 dominates the union, so it alone sets the event size. This is the assertion that
// catches "someone added a field to the biggest payload and every event got bigger".
static_assert(sizeof(pc_event) == 32 + sizeof(pc_l2));
static_assert(sizeof(pc_event) == 1192);

// ---- command structs --------------------------------------------------------------------
static_assert(sizeof(pc_order) == 56);
static_assert(sizeof(pc_order_req) == 232);
static_assert(sizeof(pc_config) == 784);

// ---- fixed-size string fields (docs/02 §5.1 rule 2) --------------------------------------
static_assert(sizeof(((pc_error*)nullptr)->msg) == PC_ERR_LEN);
static_assert(sizeof(((pc_order_ack*)nullptr)->err) == PC_ERR_LEN);
static_assert(PC_COIN_LEN == 16);
static_assert(PC_MAX_LEVELS == 24);

// ---- POD discipline: nothing that crosses may own memory or have a vtable ----------------
static_assert(std::is_trivially_copyable_v<pc_event>);
static_assert(std::is_trivially_copyable_v<pc_order_req>);
static_assert(std::is_standard_layout_v<pc_event>);
static_assert(std::is_standard_layout_v<pc_order_req>);
static_assert(std::is_standard_layout_v<pc_config>);

TEST_CASE("FFI struct layout matches the frozen ABI in include/parsec/parsec.h") {
    // Everything meaningful is asserted at compile time above; reaching this line means the
    // C++ view of the ABI is intact. The Rust view is checked by `abi_layout` in
    // rust/src/ffi/types.rs, and both must pass for the two halves to agree.
    CHECK(PC_ABI_VERSION == 3u);
    CHECK(pc_abi_version() == PC_ABI_VERSION);
}
