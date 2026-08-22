# Parsec — implementation plan

Build order for an agent implementing this project. Each phase ends in something you can run
and judge. **Do not start a phase until the previous phase's acceptance criteria pass** — the
phases are ordered so that each one's bugs are cheap to find, and skipping ahead moves them
somewhere expensive.

Reference docs: architecture `02`, exchange facts `03`, Rust layer `04`, UI `05`,
security `06`, testing `08`.

**Standing rules for every phase**
- Testnet only until Phase 7 explicitly enables mainnet.
- **Public endpoints only** (D8): `https://api.hyperliquid-testnet.xyz/{info,exchange}` and
  `wss://api.hyperliquid-testnet.xyz/ws`. No node, no `order_book_server`, no third-party
  gateway anywhere in v1.
- Every numeric that came off the wire is a decimal **string** — parse to fixed-point `int64`,
  never `double` (03 §W8).
- No allocation on the market-data path; no locks in the engine; no strings on hot paths.
- Every phase adds its tests in the same commit as the code, not after.

---

## Phase 0 — Repo skeleton and build system

**Goal.** `cmake --build build && ./build/parsec` opens an empty ImGui window, with a Rust
staticlib already linked and called once. This phase is entirely about proving the two
toolchains join correctly, which is the only genuinely novel build risk in the project.

**Delete first.** `src/order_book/` and the old `src/main.cpp` (decision D1). They are already
removed in the working tree — commit the deletion so the tree and index agree.

**Files**
```
CMakeLists.txt
cmake/Dependencies.cmake
rust/Cargo.toml
rust/src/lib.rs                 # one fn: pc_abi_version() -> u32
include/parsec/parsec.h         # just the version fn for now
src/main.cpp                    # GLFW + OpenGL3 + ImGui, asserts pc_abi_version() == 1
.gitignore                      # /build, .parsec, keystore*
```

**Notes**
- `corrosion_import_crate(MANIFEST_PATH rust/Cargo.toml)`, `crate-type = ["staticlib"]`.
- macOS needs `-framework Security -framework CoreFoundation` and `-framework SystemConfiguration`
  on the link line for the Rust TLS stack, plus `c++`/`m`. Linux needs `pthread`, `dl`, `m`.
- **CMake 4.1 rejects `cmake_minimum_required(VERSION <3.5)`**, which several FetchContent
  dependencies still declare. Set `CMAKE_POLICY_VERSION_MINIMUM=3.5` for those subprojects
  rather than patching them.
- Pin every FetchContent dependency to a tag or commit SHA, never a branch.

**Acceptance**
- Clean clone → configure → build → run, on macOS and Linux, with no manual steps.
- `pc_abi_version()` returns from Rust into C++ and the assert passes.
- Window opens, closes cleanly, and an idle window uses **<2 % CPU** (event-driven redraw is
  in from the start, not retrofitted — see 05).

---

## Phase 1 — Rust market-data edge and the FFI spine

**Goal.** A headless C++ engine that subscribes to BTC and prints a live book, BBO, and trade
tape to stdout. No UI, no keys, no signing. This is where the FFI contract gets fixed.

**Files**
```
include/parsec/parsec.h          # full event/command ABI (02 §5)
rust/src/ffi/{mod,types,queue}.rs
rust/src/transport/{http,ws,backoff}.rs
rust/src/codec/{info,ws_msg,decimal}.rs
src/core/{units,time,spsc_ring,seqlock,log}.hpp
src/md/{l2_book,bbo,trade_tape,candle_series,asset_ctx,market_store}.{hpp,cpp}
src/app/engine.{hpp,cpp}
src/main.cpp                     # engine only, console output
```

**Notes**
- Build the ABI header **first** and treat it as frozen for the phase. Add the
  `size_of`/`offset_of` assertions on both sides in this phase (02 §5.5) — they cost minutes
  now and save a day of memory-corruption debugging later.
- `codec::decimal` is the load-bearing piece: `"113377.0"` → `int64` at 1e8, exact, no float
  intermediate. Test it against every shape the venue emits, including `"0"`, `"-0"`,
  `"0.00001"`, and values with more than 8 decimals (which must be rejected, not truncated).
