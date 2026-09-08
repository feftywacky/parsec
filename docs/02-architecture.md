# Parsec — Architecture

> Design-level document. Read `01-overview.md` first for scope, and `03-hyperliquid-api.md`
> for the exchange facts this design is built on.

---

## 1. One-paragraph summary

`parsec` is a single native binary. C++ owns `main()`, all trading state, and the UI.
Rust is compiled into a static library and owns exactly one thing: **the network edge** —
TLS, HTTP, WebSocket, JSON/msgpack codecs, and secp256k1 / EIP-712 signing. The two halves
talk over a small, allocation-free C ABI: C++ pushes *commands*, Rust pushes back *events*.
Nothing crosses the boundary except POD structs with fixed-size fields and integers.

```
┌──────────────────────────────── parsec (one process) ────────────────────────────────┐
│                                                                                      │
│  ┌─── UI thread (main) ────────┐   ┌─── Engine thread ─────┐   ┌─ Rust I/O (tokio) ─┐│
│  │ GLFW + Dear ImGui           │   │ book / positions      │   │ ws mgr (reconnect) ││
│  │ 60 fps, event-driven        │   │ order state machine   │   │ http client        ││
│  │                             │   │ risk + rounding       │   │ signer (keystore)  ││
│  │  reads  ── Seqlock ─────────┼──▶│                       │◀──┼── pc_poll() drains ││
│  │  snapshots (book/portfolio) │   │                       │   │   event queue      ││
│  │                             │   │                       │   │                    ││
│  │  writes ── SPSC ring ───────┼──▶│  ── pc_* commands ────┼──▶│  submit / cancel   ││
│  │  (user intents)             │   │                       │   │                    ││
│  └─────────────────────────────┘   └───────────────────────┘   └────────────────────┘│
└──────────────────────────────────────────────────────────────────────────────────────┘
```

**Why this split.** Hyperliquid's action signing is msgpack → keccak256 → EIP-712 → ECDSA.
That stack is one `Cargo.toml` line in Rust and a week of subtle, wrong-by-default work in
C++. TLS WebSockets are likewise a solved problem in Rust (`tokio-rustls`) and a dependency
swamp in C++ (Boost.Beast + OpenSSL). Everything *after* the bytes are parsed — book
maintenance, position math, risk, order state, rendering — stays in C++ where you want the
control.

**Why three threads.** The render thread must never block on a socket. The engine must react
faster than a 16 ms frame. The network must not be throttled by either. Each boundary is
crossed by exactly one lock-free structure, chosen by access pattern:

| Direction | Structure | Why |
|---|---|---|
| Rust → Engine | drain call (`pc_poll`, blocking w/ timeout) | every event matters; must not spin |
| Engine → UI | seqlock snapshot slot | only the *latest* book/portfolio matters; readers never block writers |
| Engine → UI | SPSC ring | fills/toasts/log lines — every event matters |
| UI → Engine | SPSC ring | user intents, low rate |
| Engine → Rust | `pc_*` FFI call → tokio mpsc | non-blocking submit, result returns as event |

---

## 2. Numeric model

**No floating point anywhere in the trading path.** Doubles appear only in ImGui draw
calls and in the last conversion before display.

```cpp
// core/units.hpp
namespace pc {
inline constexpr int64_t SCALE = 100'000'000;      // 1e8, fixed for every asset

using Px  = int64_t;   // price  × 1e8   (BTC @ 118342.5 -> 11834250000000)
using Qty = int64_t;   // size   × 1e8   (0.00123 BTC    ->      123000)
using Usd = int64_t;   // notional × 1e8

// notional = px * qty / 1e8, computed in 128 bits to avoid overflow
inline Usd notional(Px px, Qty qty) {
    return static_cast<Usd>((static_cast<__int128>(px) * qty) / SCALE);
}
}
```

A single global scale (rather than the per-asset `price_scale`/`size_scale` in the earlier
`BtcPerpConfig`) removes a whole class of unit-mixing bugs. `int64` at 1e8 covers prices up
to ~9.2e10 and sizes to ~9.2e10 — orders of magnitude beyond anything on this venue.

Per-asset *precision rules* (`szDecimals`, the 5-significant-figure price rule, min notional)
are not a representation concern; they are applied once, at order entry, by `exec::Rounder`
(see §6.3). Market data is stored at full 1e8 resolution exactly as received.

Rust parses the venue's decimal **strings** into these scaled integers at the edge, so the
C++ side never sees a string price or a `double`.

---

## 3. Repository layout

```
parsec/
├── CMakeLists.txt              # top level: options, deps, corrosion, targets
├── cmake/
│   └── Dependencies.cmake      # FetchContent: imgui, glfw, json, doctest, corrosion
├── docs/                       # this doc set
├── rust/
│   ├── Cargo.toml              # crate-type = ["staticlib"]
│   └── src/
│       ├── lib.rs              # pub mod ffi; runtime bootstrap
│       ├── ffi/
│       │   ├── mod.rs          # extern "C" surface, catch_unwind wrappers
│       │   ├── types.rs        # #[repr(C)] event/command structs (mirrors parsec.h)
│       │   └── queue.rs        # event queue + blocking drain
│       ├── transport/
│       │   ├── http.rs         # POST /info, POST /exchange (rustls, keep-alive)
│       │   ├── ws.rs           # subscribe/unsubscribe, ping, reconnect+resubscribe
│       │   └── backoff.rs
│       ├── codec/
│       │   ├── info.rs         # serde models for /info responses
│       │   ├── exchange.rs     # action payloads (canonical field order!)
│       │   ├── ws_msg.rs       # subscription message models
│       │   └── decimal.rs      # decimal-string <-> scaled i64
│       ├── signer/
│       │   ├── l1.rs           # msgpack + nonce + keccak + EIP-712 Agent
│       │   ├── user_signed.rs  # HyperliquidTransaction:* typed data
│       │   ├── keystore.rs     # argon2id + XChaCha20-Poly1305 at rest
│       │   └── agent.rs        # approveAgent / API-wallet lifecycle
│       └── session.rs          # ties signer+transport, nonce allocator, rate budget
├── include/parsec/
│   └── parsec.h                # THE C ABI header — hand-written, single source of truth
└── src/
    ├── main.cpp
    ├── core/                   # units, time, spsc_ring, seqlock, arena, log, result
    ├── md/                     # L2Book, TradeTape, CandleSeries, AssetCtx, MarketStore
    ├── exec/                   # OrderRouter, OrderState, Rounder, SlippageModel
    ├── portfolio/              # PositionBook, AccountState, PnL, Reconciler
    ├── risk/                   # PreTradeChecks, KillSwitch, RateBudget, DeadMansSwitch
    ├── app/                    # Config, Session, EngineThread, wiring
    └── ui/                     # imgui panels (chart, book, ticket, positions, ...)
```

`include/parsec/parsec.h` is **hand-written, not generated**. cbindgen would work, but the
header is ~200 lines, it is the contract both sides must agree on, and a generated file that
silently changes layout is exactly the failure mode we cannot afford. A Rust unit test
asserts `size_of`/`align_of`/field offsets against the constants in the header, and CI
fails if they drift (§5.5).

---

## 4. Threading model

### 4.1 Rust I/O
A multi-thread tokio runtime with **2 worker threads** (this workload is a handful of
sockets, not a server). Tasks:
- one task per WebSocket connection, owning its subscription set;
- one HTTP client (connection-pooled, HTTP/2, keep-alive pre-warmed at startup);
- one nonce/rate-budget actor serializing all signed actions.

Every parsed message becomes a `PcEvent` pushed into a bounded MPSC queue
(`crossbeam::ArrayQueue`, capacity 8192). On overflow the *oldest market-data* event of that
class is dropped and an overflow counter is bumped — user/account events are never dropped
(separate priority queue).

### 4.2 C++ engine thread
```cpp
for (;;) {
    int n = pc_poll(engine_, ev_buf_, kBatch, /*timeout_ns=*/500'000);  // 0.5 ms
    for (int i = 0; i < n; ++i) apply(ev_buf_[i]);
    drain_ui_commands();
    tick_timers();          // dead-man's switch, reconciliation, order timeouts
    publish_snapshots();    // seqlock stores, only if dirty
}
```
Blocking drain with a short timeout: the thread sleeps when idle (no 100 % core burn) and
wakes within microseconds when a message lands. All engine state is **single-threaded** —
no locks, no atomics, no shared mutation anywhere in `md/`, `exec/`, `portfolio/`, `risk/`.