- Subscribe per 03 §W9: `bbo`, `l2Book` (`fast:true`), `trades`, `activeAssetCtx`.
- WS manager owns: 50 s ping timer, 60 s server-idle detection, jittered reconnect, full
  resubscribe on reconnect, and the **30-trade backfill dedupe** on `trades` (03 §W2.0).
- Guard against the legacy non-JSON greeting frame `"Websocket connection established."`.

**Acceptance**
- Book, BBO, and tape stream for 30 minutes with zero panics and zero allocations after
  warm-up (verify with a heap profiler or an allocation counter hook).
- Killing the network for 60 s and restoring it produces a clean reconnect and resubscribe,
  visible in the log, with no duplicate trades after recovery.
- Measured `bbo` message rate is within the expected band (03 §W3). **Re-measure default
  `l2Book` cadence from your own region here** and record it — the 5.25 s figure in 03 came
  from a different network vantage point and the design's staleness thresholds depend on it.
- `pc_poll` with a 500 µs timeout leaves the engine thread near 0 % CPU when idle.

---

## Phase 2 — Read-only terminal (first real milestone)

**Goal.** A usable market-data terminal: dockspace, order book, candle chart with timeframes,
instrument strip. Still no keys, no orders. At the end of this phase parsec is already worth
opening.

**Files**
```
src/ui/app_window.{hpp,cpp}      # GLFW/GL3/ImGui bootstrap, dockspace, theme
src/ui/theme.cpp                 # dark palette matching the reference
src/ui/panel_book.cpp
src/ui/panel_chart.cpp
src/ui/panel_trades.cpp
src/ui/panel_instruments.cpp     # top strip: mark, oracle, 24h change, vol, OI, funding
src/ui/widgets/{depth_bar,number_fmt,candle_renderer}.{hpp,cpp}
src/app/ui_bridge.{hpp,cpp}      # seqlock snapshots + SPSC command ring
```

**Notes**
- Chart backfill from REST `candleSnapshot` (max 5000 candles), then the `candle` subscription
  keeps the in-progress bucket live. `CandleSeries::apply` overwrites the last bucket while
  `t` matches and appends when it advances.
- Timeframe switch must not refetch what is already cached; keep one `CandleSeries` per
  (coin, interval) and fetch lazily on first open.
- Book panel aggregation dropdown maps to `nSigFigs`/`mantissa` — **display only**, wired
  nowhere near execution (03 §W3).
- Number formatting is its own tested unit: prices render at the venue's own precision for
  that asset, sizes at `szDecimals`, USD with thousands separators. Getting this wrong makes
  every screenshot look amateur and, worse, makes fat-finger errors easier to miss.

**Acceptance**
- Chart renders 5000 candles at 60 fps with pan/zoom smooth; switching timeframes is instant
  once cached.
- Book updates visibly and never flickers or tears (proves the seqlock is correct).
- Idle CPU **<5 %** with three panels live.
- Leaving it open for an hour shows no memory growth and no drift in the in-progress candle.

---

## Phase 3 — Identity: keystore, agent wallet, signing

**Goal.** parsec authenticates and reads the account. Positions and balances render. Still
**no order placement** — the signing path is proven on read-only and account-state operations
first, because a signing bug that first appears on a live order is expensive.

**Files**
```
rust/src/signer/{l1,user_signed,keystore,agent}.rs
rust/src/session.rs              # nonce allocator, rate budget, master-vs-agent addressing
src/app/config.{hpp,cpp}
src/ui/panel_positions.cpp
src/ui/panel_balances.cpp
src/ui/dialog_unlock.cpp
tests/rust/signing_vectors.rs
```

**Notes — this is the phase most likely to burn a day, so front-load the verification**
- Implement `sign_l1_action` against the exact byte recipe in 03 §4a and test it against
  **known vectors from the Python SDK's own test suite before ever hitting the network.** A
  signature that is wrong produces `User or API Wallet 0x... does not exist` — a message that
  tells you nothing about *why*. Offline vectors turn a guessing game into a unit test.