### 4.3 UI thread
`glfwWaitEventsTimeout(1.0/60)` so an idle window costs almost nothing, with the engine
raising `glfwPostEmptyEvent()` when a redraw-worthy update lands. Each frame the UI does
`try_load()` on the seqlock slots, drains its notification ring, and draws. It never touches
engine state directly and never calls `pc_*`.

---

## 5. The FFI contract

### 5.1 Rules
1. Only `#[repr(C)]` PODs cross. No `String`, `Vec`, `Option<T>`, or enums-with-payload.
2. Strings are fixed-size `char[N]` UTF-8, NUL-padded, truncated if longer. Coin symbols
   `char[16]`, error text `char[192]`.
3. Rust never frees C memory; C never frees Rust memory. Buffers are caller-allocated.
4. Every `extern "C"` function body is wrapped in `catch_unwind`; a panic returns an error
   code, never unwinds into C++.
5. Every function is non-blocking except `pc_poll`, which blocks only up to its timeout.

### 5.2 Handle and lifecycle
```c
typedef struct pc_engine pc_engine;

pc_engine* pc_engine_create(const pc_config* cfg);   // spawns tokio runtime
void       pc_engine_destroy(pc_engine*);            // graceful shutdown, joins runtime
int32_t    pc_last_error(pc_engine*, char* out, uint32_t cap);
```

### 5.3 Commands (C++ → Rust)
All return `pc_req_id` (u64, monotonic) or `0` on immediate rejection; the *outcome* always
arrives later as an event carrying the same `req_id`.

```c
pc_req_id pc_subscribe        (pc_engine*, const char* coin, uint32_t stream_mask);
pc_req_id pc_unsubscribe      (pc_engine*, const char* coin, uint32_t stream_mask);
pc_req_id pc_place_order      (pc_engine*, const pc_order_req*);
pc_req_id pc_cancel_order     (pc_engine*, uint32_t asset, uint64_t oid);
pc_req_id pc_cancel_by_cloid  (pc_engine*, uint32_t asset, const uint8_t cloid[16]);
pc_req_id pc_modify_order     (pc_engine*, const pc_modify_req*);
pc_req_id pc_cancel_all       (pc_engine*);
pc_req_id pc_set_leverage     (pc_engine*, uint32_t asset, bool is_cross, uint32_t leverage);
pc_req_id pc_set_iso_margin   (pc_engine*, uint32_t asset, int64_t usd_delta);
pc_req_id pc_schedule_cancel  (pc_engine*, uint64_t deadline_ms);   // dead man's switch
pc_req_id pc_fetch            (pc_engine*, uint32_t what, const char* coin);
```

`stream_mask` is a bitset (`PC_STREAM_L2 | PC_STREAM_TRADES | PC_STREAM_CANDLE_*`), so one
call sets up an instrument.

### 5.4 Events (Rust → C++)
```c
typedef struct {
    uint16_t  kind;        // PC_EV_*
    uint16_t  flags;       // PC_F_SNAPSHOT, PC_F_STALE, ...
    uint32_t  asset;       // asset index, or PC_ASSET_NONE
    uint64_t  req_id;      // correlates with the command that caused it, else 0
    uint64_t  exch_time_ms;
    uint64_t  recv_time_ns; // local monotonic, stamped the instant bytes arrived
    union { /* one struct per kind, see §5.6 */ } u;
} pc_event;                 // fixed size; the union is dominated by pc_l2 (48 levels)
```

| kind | payload |
|---|---|
| `PC_EV_L2_BOOK` | `pc_l2 { uint8_t n_bid, n_ask; pc_level bids[24], asks[24]; }` |
| `PC_EV_BBO` | best bid/ask px+sz |
| `PC_EV_TRADE` | px, qty, side, tid, time |
| `PC_EV_CANDLE` | interval id, o/h/l/c/v/n, open_ms, close_ms, `PC_F_CLOSED` |
| `PC_EV_ASSET_CTX` | mark, oracle, mid, funding, open_interest, day_vlm, prev_day_px |
| `PC_EV_ORDER_UPDATE` | oid, cloid, status enum, px, sz, orig_sz, side |
| `PC_EV_FILL` | oid, cloid, px, qty, fee, closed_pnl, is_maker, dir enum, hash |
| `PC_EV_POSITION` | one leg: coin, szi (signed), entry_px, position_value, upnl, roe, liq_px, margin_used, leverage type+value |
| `PC_EV_ACCOUNT` | account_value, total_margin_used, total_ntl_pos, withdrawable, maintenance_margin |
| `PC_EV_ORDER_ACK` | req_id, status enum, oid, filled_sz, avg_px, `char err[192]` |
| `PC_EV_CONN` | which socket, state enum, reconnect count |
| `PC_EV_RATE` | remaining request budget, reset time |
| `PC_EV_ERROR` | code, `char msg[192]` |

`PC_EV_POSITION` and `PC_EV_ACCOUNT` are emitted together, bracketed by
`PC_F_SNAPSHOT_BEGIN` / `PC_F_SNAPSHOT_END`, so the engine can swap portfolio state
atomically rather than merging partial updates.

### 5.5 Layout verification
The full header is in `04-rust-layer.md` §5.1 — it is the single source of truth, and the
numbers below must be read off it rather than copied from here.

`rust/src/ffi/types.rs` ends with:
```rust
#[test] fn abi_layout() {
    // header is 32 bytes: kind+flags+asset (8) | req_id (8) | exch_time_ms (8) | recv_time_ns (8)
    assert_eq!(offset_of!(PcEvent, u), 32);
    assert_eq!(size_of::<PcEvent>(), size_of::<PcEventHeader>() + size_of::<PcL2>());
    assert_eq!(size_of::<PcLevel>(), 24);        // px + sz + n + pad
    // ... one line per field the header names
}
```
and `tests/abi_header_test.cpp` does the mirror-image `static_assert`s. Both run in CI. If
someone adds a field to one side, the build breaks loudly instead of misreading memory.

### 5.6 Why not `cxx` / cbindgen
`cxx` is excellent when both sides share rich types and call each other synchronously. Here
the boundary is deliberately one-way, POD-only, and crossed by a *queue*, not by calls — the
whole point is that the tokio worker never runs C++ code and the engine thread never blocks
on Rust. A hand-written 200-line header expresses that better than a generated one, costs
nothing to maintain at this size, and keeps the build free of a codegen step.

---

## 6. C++ core

### 6.1 `md/` — market data

```cpp
struct Level { Px px; Qty sz; uint32_t n_orders; };

class L2Book {                    // fixed capacity, zero allocation, trivially copyable
    Level bids_[kMaxLevels];      // descending px
    Level asks_[kMaxLevels];      // ascending px
    uint8_t n_bid_, n_ask_;
    uint64_t exch_time_ms_, recv_time_ns_, seq_;
public:
    void apply(const pc_l2&);                 // full-snapshot replace
    Px  best_bid() const; Px best_ask() const;
    Px  mid() const;  Px spread() const;
    Qty depth_within(Px px, Side) const;      // for slippage + depth bars
    // walks the book; returns (avg_px, filled_qty) for a taker order
    struct Sweep { Px avg_px; Qty filled; Usd notional; };
    Sweep sweep(Side, Qty) const;
};
```

Hyperliquid publishes the book as a **snapshot per update**, not a delta stream (confirmed
in `03-hyperliquid-api.md` §WS). That is a gift: no sequence-gap handling, no diff
application, no desync. `apply()` is a memcpy-class operation and `L2Book` is trivially
copyable, which is exactly what the seqlock needs.

- `TradeTape` — ring of the last 512 prints per coin, for the Trades tab and for tape-based
  displays.