- msgpack field order is part of the hash. Encode actions through explicit ordered structs, not
  a generic map. **`f: false` and `a: false` must be omitted entirely**, not serialized (03 §4a).
- `source = "a"` mainnet, `"b"` testnet — drive this from the resolved base URL, and assert it
  matches the configured network so a proxy cannot silently flip you.
- Nonce allocator: monotonic, ms-based, per-agent, fast-forwardable. One agent per process.
- **Query with the master address, sign with the agent key.** Wire the two addresses into
  distinct types (`MasterAddress`, `AgentAddress`) so they cannot be swapped by accident —
  the failure mode is an empty account, which looks like "no positions" rather than an error.
- `parsec setup` and `parsec setup --print-approval` per 06 §2, plus the `--signature` path.

**Acceptance**
- Signing unit tests pass offline against Python-SDK vectors.
- `approveAgent` succeeds on testnet; keystore round-trips; wrong passphrase fails cleanly.
- Positions and balances match the Hyperliquid web UI **exactly**, including unrealized P&L,
  margin used, and liquidation price, for both a cross and an isolated position.
- **Run the agent-permission test** (06 §1): attempt `withdraw3` with the agent key and confirm
  it is rejected. This converts the security model from an inference into a verified fact.
- Restart with the keystore locked → unlock → full account state within 2 s.

---

## Phase 4 — Order entry (second real milestone)

**Goal.** Place, cancel, and modify orders from the ticket. Leverage and margin mode. TP/SL.
At the end of this phase parsec can trade.

**Files**
```
src/exec/{rounder,order_router,order_state,slippage}.{hpp,cpp}
src/risk/{pre_trade,rate_budget}.{hpp,cpp}
src/ui/panel_ticket.cpp
src/ui/panel_open_orders.cpp
rust/src/codec/exchange.rs
tests/cpp/test_rounder.cpp
```

**Notes**
- **`exec::Rounder` before anything else in this phase**, with exhaustive tests. The rule is a
  *conjunction*: ≤5 significant figures **and** ≤`6 - szDecimals` decimals, with integers
  always allowed (03 §6). Implement in integer/decimal arithmetic. The Python SDK's version
  runs through a float with banker's rounding; mirroring its *stages* is right, mirroring its
  float behaviour is not.
- Market orders are IOC at a slippage-adjusted price (03 §"Market orders"). Reference price is
  `allMids`-equivalent — use the `bbo` mid, which is fresher. Show the modelled fill from
  `L2Book::sweep()` in the ticket **before** the button is clickable.
- TP/SL uses `grouping: "normalTpsl"` with reduce-only children of opposite side and equal
  size. Non-reduce-only children are rejected in *pre-validation*, as one error for the whole
  batch — handle that shape (03 §8).
- **Response handling is the classic trap**: `status: "ok"` is not success. Walk
  `response.data.statuses[]`. Over WS, a rejected order arrives as `type:"action"` with
  `payload.status == "err"`, **not** `type:"error"` (03 §W4).
- Every order carries a cloid. Every inflight post has a client-side timeout, because a
  malformed request never produces a `post` envelope at all (03 §W4) — an id-keyed map without
  a timeout leaks forever.
- On ack timeout: mark `Unknown`, force reconciliation, **never auto-resend**.

**Acceptance**
- On testnet: place/cancel a resting limit order; place a market order and see the fill; attach
  TP/SL and watch a filled child cancel its sibling with `siblingFilledCanceled`.
- Every risk check rejects with a message naming the check and the margin by which it failed.
- Rounder tests cover `szDecimals` 0–6, prices from 0.0001 to 500000, exact-5-sig-fig
  boundaries, and integer prices.
- Deliberately send an order below $10 notional and confirm the error surfaces in the UI as a
  clear toast rather than a silent no-op.

---

## Phase 5 — Portfolio, reconciliation, and the safety net

**Goal.** parsec can be left open with money on it. This phase is entirely about the
difference between a demo and a tool you would actually trust.