- `CandleSeries` — per (coin, interval) ring of `Candle`. The venue's `candle` subscription
  emits the *in-progress* candle repeatedly; `CandleSeries::apply()` overwrites index
  `[last]` while `open_ms` matches and appends when it advances. Backfill comes from
  `candleSnapshot` over REST on first open of a timeframe, then the ws stream keeps it live.
- `AssetCtx` — mark, oracle, funding, OI, 24 h volume/change: everything the header strip
  and the risk math need.
- `MarketStore` — `coin → { L2Book, TradeTape, CandleSeries[n_intervals], AssetCtx }`,
  indexed by asset id (a dense `uint32_t`), not by string, on every hot path.

**Feed selection is a first-order design decision, not a detail.** Live measurement (03 §W3)
found the public feeds fall into sharply different tiers:

| Feed | rate | bytes/msg | role in parsec |
|---|---|---|---|
| `bbo` | 7.4/s | 145 | **execution truth** — the price the ticket and slippage model use |
| `l2Book fast:true` (5 levels) | 1.8/s | 470 | the active instrument's book panel |
| `l2Book` default (20 levels) | 0.23/s | 1600 | depth display for the watchlist |
| `allMids` | 0.2/s | 15800 | **not subscribed** — `activeAssetCtx` covers it per-coin |

`bbo` carries ~360x more top-of-book information per byte than the default `l2Book`. So
`MarketStore` keeps **two** notions of price: `L2Book` for display and depth math, and a
separate `Bbo` slot updated far more often, which is what `exec/` and `risk/` read. Wiring the
ticket to `L2Book::best_bid()` instead of the `bbo` feed would quietly trade on a price up to
five seconds stale.

The divergence between them is also the cheapest staleness alarm available (§8).

#### 6.1.1 The feed source is a config value, not an architecture

Everything above the FFI boundary consumes `pc_event`s. Nothing in `md/`, `exec/`,
`portfolio/`, or `ui/` knows whether those events came from `api.hyperliquid.xyz`, a local
node, or a recorded file. That property is worth protecting deliberately, because it is what
keeps the following options open at the cost of a Rust module rather than a rewrite:

| Source | What it buys | What it costs | v1? |
|---|---|---|---|
| **Public WS + REST** | 20 book levels, ~0.5–5 s book coalescing, `bbo` at ~7.4/s | nothing | ✅ **D8** |
| `order_book_server` (open source) | `n_levels` up to 100, `l4Book` order-level stream | **requires a non-validating node**; ~100 GB logs/day; public gossip ports 4001/4002; "as is, educational purposes" disclaimer, not core-team maintained; block batching makes it "a few ms slower than a streaming implementation" | ❌ |
| Own node + `--write-raw-book-diffs` | true tick-by-tick book with `user`/`oid` per change | same node burden, plus building the reconstruction yourself | ❌ |
| Recorded frames | determinism, regression tests, backtesting | none — it is the replay harness (08 §4) | Phase 6 |

**On the open-source `order_book_server` specifically:** it would *not* interfere with the C++
core. Architecturally it is just another WebSocket speaking a near-identical protocol, and the
Rust transport layer would normalize it into the same `pc_event`s. The reason it is out is that
it does not help. It is not a standalone service — it is a front-end for a node you must run.
And what it adds is depth beyond 20 levels and sub-coalescing-window book transitions, neither
of which a human-driven terminal can act on: the book panel displays a dozen levels, and a
trader cannot react inside 500 ms. The gain is near zero for v1's actual use case, against a
very large operational cost.

That calculus changes completely if parsec ever grows automated strategies — which is exactly
why the boundary is drawn where it is.

### 6.2 `exec/` — order routing

`OrderRouter` owns the local truth about every order the client has ever sent this session:

```
        pc_place_order()            PC_EV_ORDER_ACK(resting)      PC_EV_FILL(partial)
Local ─────────────────▶ PendingNew ──────────────────▶ Resting ────────────────▶ PartiallyFilled
                             │                             │                          │
              ACK(error)     │            ORDER_UPDATE(canceled/rejected)             │ FILL(full)
                             ▼                             ▼                          ▼
                          Rejected                     Canceled                     Filled
```

Rules that make this survivable:
- **Every order carries a cloid** — 16 bytes: `[8B session id][8B monotonic seq]`. The venue
  echoes it on updates and fills, so an ack lost to a reconnect is recoverable, and a restart
  can tell "my order" from "an order I placed yesterday".
- **`PendingNew` has a deadline.** If no ack in `order_ack_timeout_ms` (default 5 000), the
  router marks it `Unknown`, refuses to reuse the slot, and forces a reconciliation fetch.
  It does *not* resend — duplicate orders are worse than a missing one.
- The router never trusts a local fill; it reconciles against `userFills` (§6.4).

### 6.3 `exec::Rounder` — the part that silently loses money if wrong

Hyperliquid rejects orders whose price violates *both* a significant-figure rule and a
decimal-place rule (exact statement in `03-hyperliquid-api.md` §Precision). `Rounder` is the
only place that knows those rules:

```cpp
class Rounder {
public:
    Px  round_px(uint32_t asset, Px raw, Side, RoundMode) const;  // toward passive by default
    Qty round_sz(uint32_t asset, Qty raw) const;                  // floor to szDecimals
    bool check_min_notional(Px, Qty) const;                       // >= $10
    Px  marketable_px(uint32_t asset, Side, Px ref, Bps slippage) const; // IOC "market" px
};
```
`RoundMode::Passive` is the default so a rounded limit buy never becomes more aggressive than
the trader asked for. Unit tests cover the published examples plus every boundary
(`szDecimals` 0..6, prices spanning 0.0001 → 100000, integers, exactly-5-sig-fig values).

**Market orders.** Hyperliquid has no market order type; the venue's own UI sends an IOC
limit at an aggressive price. `marketable_px()` = mid (or best opposite) moved by
`slippage_bps` (default 500 = 5 %, matching the SDKs), then rounded. Before sending, the
ticket shows the *modelled* fill from `L2Book::sweep()` — that is where the "Est / Max
slippage" line in the screenshot comes from, and it is real information, not a placeholder.

### 6.4 `portfolio/` — positions and P&L

```cpp
struct Position {
    uint32_t asset; Qty szi;            // signed: >0 long, <0 short
    Px entry_px, liq_px;
    Usd position_value, unrealized_pnl, margin_used, funding_paid;
    int32_t roe_bps;
    MarginMode mode; uint32_t leverage;
};
```

Two sources of truth, deliberately kept apart:
- **Authoritative** — the `clearinghouseState` and `openOrders` WS subscriptions, re-pushed
  in full every ~4 s. (`webData2` no longer exists; it became `webData3` and now carries only
  vault/agent metadata — 03 §W2.3.)
- **Optimistic** — the local projection after a fill, so the UI reacts in the same frame the
  fill lands rather than waiting for the next snapshot.

`Reconciler` compares them every snapshot. Any divergence beyond a tolerance snaps the local
state to the venue's, logs a `RECONCILE_DIVERGENCE` line with both values, and (if it happens
twice in a row) trips a soft alarm in the UI. Silent drift between what the client thinks it
holds and what it actually holds is the single most dangerous bug class in a trading client,
so it gets an explicit, loud mechanism rather than trust.

Mark-to-market P&L is recomputed from `AssetCtx::mark` on every tick — cheap, and it keeps
the UI's unrealized P&L identical to the venue's definition (which uses mark, not last).

### 6.5 `risk/` — pre-trade checks

Every order passes `risk::check(intent, portfolio, market)` on the engine thread before it
can reach `pc_place_order`. Rejections never leave the process.

| Check | Default | Rationale |
|---|---|---|
| Max order notional | $25 000 | fat-finger |
| Max position notional per asset | $100 000 | concentration |
| Max account leverage | 10× | blow-up guard |
| Price band vs mark | ±10 % | typo'd limit price |
| Min notional | $10 | venue rule; reject early with a clear message |
| Order rate | 20 / 10 s | protects the address rate-limit budget |
| Kill switch | off | when armed: cancel-all, flatten, refuse new orders |
| Dead man's switch | 30 s | `scheduleCancel` heartbeat; if parsec dies, the venue cancels |

All are config values, all are surfaced in the UI, and every rejection produces a toast that
says which check failed and by how much.

---

## 7. UI