**Files**
```
src/portfolio/{position_book,account_state,pnl,reconciler}.{hpp,cpp}
src/risk/{kill_switch,dead_mans_switch}.{hpp,cpp}
src/ui/panel_fills.cpp
src/ui/panel_funding.cpp
src/ui/panel_order_history.cpp
src/ui/status_bar.cpp            # connection, staleness, rate budget, DMS countdown
```

**Notes**
- `scheduleCancel` heartbeat: refresh the deadline every ~10 s to a point 30 s out. Constraints
  are real — `time` must be ≥5 s in the future, and **only 10 triggers per UTC day** (03
  §`scheduleCancel`), so the *trigger* is an emergency backstop, not a routine occurrence.
- Reconciler compares optimistic local state against the `clearinghouseState` /`openOrders`
  snapshots (~4 s cadence). Divergence beyond tolerance → snap to venue, log both values, and
  raise a visible alarm on the second consecutive occurrence.
- Staleness detection per 03 §W6, all four signals: `data.time` skew, duplicate payloads,
  `bbo`-vs-`l2Book` divergence, and missed pongs. Surface it in the status bar as a real
  indicator, not a hidden log line.
- Kill switch: one hotkey → cancel all, optionally flatten, refuse new orders until re-armed.
  Confirm-on-flatten, no confirm on cancel-all.
- Fee accounting: `fee` already includes `builderFee`; adding them double-counts (03 §W2.9).
  `crossed` is the maker/taker flag.

**Acceptance**
- Kill parsec with orders resting → the venue cancels them within 30 s. **Test this for real**,
  it is the single most important safety property in the system.
- Force a divergence (cancel an order from the web UI) and watch the reconciler detect, log,
  and correct it.
- Pull the network mid-session: status bar goes stale within the threshold, market orders
  disable, reconnect restores full state with no phantom positions.
- P&L, funding, and fees match the venue's own numbers over a multi-day testnet session.

---

## Phase 6 — Polish

**Goal.** The details that make it feel like the reference screenshot rather than a prototype.

- Click a book level → price into the ticket. Drag a chart order line → modify.
- Chart overlays: entry price, liquidation price, resting orders, TP/SL levels, fill markers.
- Hotkeys: buy/sell, cancel all, flatten, timeframe cycling, instrument switching.
- Multi-instrument watchlist with the top strip live across all of them.
- Layout, watchlist, and preferences persisted to `~/.parsec/ui.json`.
- Session recorder: dump raw WS frames to disk for replay (feeds the deterministic tests in
  `08`, and is the foundation if the backtester idea ever returns).

**Acceptance.** Side-by-side with the reference UI, the same information is available in the
same places, and every number agrees.

---

## Phase 7 — Mainnet enablement

Not a code phase so much as a gate. Before the mainnet flag is allowed to flip:

1. Every prior acceptance criterion passes on testnet.
2. A full week of continuous testnet uptime with no unexplained reconciliation divergence.
3. The dead-man's-switch test has been run and passed at least twice.
4. The agent-cannot-withdraw test (06 §1) has been run against **mainnet's** rules on a fresh
   agent with a trivial balance.
5. Mainnet requires the config flag *and* a typed confirmation; the header badge shows the
   network in a distinct colour at all times.
6. First mainnet session is capped by risk limits set to roughly 1 % of intended size, raised
   only after a week of correct behaviour.

---

## Sequencing rationale

Two orderings here are deliberate and worth preserving if the plan gets rearranged.

**Signing is proven in Phase 3, before any order can be placed in Phase 4.** Hyperliquid's
response to a bad signature is a message about a nonexistent wallet — it identifies neither the
cause nor the field. Discovering that while also debugging order semantics conflates two
independent problems. Offline test vectors first, then account reads, then orders.

**The safety net is Phase 5, before polish.** It is tempting to sequence the dead-man's switch
and reconciler last, since neither is visible. But Phase 4 is the point where parsec can lose
money unattended, and the gap between "can place orders" and "can be trusted to hold them"
should be as short as possible.