Layout mirrors the reference screenshot, as a single ImGui dockspace:

```
┌────────────────────────────────────────────────────────────────────────────────────┐
│ instrument strip: BTC ETH SOL HYPE …  |  mark  oracle  24h Δ  24h vol  OI  funding  │
├──────────────────────────────────────────────┬──────────────┬──────────────────────┤
│                                              │  Order Book  │  Order Ticket        │
│   Chart (candlesticks + volume)             │  ┌─ asks ─┐  │  Isolated | 3x | ...  │
│   1m 5m 15m 1h 4h D  ·  log/linear           │  │ depth  │  │  Market | Limit      │
│   overlays: position entry, liq px,          │  └────────┘  │  Buy/Long Sell/Short │
│             resting orders, TP/SL            │   spread     │  size + % slider     │
│                                              │  ┌─ bids ─┐  │  reduce only, TP/SL  │
│                                              │  └────────┘  │  liq px / margin /   │
│                                              │  [Trades]    │  slippage / fees     │
├──────────────────────────────────────────────┴──────────────┴──────────────────────┤
│ Balances │ Positions │ Open Orders │ Trade History │ Funding │ Order History        │
└────────────────────────────────────────────────────────────────────────────────────┘
```

Panels are independent widgets over the seqlock snapshot; each is `ui/panel_*.cpp` and none
holds state beyond view preferences. Interaction rules worth fixing early:

- **Click a book level → the limit price populates the ticket.** The single most-used
  interaction in any trading UI.
- **Drag a resting-order line on the chart → modify.** Guarded by a confirm threshold.
- **Depth bars** are drawn from cumulative size normalized to the visible window, not to the
  whole book, so they stay legible when one side is thin.
- **Every number that came from the venue is displayed exactly as the venue would round it**
  — the ticket previews the *rounded* price and size before you can click Buy, so what you
  see is what gets signed.

Rendering the chart: the whole widget is hand-rolled on `ImDrawList` over a **bar-index** x
axis, not a time axis, so gaps collapse and zoom is pixels-per-bar. Bounding bar spacing below
at 1 px bounds the draw loop at one candle per pixel column, whatever the series holds.
(Details, and why ImPlot could not express this, live in `05-ui.md` §2.)

---

## 8. Failure behaviour

The design's answer to "what happens when the network hiccups" is fixed here rather than
discovered later:

| Failure | Behaviour |
|---|---|
| WS drops | Rust reconnects with jittered backoff (0.5 s → 30 s), resubscribes everything, emits `PC_EV_CONN`. UI greys the affected panel and shows a reconnecting badge. |
| WS silent (no msg > 30 s) | treated as dead; force reconnect. |
| Gap after reconnect | No sequence numbers exist, so gaps are undetectable (03 §W6). Rust resubscribes; `userFills` and `clearinghouseState` arrive with snapshots that backfill. `orderUpdates` has **no** snapshot, so the engine additionally fetches `openOrders` and `userFillsByTime(since=last_seen)` and reconciles. |
| Order ack never arrives | order → `Unknown`, reconciliation fetch, UI shows it amber. Never auto-resend. |
| Rate limited (429) | Rust queues and retries with backoff; `PC_EV_RATE` drives a visible budget meter; risk layer starts rejecting new orders at <10 % budget. |
| Book stale | Detected three ways (03 §W6): `now - data.time` beyond 3x the feed's expected cadence, repeated byte-identical payloads, and `l2Book` touch diverging from `bbo` for longer than the coalescing window. Sets `PC_F_STALE`; the ticket disables market orders, since the slippage estimate is no longer trustworthy. |
| parsec crashes | `scheduleCancel` deadline expires within 30 s and the venue cancels every resting order. |
| Clock skew | nonce is server-time-anchored; on `nonce too old/new` errors, resync offset from a venue timestamp and retry once. |

---

## 9. Explicit non-goals for v1

Perps only (no spot, no vaults, no TWAP/scale/chase order types), one account, no
multi-account, no strategy automation beyond the dead man's switch, no historical trade
analytics beyond what the venue returns. Each is a clean addition later precisely because the
engine is headless and the UI is a thin reader.
